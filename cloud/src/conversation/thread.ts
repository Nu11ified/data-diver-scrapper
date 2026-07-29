
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
  type Graph,
  type Subject,
  type TraceStep,
  type TreeDoc,
} from "../decision/graph.ts";
import { calibratedStates, estimate, holdoutError, isCalibrated } from "../valuation.ts";
import {
  classifyOwner,
  formatInZone,
  nextSendWindow,
  zoneFor,
  type Audience,
} from "../outreach/schedule.ts";

export interface PropertyMatch {
  readonly propertyKey: string;
  readonly address: string;
  readonly owner: string;
  readonly mailing: string;
  readonly phone: string;
  readonly email: string;
  readonly lifecycleState: string;
  readonly owed: number;
  readonly assessed: number;
  readonly debtToValue: number;
  readonly violations: number;
  readonly sources: number;
  readonly signals: Readonly<Record<string, number>>;
}

export const templateDraft = (match: PropertyMatch): string =>
  `Hi ${match.owner.split(",")[0] ?? "there"}, I'm reaching out about the ` +
  `property at ${match.address}. Would you be open to discussing an offer?`;

/// What the property is worth, or an honest refusal. The model only holds for
/// states whose assessment ratio it learned; elsewhere it would be
/// extrapolating across assessment law, which failed its own holdout.
export const worthSummary = (match: PropertyMatch): string => {
  const state = (match.propertyKey.split("|")[0] ?? "").slice(-2).toUpperCase();
  if (!isCalibrated(state)) {
    return match.assessed > 0
      ? `Assessed at ${money(match.assessed)} by the county. No estimate: ` +
          `${state || "this state"} is not one of the states this model was ` +
          `calibrated on (${calibratedStates().join(", ")}).`
      : "No assessed value on record, so no estimate.";
  }
  const valued = estimate({
    state,
    assessed: match.assessed,
    improvementValue: match.signals.improvement_value ?? 0,
    landValue: match.signals.land_value ?? 0,
    sqft: match.signals.living_area ?? match.signals.building_area ?? 0,
    yearBuilt: match.signals.year_built ?? 0,
    landArea: match.signals.land_area ?? 0,
    use: match.lifecycleState,
    year: new Date().getUTCFullYear(),
  });
  if (valued === undefined) {
    return "No assessed value on record, so no estimate.";
  }
  // Deliberately not called a confidence interval. The number is a median
  // absolute error, so by construction about half of past sales missed by
  // more than this; quoting it as a range would promise coverage it has
  // never been measured to have.
  return (
    `Estimated worth: ${money(valued.estimate)}, against ` +
    `${money(match.assessed)} assessed.\n` +
    `Half of past sales in jurisdictions this model never trained on landed ` +
    `within ${(holdoutError() * 100).toFixed(0)}% of its estimate; the other ` +
    `half missed by more. Not a confidence interval.`
  );
};

export const contactSummary = (match: PropertyMatch): string => {
  const lines = [
    match.mailing !== "" ? `Mailing address: ${match.mailing}` : "",
    match.phone !== "" ? `Phone (from county records): ${match.phone}` : "",
    match.email !== "" ? `Email (from county records): ${match.email}` : "",
  ].filter((line) => line !== "");
  return lines.length > 0
    ? lines.join("\n")
    : "No contact details in county records beyond the owner name.";
};

export interface Turn {
  readonly role: "user" | "scout";
  readonly text: string;
  readonly at: string;
}

/// Only these bypass the model. Everything a person would actually say is
/// conversation, and routing it to a keyword table is how a demo answers "Hi"
/// with a menu.
const SLASH_COMMANDS = [
  "connect",
  "login",
  "logout",
  "disconnect",
  "reset",
  "reset-account",
  "delete",
  "delete-account",
  "help",
  "status",
];

export const slashCommand = (text: string): string => {
  const trimmed = text.trim().toLowerCase();
  if (!trimmed.startsWith("/")) return "";
  const word = trimmed.slice(1).split(/\s+/)[0] ?? "";
  const normalized = word.replace(/_/g, "-");
  return SLASH_COMMANDS.includes(normalized) ? normalized : "";
};

export const isDeterministicCommand = (text: string): boolean =>
  slashCommand(text) !== "";


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

export interface DraftRequest {
  readonly match: PropertyMatch;
  readonly explanation: string;
  readonly requiresApproval: boolean;
}

export interface OutreachOrder {
  readonly propertyKey: string;
  readonly draft: string;
  readonly audience: Audience;
  readonly scheduledForIso: string;
  readonly approved: boolean;
}

export interface HandleOutcome {
  readonly reply: string;
  readonly tree?: TreeDoc;
  readonly draftRequest?: DraftRequest;
  readonly outreach?: OutreachOrder;
  readonly evaluations: readonly PropertyEvaluation[];
}

const TREE_NAME = "acquisition";
const TURN_WINDOW = 24;
const TURNS_KEPT_AFTER_COMPACTION = 8;
const SUMMARY_LIMIT = 2_400;

const money = (value: number): string =>
  `$${Math.round(value).toLocaleString("en-US")}`;

export const subjectOf = (match: PropertyMatch): Subject => ({
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

export interface ThreadSnapshot {
  readonly tree: TreeDoc;
  readonly summary: string;
  readonly recentTurns: readonly Turn[];
  readonly configured: boolean;
  readonly county: string;
}

export interface ApplyScoutInput {
  readonly userText: string;
  readonly reply: string;
  readonly graph?: Graph;
  readonly county?: string;
}

export interface AttachDraftInput {
  readonly userText: string;
  readonly match: PropertyMatch;
  readonly explanation: string;
  readonly draft: string;
  readonly requiresApproval: boolean;
}

export interface ThreadShape {
  readonly forget: () => Effect.Effect<void, never, RuntimeContextInterface>;
  readonly handleMessage: (
    input: HandleMessageInput,
  ) => Effect.Effect<HandleOutcome, never, RuntimeContextInterface>;
  readonly snapshot: () => Effect.Effect<ThreadSnapshot, never, RuntimeContextInterface>;
  readonly applyScout: (
    input: ApplyScoutInput,
  ) => Effect.Effect<HandleOutcome, never, RuntimeContextInterface>;
  readonly attachDraft: (
    input: AttachDraftInput,
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

      const loadTree = Effect.gen(function* () {
        const stored = yield* state.storage.get<TreeDoc>("tree");
        if (stored !== undefined) return { tree: stored, created: false };
        const legacy = yield* state.storage.get<CriteriaSpec>("criteria");
        const tree = compileSpec(legacy ?? DEFAULT_SPEC, TREE_NAME, 1);
        yield* state.storage.put("tree", tree);
        return { tree, created: true };
      });

      return {
        /// Everything this thread remembers: the tree, the turns, the folded
        /// summary, the pending step and the county. Clearing the rows in
        /// Postgres without this leaves the object still holding a compacted
        /// history, so the next message would not look like a first one.
        forget: () =>
          Effect.gen(function* () {
            for (const key of ["tree", "criteria", "turns", "summary", "pending", "county"]) {
              yield* state.storage.delete(key);
            }
          }),

        handleMessage: (input: HandleMessageInput) =>
          Effect.gen(function* () {
            const loaded = yield* loadTree;
            let tree: TreeDoc = loaded.tree;
            let changedTree: TreeDoc | undefined = loaded.created ? loaded.tree : undefined;

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
              outreach?: OutreachOrder,
            ) =>
              Effect.gen(function* () {
                yield* state.storage.put("pending", nextPending);
                yield* record(userText, reply, tree, turns, summary);
                const toPersist = changedTree ?? (evaluations.length > 0 ? tree : undefined);
                return {
                  reply,
                  evaluations,
                  ...(toPersist === undefined ? {} : { tree: toPersist }),
                  ...(outreach === undefined ? {} : { outreach }),
                } satisfies HandleOutcome;
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
                const match = pending.match;
                const audience = classifyOwner(match.owner);
                const timeZone = zoneFor(match.propertyKey.split("|")[0] ?? "");
                const window = nextSendWindow(audience, new Date(), timeZone);
                const sent =
                  `Approved. Outreach to ${match.owner || "the recorded owner"} is ` +
                  `scheduled for ${formatInZone(window.at, timeZone)} - ${window.reason}.\n` +
                  `(Demo mode: this message will NOT actually be sent. No real ` +
                  `outreach leaves this system.)\n\n` +
                  `Reply REVIEW to see remaining matches.`;
                return yield* respond(text, sent, { kind: "idle" }, [], {
                  propertyKey: match.propertyKey,
                  draft: pending.draft,
                  audience,
                  scheduledForIso: window.at.toISOString(),
                  approved: true,
                });
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
              const explanation =
                `${match.address} — recorded owner: ${match.owner || "unknown"}.\n` +
                `${contactSummary(match)}\n\n` +
                `${worthSummary(match)}\n\n` +
                `Why it matched (criteria v${tree.version}):\n${explainTrace(evaluated.trace)}`;
              const codexConnected =
                input.codexAccount !== undefined && input.codexAccount !== "";
              if (codexConnected) {
                // The worker drafts through Codex, then finalizes via attachDraft.
                return {
                  reply: "",
                  evaluations: [],
                  draftRequest: {
                    match,
                    explanation,
                    requiresApproval: evaluated.requiresApproval,
                  },
                  ...(changedTree === undefined ? {} : { tree: changedTree }),
                } satisfies HandleOutcome;
              }
              const draft = templateDraft(match);
              const body = `${explanation}\n\nDraft (simulated outreach):\n"${draft}"`;
              if (!evaluated.requiresApproval) {
                const audience = classifyOwner(match.owner);
                const timeZone = zoneFor(match.propertyKey.split("|")[0] ?? "");
                const window = nextSendWindow(audience, new Date(), timeZone);
                return yield* respond(
                  text,
                  `${body}\n\nYour decision tree has no approval gate: outreach ` +
                    `scheduled for ${formatInZone(window.at, timeZone)} - ${window.reason}.\n` +
                    `(Demo mode: this message will NOT actually be sent.)`,
                  { kind: "idle" },
                  [],
                  {
                    propertyKey: match.propertyKey,
                    draft,
                    audience,
                    scheduledForIso: window.at.toISOString(),
                    approved: false,
                  },
                );
              }
              return yield* respond(
                text,
                `${body}\n\nReply APPROVE to schedule it, or REJECT to discard.`,
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

        snapshot: () =>
          Effect.gen(function* () {
            const loaded = yield* loadTree;
            const turns = (yield* state.storage.get<readonly Turn[]>("turns")) ?? [];
            const summary = (yield* state.storage.get<string>("summary")) ?? "";
            const county = (yield* state.storage.get<string>("county")) ?? "norfolk";
            return {
              tree: loaded.tree,
              summary,
              recentTurns: turns.slice(-8),
              configured: loaded.tree.version > 1,
              county,
            };
          }),

        attachDraft: (input: AttachDraftInput) =>
          Effect.gen(function* () {
            const loaded = yield* loadTree;
            const tree = loaded.tree;
            const changedTree = loaded.created ? loaded.tree : undefined;
            const turns = (yield* state.storage.get<readonly Turn[]>("turns")) ?? [];
            const summary = (yield* state.storage.get<string>("summary")) ?? "";
            const body =
              `${input.explanation}\n\nDraft (simulated outreach):\n"${input.draft}"`;
            let reply: string;
            let nextPending: Pending;
            let outreach: OutreachOrder | undefined;
            if (input.requiresApproval) {
              reply = `${body}\n\nReply APPROVE to schedule it, or REJECT to discard.`;
              nextPending = {
                kind: "approve_outreach",
                match: input.match,
                draft: input.draft,
              };
            } else {
              const audience = classifyOwner(input.match.owner);
              const timeZone = zoneFor(input.match.propertyKey.split("|")[0] ?? "");
              const window = nextSendWindow(audience, new Date(), timeZone);
              reply =
                `${body}\n\nYour decision tree has no approval gate: outreach ` +
                `scheduled for ${formatInZone(window.at, timeZone)} - ${window.reason}.\n` +
                `(Demo mode: this message will NOT actually be sent.)`;
              nextPending = { kind: "idle" };
              outreach = {
                propertyKey: input.match.propertyKey,
                draft: input.draft,
                audience,
                scheduledForIso: window.at.toISOString(),
                approved: false,
              };
            }
            yield* state.storage.put("pending", nextPending);
            yield* record(input.userText, reply, tree, turns, summary);
            return {
              reply,
              evaluations: [],
              ...(changedTree === undefined ? {} : { tree: changedTree }),
              ...(outreach === undefined ? {} : { outreach }),
            } satisfies HandleOutcome;
          }),

        applyScout: (input: ApplyScoutInput) =>
          Effect.gen(function* () {
            const loaded = yield* loadTree;
            let tree: TreeDoc = loaded.tree;
            let changedTree: TreeDoc | undefined = loaded.created ? loaded.tree : undefined;
            if (input.graph !== undefined) {
              tree = { name: tree.name, version: tree.version + 1, graph: input.graph };
              yield* state.storage.put("tree", tree);
              changedTree = tree;
            }
            if (input.county !== undefined && input.county !== "") {
              yield* state.storage.put("county", input.county);
            }
            const turns = (yield* state.storage.get<readonly Turn[]>("turns")) ?? [];
            const summary = (yield* state.storage.get<string>("summary")) ?? "";
            yield* record(input.userText, input.reply, tree, turns, summary);
            const outcome: HandleOutcome =
              changedTree === undefined
                ? { reply: input.reply, evaluations: [] }
                : { reply: input.reply, evaluations: [], tree: changedTree };
            return outcome;
          }),
      };
    });
  }),
);
