
import type { RuntimeContextInterface } from "alchemy";
import * as Cloudflare from "alchemy/Cloudflare";
import * as Effect from "effect/Effect";

export interface PropertyMatch {
  readonly propertyKey: string;
  readonly address: string;
  readonly owner: string;
  readonly lifecycleState: string;
  readonly owed: number;
  readonly assessed: number;
  readonly debtToValue: number;
  readonly violations: number;
  readonly sources: number;
}

export interface Criteria {
  readonly minOwed: number;
  readonly requireMultiSource: boolean;
  readonly minDebtToValue: number;
}

interface Turn {
  readonly role: "user" | "scout";
  readonly text: string;
  readonly at: string;
}

type Pending =
  | { readonly kind: "review"; readonly matches: readonly PropertyMatch[] }
  | {
      readonly kind: "approve_outreach";
      readonly match: PropertyMatch;
      readonly draft: string;
    }
  | null;

const DEFAULT_CRITERIA: Criteria = {
  minOwed: 10_000,
  requireMultiSource: false,
  minDebtToValue: 0,
};

const TURN_WINDOW = 24;
const TURNS_KEPT_AFTER_COMPACTION = 8;
const SUMMARY_LIMIT = 2_400;

const money = (value: number): string =>
  `$${Math.round(value).toLocaleString("en-US")}`;

const describeMatch = (match: PropertyMatch, index: number): string => {
  const lines = [
    `${index + 1}. ${match.address}`,
    `   Owner: ${match.owner || "unknown"}`,
    `   Owed: ${money(match.owed)}${match.assessed > 0 ? ` against ${money(match.assessed)} assessed` : ""}`,
  ];
  if (match.debtToValue > 0) lines.push(`   Debt to value: ${match.debtToValue.toFixed(1)}x`);
  if (match.violations > 0) lines.push(`   Open violations: ${match.violations}`);
  lines.push(`   Corroborated by ${match.sources} source${match.sources === 1 ? "" : "s"}`);
  return lines.join("\n");
};

const explainMatch = (match: PropertyMatch, criteria: Criteria): string => {
  const reasons = [`✓ ${money(match.owed)} delinquent (your floor is ${money(criteria.minOwed)})`];
  if (match.assessed > 0) {
    reasons.push(`✓ Assessed at ${money(match.assessed)} by the county assessor`);
  }
  if (match.violations > 0) reasons.push(`✓ ${match.violations} open code violation(s)`);
  if (match.sources > 1) reasons.push(`✓ Confirmed by ${match.sources} independent county sources`);
  return reasons.join("\n");
};

export interface HandleMessageInput {
  readonly text: string;
  readonly candidates: readonly PropertyMatch[];
}

export interface ThreadShape {
  readonly handleMessage: (
    input: HandleMessageInput,
  ) => Effect.Effect<string, never, RuntimeContextInterface>;
}

export class ConversationThread extends Cloudflare.DurableObject<
  ConversationThread,
  ThreadShape
>()("ConversationThread") {}

export const ConversationThreadLive = ConversationThread.make<never>(
  Effect.gen(function* () {
    const state = yield* Cloudflare.DurableObjectState;

    return Effect.gen(function* () {
      const record = (
        userText: string,
        reply: string,
        criteria: Criteria,
        turns: readonly Turn[],
        summary: string,
      ) =>
        Effect.gen(function* () {
          const now = new Date().toISOString();
          let nextTurns: readonly Turn[] = [
            ...turns,
            { role: "user", text: userText, at: now },
            { role: "scout", text: reply, at: now },
          ];
          let nextSummary = summary;
          if (nextTurns.length > TURN_WINDOW) {
            const folded = nextTurns.slice(0, -TURNS_KEPT_AFTER_COMPACTION);
            const userAsks = folded.filter((t) => t.role === "user").length;
            const paragraph =
              `[${folded[0]?.at.slice(0, 10) ?? ""}..${folded.at(-1)?.at.slice(0, 10) ?? ""}] ` +
              `${userAsks} user messages handled; criteria now: owed >= ${money(criteria.minOwed)}` +
              `${criteria.requireMultiSource ? ", multi-source only" : ""}` +
              `${criteria.minDebtToValue > 0 ? `, debt/value >= ${criteria.minDebtToValue}` : ""}.`;
            nextSummary = `${nextSummary}\n${paragraph}`.trim().slice(-SUMMARY_LIMIT);
            nextTurns = nextTurns.slice(-TURNS_KEPT_AFTER_COMPACTION);
          }
          yield* state.storage.put("turns", nextTurns);
          yield* state.storage.put("summary", nextSummary);
        });

      return {
        handleMessage: (input: HandleMessageInput) =>
          Effect.gen(function* () {
            const criteria =
              (yield* state.storage.get<Criteria>("criteria")) ?? DEFAULT_CRITERIA;
            const turns = (yield* state.storage.get<readonly Turn[]>("turns")) ?? [];
            const summary = (yield* state.storage.get<string>("summary")) ?? "";
            const pending = (yield* state.storage.get<Pending>("pending")) ?? null;

            const respond = (userText: string, reply: string, nextPending: Pending) =>
              Effect.gen(function* () {
                yield* state.storage.put("pending", nextPending);
                yield* record(userText, reply, criteria, turns, summary);
                return reply;
              });

            const text = input.text.trim();
            const lower = text.toLowerCase();

            const qualified = input.candidates.filter(
              (m) =>
                m.owed >= criteria.minOwed &&
                (!criteria.requireMultiSource || m.sources > 1) &&
                (criteria.minDebtToValue <= 0 || m.debtToValue >= criteria.minDebtToValue),
            );

            if (pending?.kind === "approve_outreach") {
              if (lower === "approve" || lower === "yes") {
                const sent =
                  `Simulated outreach sent for ${pending.match.address}.\n` +
                  `(Demo mode: no real message left this system.)\n\n` +
                  `Reply REVIEW to see remaining matches.`;
                return yield* respond(text, sent, null);
              }
              if (lower === "reject" || lower === "no") {
                return yield* respond(
                  text,
                  "Draft discarded. Reply REVIEW to see other matches.",
                  null,
                );
              }
            }

            if (pending?.kind === "review" && /^\d+$/.test(lower)) {
              const index = Number.parseInt(lower, 10) - 1;
              const match = pending.matches[index];
              if (match === undefined) {
                return yield* respond(
                  text,
                  `Reply with a number between 1 and ${pending.matches.length}.`,
                  pending,
                );
              }
              const draft =
                `Hi ${match.owner.split(",")[0] ?? "there"}, I'm reaching out about the ` +
                `property at ${match.address}. Would you be open to discussing an offer?`;
              const reply =
                `${match.address} — recorded owner: ${match.owner || "unknown"}.\n\n` +
                `Why it matched:\n${explainMatch(match, criteria)}\n\n` +
                `Draft (simulated outreach):\n"${draft}"\n\n` +
                `Reply APPROVE to simulate sending it, or REJECT to discard.`;
              return yield* respond(text, reply, {
                kind: "approve_outreach",
                match,
                draft,
              });
            }

            if (lower === "review" || lower === "yes" || lower === "scan") {
              if (qualified.length === 0) {
                return yield* respond(
                  text,
                  `No properties currently match your criteria ` +
                    `(owed >= ${money(criteria.minOwed)}` +
                    `${criteria.requireMultiSource ? ", multi-source only" : ""}). ` +
                    `Reply CRITERIA to see or change them.`,
                  null,
                );
              }
              const top = qualified.slice(0, 3);
              const reply =
                `${qualified.length} properties match your criteria. Top ${top.length}:\n\n` +
                top.map(describeMatch).join("\n\n") +
                `\n\nReply with a property number for owner and outreach.`;
              return yield* respond(text, reply, { kind: "review", matches: top });
            }

            if (lower === "criteria") {
              return yield* respond(
                text,
                `Your acquisition criteria:\n` +
                  `- Minimum owed: ${money(criteria.minOwed)}\n` +
                  `- Multi-source corroboration required: ${criteria.requireMultiSource ? "yes" : "no"}\n` +
                  `- Minimum debt/value: ${criteria.minDebtToValue > 0 ? `${criteria.minDebtToValue}x` : "none"}\n\n` +
                  `Change them like: "min owed 25000", "require multi source",  ` +
                  `"any source", "min debt ratio 0.5".`,
                pending,
              );
            }

            const minOwedMatch = /^min owed (\d+)$/.exec(lower);
            if (minOwedMatch !== null) {
              const value = Number.parseInt(minOwedMatch[1] ?? "0", 10);
              yield* state.storage.put("criteria", { ...criteria, minOwed: value });
              const remaining = input.candidates.filter((m) => m.owed >= value).length;
              return yield* respond(
                text,
                `Done: minimum owed is now ${money(value)}. ` +
                  `${remaining} properties currently qualify. Reply REVIEW to see them.`,
                null,
              );
            }
            if (lower === "require multi source") {
              yield* state.storage.put("criteria", { ...criteria, requireMultiSource: true });
              return yield* respond(
                text,
                "Done: only properties corroborated by more than one county source will match.",
                null,
              );
            }
            if (lower === "any source") {
              yield* state.storage.put("criteria", { ...criteria, requireMultiSource: false });
              return yield* respond(text, "Done: single-source properties can match again.", null);
            }
            const ratioMatch = /^min debt ratio ([0-9.]+)$/.exec(lower);
            if (ratioMatch !== null) {
              const value = Number.parseFloat(ratioMatch[1] ?? "0");
              yield* state.storage.put("criteria", { ...criteria, minDebtToValue: value });
              return yield* respond(
                text,
                `Done: minimum debt/value is now ${value}x.`,
                null,
              );
            }

            if (lower === "memory") {
              const window = turns
                .slice(-6)
                .map((t) => `${t.role}: ${t.text.split("\n")[0] ?? ""}`)
                .join("\n");
              return yield* respond(
                text,
                `Thread summary (compacted):\n${summary || "(nothing compacted yet)"}\n\n` +
                  `Recent turns:\n${window || "(none)"}`,
                pending,
              );
            }

            return yield* respond(
              text,
              `Goliath Scout here. The county scan found ${qualified.length} properties ` +
                `matching your criteria.\n\n` +
                `Commands: REVIEW, CRITERIA, MEMORY, a property number, APPROVE, REJECT.`,
              pending,
            );
          }),
      };
    });
  }),
);
