
import type { RuntimeContextInterface } from "alchemy";
import * as Cloudflare from "alchemy/Cloudflare";
import * as Effect from "effect/Effect";

import {
  DEFAULT_SPEC,
  compileSpec,
  describe,
  evaluate,
  explainTrace,
  type CriteriaSpec,
  type Subject,
  type TraceStep,
  type TreeDoc,
} from "../decision/graph.ts";

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
  readonly signals: Readonly<Record<string, number>>;
}

interface Turn {
  readonly role: "user" | "scout";
  readonly text: string;
  readonly at: string;
}

interface EvaluatedMatch {
  readonly match: PropertyMatch;
  readonly requiresApproval: boolean;
  readonly trace: readonly TraceStep[];
}

type Pending =
  | { readonly kind: "idle" }
  | { readonly kind: "review"; readonly matches: readonly EvaluatedMatch[] }
  | {
      readonly kind: "approve_outreach";
      readonly match: PropertyMatch;
      readonly draft: string;
    };

export interface PropertyEvaluation {
  readonly propertyKey: string;
  readonly outcome: "match" | "discard" | "invalid";
  readonly requiresApproval: boolean;
  readonly treeName: string;
  readonly treeVersion: number;
  readonly trace: readonly TraceStep[];
}

export interface HandleMessageInput {
  readonly text: string;
  readonly candidates: readonly PropertyMatch[];
  readonly connectUrl?: string;
  readonly codexAccount?: string;
}

export interface HandleOutcome {
  readonly reply: string;
  readonly tree?: TreeDoc;
  readonly evaluations: readonly PropertyEvaluation[];
}

const TREE_NAME = "acquisition";
const TURN_WINDOW = 24;
const TURNS_KEPT_AFTER_COMPACTION = 8;
const SUMMARY_LIMIT = 2_400;

const money = (value: number): string =>
  `$${Math.round(value).toLocaleString("en-US")}`;

const subjectOf = (match: PropertyMatch): Subject => ({
  ...match.signals,
  owed: match.owed,
  assessed: match.assessed,
  debtToValue: match.debtToValue,
  violations: match.violations,
  sources: match.sources,
});

const describeMatch = (evaluated: EvaluatedMatch, index: number): string => {
  const match = evaluated.match;
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

export interface ThreadShape {
  readonly handleMessage: (
    input: HandleMessageInput,
  ) => Effect.Effect<HandleOutcome, never, RuntimeContextInterface>;
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
        tree: TreeDoc,
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
            const spec = tree.spec;
            const criteriaText =
              spec === undefined
                ? `criteria v${tree.version} (custom decision tree)`
                : `criteria now: owed >= ${money(spec.minOwed)}` +
                  `${spec.requireMultiSource ? ", multi-source only" : ""}` +
                  `${spec.minDebtToValue > 0 ? `, debt/value >= ${spec.minDebtToValue}` : ""}`;
            const paragraph =
              `[${folded[0]?.at.slice(0, 10) ?? ""}..${folded.at(-1)?.at.slice(0, 10) ?? ""}] ` +
              `${userAsks} user messages handled; ${criteriaText}.`;
            nextSummary = `${nextSummary}\n${paragraph}`.trim().slice(-SUMMARY_LIMIT);
            nextTurns = nextTurns.slice(-TURNS_KEPT_AFTER_COMPACTION);
          }
          yield* state.storage.put("turns", nextTurns);
          yield* state.storage.put("summary", nextSummary);
        });

      return {
        handleMessage: (input: HandleMessageInput) =>
          Effect.gen(function* () {
            const stored = yield* state.storage.get<TreeDoc>("tree");
            const legacy = yield* state.storage.get<CriteriaSpec>("criteria");
            let changedTree: TreeDoc | undefined;
            let tree: TreeDoc;
            if (stored === undefined) {
              tree = compileSpec(legacy ?? DEFAULT_SPEC, TREE_NAME, 1);
              yield* state.storage.put("tree", tree);
              changedTree = tree;
            } else {
              tree = stored;
            }

            const turns = (yield* state.storage.get<readonly Turn[]>("turns")) ?? [];
            const summary = (yield* state.storage.get<string>("summary")) ?? "";
            const rawPending =
              (yield* state.storage.get<Pending>("pending")) ?? { kind: "idle" as const };
            // Pre-graph deploys stored review matches without traces; drop them.
            const pending: Pending =
              rawPending.kind === "review" && rawPending.matches.some((m) => !("match" in m))
                ? { kind: "idle" }
                : rawPending;

            const adopt = (spec: CriteriaSpec) =>
              Effect.gen(function* () {
                tree = compileSpec(spec, TREE_NAME, tree.version + 1);
                yield* state.storage.put("tree", tree);
                changedTree = tree;
              });

            const respond = (
              userText: string,
              reply: string,
              nextPending: Pending,
              evaluations: readonly PropertyEvaluation[] = [],
            ) =>
              Effect.gen(function* () {
                yield* state.storage.put("pending", nextPending);
                yield* record(userText, reply, tree, turns, summary);
                const toPersist = changedTree ?? (evaluations.length > 0 ? tree : undefined);
                const outcome: HandleOutcome =
                  toPersist === undefined
                    ? { reply, evaluations }
                    : { reply, evaluations, tree: toPersist };
                return outcome;
              });

            const evaluateAll = () =>
              input.candidates.map((match) => ({
                match,
                verdict: evaluate(tree.graph, subjectOf(match)),
              }));

            const asEvaluations = (
              pairs: ReturnType<typeof evaluateAll>,
            ): readonly PropertyEvaluation[] =>
              pairs.map(({ match, verdict }) => ({
                propertyKey: match.propertyKey,
                outcome: verdict.outcome,
                requiresApproval: verdict.outcome === "match" && verdict.requiresApproval,
                treeName: tree.name,
                treeVersion: tree.version,
                trace: verdict.trace,
              }));

            const qualifiedOf = (pairs: ReturnType<typeof evaluateAll>): EvaluatedMatch[] =>
              pairs.flatMap(({ match, verdict }) =>
                verdict.outcome === "match"
                  ? [{ match, requiresApproval: verdict.requiresApproval, trace: verdict.trace }]
                  : [],
              );

            const text = input.text.trim();
            const lower = text.toLowerCase();

            if (pending.kind === "approve_outreach") {
              if (lower === "approve" || lower === "yes") {
                const sent =
                  `Simulated outreach sent for ${pending.match.address}.\n` +
                  `(Demo mode: no real message left this system.)\n\n` +
                  `Reply REVIEW to see remaining matches.`;
                return yield* respond(text, sent, { kind: "idle" });
              }
              if (lower === "reject" || lower === "no") {
                return yield* respond(
                  text,
                  "Draft discarded. Reply REVIEW to see other matches.",
                  { kind: "idle" },
                );
              }
            }

            if (pending.kind === "review" && /^\d+$/.test(lower)) {
              const index = Number.parseInt(lower, 10) - 1;
              const evaluated = pending.matches[index];
              if (evaluated === undefined) {
                return yield* respond(
                  text,
                  `Reply with a number between 1 and ${pending.matches.length}.`,
                  pending,
                );
              }
              const match = evaluated.match;
              const draft =
                `Hi ${match.owner.split(",")[0] ?? "there"}, I'm reaching out about the ` +
                `property at ${match.address}. Would you be open to discussing an offer?`;
              const explanation =
                `${match.address} — recorded owner: ${match.owner || "unknown"}.\n\n` +
                `Why it matched (criteria v${tree.version}):\n${explainTrace(evaluated.trace)}\n\n` +
                `Draft (simulated outreach):\n"${draft}"`;
              if (!evaluated.requiresApproval) {
                return yield* respond(
                  text,
                  `${explanation}\n\nYour decision tree has no approval gate: ` +
                    `simulated outreach sent.\n(Demo mode: no real message left this system.)`,
                  { kind: "idle" },
                );
              }
              return yield* respond(
                text,
                `${explanation}\n\nReply APPROVE to simulate sending it, or REJECT to discard.`,
                { kind: "approve_outreach", match, draft },
              );
            }

            if (lower === "review" || lower === "yes" || lower === "scan") {
              const pairs = evaluateAll();
              const qualified = qualifiedOf(pairs);
              const evaluations = asEvaluations(pairs);
              if (qualified.length === 0) {
                return yield* respond(
                  text,
                  `No properties currently match your criteria (v${tree.version}):\n` +
                    `${describe(tree.graph)}\n` +
                    `Reply CRITERIA to see or change them.`,
                  { kind: "idle" },
                  evaluations,
                );
              }
              const top = qualified.slice(0, 3);
              const reply =
                `${qualified.length} properties match your criteria. Top ${top.length}:\n\n` +
                top.map(describeMatch).join("\n\n") +
                `\n\nReply with a property number for owner and outreach.`;
              return yield* respond(text, reply, { kind: "review", matches: top }, evaluations);
            }

            if (lower === "criteria") {
              return yield* respond(
                text,
                `Your acquisition criteria (v${tree.version}):\n${describe(tree.graph)}\n` +
                  `Codex: ${input.codexAccount !== undefined && input.codexAccount !== "" ? `connected as ${input.codexAccount}` : "not connected (reply CONNECT)"}\n\n` +
                  `Change them like: "min owed 25000", "require multi source", ` +
                  `"any source", "min debt ratio 0.5".`,
                pending,
              );
            }

            const minOwedMatch = /^min owed (\d+)$/.exec(lower);
            const wantsSpecEdit =
              minOwedMatch !== null ||
              lower === "require multi source" ||
              lower === "any source" ||
              /^min debt ratio ([0-9.]+)$/.test(lower);
            const baseSpec = tree.spec;
            if (wantsSpecEdit && baseSpec === undefined) {
              return yield* respond(
                text,
                `Your criteria are a custom decision tree (v${tree.version}), so the ` +
                  `shorthand commands do not apply. Describe the change in plain ` +
                  `language instead.`,
                pending,
              );
            }
            if (minOwedMatch !== null && baseSpec !== undefined) {
              const value = Number.parseInt(minOwedMatch[1] ?? "0", 10);
              yield* adopt({ ...baseSpec, minOwed: value });
              const remaining = qualifiedOf(evaluateAll()).length;
              return yield* respond(
                text,
                `Done: minimum owed is now ${money(value)} (criteria v${tree.version}). ` +
                  `${remaining} properties currently qualify. Reply REVIEW to see them.`,
                { kind: "idle" },
              );
            }
            if (lower === "require multi source" && baseSpec !== undefined) {
              yield* adopt({ ...baseSpec, requireMultiSource: true });
              return yield* respond(
                text,
                `Done: only properties corroborated by more than one county source ` +
                  `will match (criteria v${tree.version}).`,
                { kind: "idle" },
              );
            }
            if (lower === "any source" && baseSpec !== undefined) {
              yield* adopt({ ...baseSpec, requireMultiSource: false });
              return yield* respond(
                text,
                `Done: single-source properties can match again (criteria v${tree.version}).`,
                { kind: "idle" },
              );
            }
            const ratioMatch = /^min debt ratio ([0-9.]+)$/.exec(lower);
            if (ratioMatch !== null && baseSpec !== undefined) {
              const value = Number.parseFloat(ratioMatch[1] ?? "0");
              yield* adopt({ ...baseSpec, minDebtToValue: value });
              return yield* respond(
                text,
                `Done: minimum debt/value is now ${value}x (criteria v${tree.version}).`,
                { kind: "idle" },
              );
            }

            if (lower === "connect" || lower === "connect codex") {
              if (input.connectUrl !== undefined && input.connectUrl !== "") {
                return yield* respond(
                  text,
                  `Open this link to connect your ChatGPT account to Goliath Scout:\n` +
                    `${input.connectUrl}\n\n` +
                    `It is single-use and expires in 15 minutes. Your tokens are ` +
                    `stored encrypted; reply CRITERIA any time to check the connection.`,
                  pending,
                );
              }
              return yield* respond(
                text,
                "Codex connect is not available right now: the credential store " +
                  "is not configured on this deployment.",
                pending,
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

            const qualified = qualifiedOf(evaluateAll());
            return yield* respond(
              text,
              `Goliath Scout here. The county scan found ${qualified.length} properties ` +
                `matching your criteria.\n\n` +
                `Commands: REVIEW, CRITERIA, CONNECT, MEMORY, a property number, APPROVE, REJECT.`,
              pending,
            );
          }),
      };
    });
  }),
);
