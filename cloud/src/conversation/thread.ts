
import type { RuntimeContextInterface } from "alchemy";
import * as Cloudflare from "alchemy/Cloudflare";
import * as Effect from "effect/Effect";

import {
  DEFAULT_SPEC,
  SIGNAL_CATALOG,
  compileSpec,
  describe,
  evaluate,
  explainTrace,
  validateGraph,
  type CriteriaSpec,
  type Graph,
  type Subject,
  type TraceStep,
  type TreeDoc,
  type Verdict,
} from "../decision/graph.ts";
import { calibratedStates, estimate, holdoutError, isCalibrated } from "../valuation.ts";
import { linkBlock } from "../links.ts";
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

export type Pending =
  | { readonly kind: "idle" }
  | { readonly kind: "review"; readonly matches: readonly EvaluatedMatch[] }
  | {
      readonly kind: "remember_filter";
      readonly graph: Graph;
      readonly matches: readonly EvaluatedMatch[];
    }
  | {
      readonly kind: "approve_outreach";
      readonly match: PropertyMatch;
      readonly draft: string;
    }
  | { readonly kind: "approve_tree"; readonly graph: Graph };

type OnboardingStep =
  | "market"
  | "min_owed"
  | "min_assessed"
  | "evidence"
  | "recent"
  | "approval"
  | "confirm";

interface OnboardingState {
  readonly step: OnboardingStep;
  readonly county?: string;
  readonly minOwed?: number;
  readonly minAssessed?: number;
  readonly requireMultiSource?: boolean;
  readonly requireViolation?: boolean;
  readonly maxDaysSinceEvent?: number;
  readonly requireApproval?: boolean;
}

const AFFIRMATIONS = [
  "yes",
  "y",
  "yeah",
  "yep",
  "save",
  "save it",
  "keep",
  "keep it",
  "remember",
  "remember it",
];

const DECLINES = ["no", "n", "nope", "no thanks", "drop it", "forget it", "dont", "don't"];

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

const parseMoney = (text: string): number | undefined => {
  const lower = text.trim().toLowerCase();
  if (/\b(any|none|no minimum|no min)\b/.test(lower)) return 0;
  const match = lower.replace(/[$,]/g, "").match(/(\d+(?:\.\d+)?)\s*([km])?/);
  if (match === null) return undefined;
  const amount = Number.parseFloat(match[1] ?? "");
  const multiplier = match[2] === "m" ? 1_000_000 : match[2] === "k" ? 1_000 : 1;
  const value = amount * multiplier;
  return Number.isFinite(value) && value >= 0 ? value : undefined;
};

const parseMarket = (text: string): string | undefined => {
  const match = text.trim().match(/^(.+?)(?:,\s*|\s+)([A-Za-z]{2})$/);
  if (match === null) return undefined;
  const place = (match[1] ?? "").trim();
  const state = (match[2] ?? "").toUpperCase();
  return place === "" ? undefined : `${place}, ${state}`;
};

const parseChoice = (text: string): boolean | undefined => {
  const lower = text.trim().toLowerCase();
  if (/^(y|yes|yeah|yep|sure|required|require it)\b/.test(lower)) return true;
  if (/^(n|no|nope|not required|optional)\b/.test(lower)) return false;
  return undefined;
};

const parseEvidence = (
  text: string,
): { readonly requireMultiSource: boolean; readonly requireViolation: boolean } | undefined => {
  const lower = text.trim().toLowerCase();
  if (/\b(both|all)\b/.test(lower)) {
    return { requireMultiSource: true, requireViolation: true };
  }
  if (/\b(neither|none)\b/.test(lower)) {
    return { requireMultiSource: false, requireViolation: false };
  }
  const requireMultiSource = /\b(multiple|multi|two|2|sources?)\b/.test(lower);
  const requireViolation = /\b(violation|violations|code)\b/.test(lower);
  return requireMultiSource || requireViolation
    ? { requireMultiSource, requireViolation }
    : undefined;
};

const parseRecency = (text: string): number | undefined | "invalid" => {
  const lower = text.trim().toLowerCase();
  if (/\b(any|none|no limit|doesn't matter|doesnt matter)\b/.test(lower)) {
    return undefined;
  }
  const match = lower.match(/(\d+(?:\.\d+)?)\s*(day|days|month|months|year|years)?/);
  if (match === null) return "invalid";
  const amount = Number.parseFloat(match[1] ?? "");
  const unit = match[2] ?? "days";
  const days = unit.startsWith("month")
    ? amount * 30
    : unit.startsWith("year")
      ? amount * 365
      : amount;
  return Number.isFinite(days) && days > 0 ? Math.round(days) : "invalid";
};

const onboardingGraph = (state: OnboardingState): Graph => {
  const conditions = [
    {
      id: "owed_floor",
      field: "owed",
      op: "gte" as const,
      value: state.minOwed ?? 0,
    },
    ...(state.minAssessed !== undefined && state.minAssessed > 0
      ? [{
          id: "assessed_floor",
          field: "assessed",
          op: "gte" as const,
          value: state.minAssessed,
        }]
      : []),
    ...(state.requireMultiSource === true
      ? [{ id: "multiple_sources", field: "sources", op: "gte" as const, value: 2 }]
      : []),
    ...(state.requireViolation === true
      ? [{ id: "open_violation", field: "violations", op: "gte" as const, value: 1 }]
      : []),
    ...(state.maxDaysSinceEvent !== undefined
      ? [{
          id: "recent_activity",
          field: "days_since_event",
          op: "lte" as const,
          value: state.maxDaysSinceEvent,
        }]
      : []),
  ];
  const matchEntry = state.requireApproval === false ? "match" : "needs_approval";
  return {
    entry: conditions[0]?.id ?? matchEntry,
    nodes: [
      ...conditions.map((condition, index) => ({
        kind: "condition" as const,
        ...condition,
        onPass: conditions[index + 1]?.id ?? matchEntry,
        onFail: "discard",
      })),
      ...(state.requireApproval === false
        ? []
        : [{
            kind: "approval" as const,
            id: "needs_approval",
            prompt: "Outreach requires your approval",
            next: "match",
          }]),
      { kind: "action" as const, id: "match", action: "match" as const },
      { kind: "action" as const, id: "discard", action: "discard" as const },
    ],
  };
};

export const subjectOf = (match: PropertyMatch): Subject => ({
  ...match.signals,
  owed: match.owed,
  assessed: match.assessed,
  debtToValue: match.debtToValue,
  violations: match.violations,
  sources: match.sources,
});

interface EvaluatedPair {
  readonly match: PropertyMatch;
  readonly verdict: Verdict;
}

const evaluateAgainst = (
  graph: Graph,
  candidates: readonly PropertyMatch[],
): readonly EvaluatedPair[] =>
  candidates.map((match) => ({ match, verdict: evaluate(graph, subjectOf(match)) }));

const qualifiedOf = (pairs: readonly EvaluatedPair[]): readonly EvaluatedMatch[] =>
  pairs.flatMap(({ match, verdict }) =>
    verdict.outcome === "match"
      ? [{ match, requiresApproval: verdict.requiresApproval, trace: verdict.trace }]
      : [],
  );

const knownSignalsOf = (candidates: readonly PropertyMatch[]): readonly string[] => [
  ...new Set([
    ...Object.keys(SIGNAL_CATALOG),
    ...candidates.flatMap((candidate) => Object.keys(candidate.signals)),
  ]),
];

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

export interface PreviewFilterInput {
  readonly userText: string;
  readonly lead: string;
  readonly graph: Graph;
  readonly candidates: readonly PropertyMatch[];
  readonly limit?: number;
}

export interface ProposeTreeInput {
  readonly userText: string;
  readonly lead: string;
  readonly graph: Graph;
  readonly candidates: readonly PropertyMatch[];
}

export interface RememberFilterInput {
  readonly userText: string;
  readonly lead: string;
  readonly remember: boolean;
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
  readonly previewFilter: (
    input: PreviewFilterInput,
  ) => Effect.Effect<HandleOutcome, never, RuntimeContextInterface>;
  readonly proposeTree: (
    input: ProposeTreeInput,
  ) => Effect.Effect<HandleOutcome, never, RuntimeContextInterface>;
  readonly rememberFilter: (
    input: RememberFilterInput,
  ) => Effect.Effect<HandleOutcome, never, RuntimeContextInterface>;
  readonly attachDraft: (
    input: AttachDraftInput,
  ) => Effect.Effect<HandleOutcome, never, RuntimeContextInterface>;
}

export interface ThreadStorage {
  readonly get: <T>(key: string) => Effect.Effect<T | undefined, never, RuntimeContextInterface>;
  readonly put: (key: string, value: unknown) => Effect.Effect<void, never, RuntimeContextInterface>;
  readonly delete: (key: string) => Effect.Effect<void, never, RuntimeContextInterface>;
}

export class ConversationThread extends Cloudflare.DurableObject<
  ConversationThread,
  ThreadShape
>()("ConversationThread") {}

export const makeThread = (storage: ThreadStorage): ThreadShape => {
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
      yield* storage.put("turns", nextTurns);
      yield* storage.put("summary", nextSummary);
    });

  const loadTree = Effect.gen(function* () {
    const stored = yield* storage.get<TreeDoc>("tree");
    if (stored !== undefined) return { tree: stored, created: false };
    const legacy = yield* storage.get<CriteriaSpec>("criteria");
    const tree = compileSpec(legacy ?? DEFAULT_SPEC, TREE_NAME, 1);
    yield* storage.put("tree", tree);
    return { tree, created: true };
  });

  const startOnboarding = (userText: string) =>
    Effect.gen(function* () {
      const loaded = yield* loadTree;
      const turns = (yield* storage.get<readonly Turn[]>("turns")) ?? [];
      const summary = (yield* storage.get<string>("summary")) ?? "";
      const reply =
        `Data Diver turns county tax, assessment, code and court records into ` +
        `property leads using a decision tree built only for you. I will ask six ` +
        `short questions, show you the rule, and run nothing until you approve it.\n\n` +
        `1/6 — What county or city and state do you buy in? Example: Norfolk, VA`;
      yield* storage.put("onboarding", { step: "market" } satisfies OnboardingState);
      yield* storage.put("pending", { kind: "idle" } satisfies Pending);
      yield* record(userText, reply, loaded.tree, turns, summary);
      return { reply, evaluations: [] } satisfies HandleOutcome;
    });

  const applyScout = (input: ApplyScoutInput) =>
    Effect.gen(function* () {
      const loaded = yield* loadTree;
      let tree: TreeDoc = loaded.tree;
      let changedTree: TreeDoc | undefined = loaded.created ? loaded.tree : undefined;
      if (input.graph !== undefined) {
        tree = { name: tree.name, version: tree.version + 1, graph: input.graph };
        yield* storage.put("tree", tree);
        changedTree = tree;
      }
      if (input.county !== undefined && input.county !== "") {
        yield* storage.put("county", input.county);
      }
      const turns = (yield* storage.get<readonly Turn[]>("turns")) ?? [];
      const summary = (yield* storage.get<string>("summary")) ?? "";
      yield* record(input.userText, input.reply, tree, turns, summary);
      const outcome: HandleOutcome =
        changedTree === undefined
          ? { reply: input.reply, evaluations: [] }
          : { reply: input.reply, evaluations: [], tree: changedTree };
      return outcome;
    });

  const previewFilter = (input: PreviewFilterInput) =>
    Effect.gen(function* () {
      const problems = validateGraph(input.graph, knownSignalsOf(input.candidates));
      if (problems.length > 0) {
        return yield* applyScout({
          userText: input.userText,
          reply:
            `I drafted a one-off filter but it failed validation:\n` +
            problems.map((problem) => `- ${problem}`).join("\n") +
            `\nNothing was changed; try rephrasing.`,
        });
      }
      const loaded = yield* loadTree;
      const tree = loaded.tree;
      const qualified = qualifiedOf(evaluateAgainst(input.graph, input.candidates));
      const top = qualified.slice(0, Math.max(1, input.limit ?? 3));
      const lead = input.lead.trim();
      const body =
        qualified.length === 0
          ? `Nothing matches that filter.`
          : `${qualified.length} propert${qualified.length === 1 ? "y matches" : "ies match"} ` +
            `it. Top ${top.length}:\n\n${top.map(describeMatch).join("\n\n")}`;
      const reply =
        `${lead === "" ? "" : `${lead}\n\n`}${body}\n\n` +
        `Just for this look: your saved criteria (v${tree.version}) are untouched.\n` +
        `Reply YES to make this your criteria, or NO to drop it.`;
      yield* storage.put("pending", {
        kind: "remember_filter",
        graph: input.graph,
        matches: top,
      } satisfies Pending);
      const turns = (yield* storage.get<readonly Turn[]>("turns")) ?? [];
      const summary = (yield* storage.get<string>("summary")) ?? "";
      yield* record(input.userText, reply, tree, turns, summary);
      /// An evaluation names the stored tree version that produced it; an overlay has none.
      return {
        reply,
        evaluations: [],
        ...(loaded.created ? { tree } : {}),
      } satisfies HandleOutcome;
    });

  const MAX_NAMED_CHANGES = 5;

  const namedList = (
    keys: readonly string[],
    matches: ReadonlyMap<string, PropertyMatch>,
  ): string => {
    const names = keys.map((key) => matches.get(key)?.address ?? key);
    return names.length > MAX_NAMED_CHANGES
      ? `${names.slice(0, MAX_NAMED_CHANGES).join(", ")}, +${names.length - MAX_NAMED_CHANGES} more`
      : names.join(", ");
  };

  const proposeTree = (input: ProposeTreeInput) =>
    Effect.gen(function* () {
      const loaded = yield* loadTree;
      const activeTree = loaded.tree;
      const activeMatches = new Map(
        qualifiedOf(evaluateAgainst(activeTree.graph, input.candidates)).map((m) => [
          m.match.propertyKey,
          m.match,
        ]),
      );
      const proposedMatches = new Map(
        qualifiedOf(evaluateAgainst(input.graph, input.candidates)).map((m) => [
          m.match.propertyKey,
          m.match,
        ]),
      );
      const remain = [...activeMatches.keys()].filter((key) => proposedMatches.has(key));
      const removed = [...activeMatches.keys()].filter((key) => !proposedMatches.has(key));
      const added = [...proposedMatches.keys()].filter((key) => !activeMatches.has(key));
      const parts = [
        `${remain.length} remain qualified`,
        `${removed.length} removed${removed.length > 0 ? `: ${namedList(removed, activeMatches)}` : ""}`,
        `${added.length} added${added.length > 0 ? `: ${namedList(added, proposedMatches)}` : ""}`,
      ];
      const lead = input.lead.trim();
      const reply =
        `${lead === "" ? "" : `${lead}\n\n`}Impact preview against ${input.candidates.length} ` +
        `scanned propert${input.candidates.length === 1 ? "y" : "ies"} (active criteria ` +
        `v${activeTree.version}):\n${parts.join("; ")}.\n\n` +
        `This is a proposal, not yet applied. Reply APPROVE to commit it as criteria ` +
        `v${activeTree.version + 1}, or REJECT to discard.`;
      yield* storage.put("pending", { kind: "approve_tree", graph: input.graph } satisfies Pending);
      const turns = (yield* storage.get<readonly Turn[]>("turns")) ?? [];
      const summary = (yield* storage.get<string>("summary")) ?? "";
      yield* record(input.userText, reply, activeTree, turns, summary);
      return {
        reply,
        evaluations: [],
        ...(loaded.created ? { tree: activeTree } : {}),
      } satisfies HandleOutcome;
    });

  const rememberFilter = (input: RememberFilterInput) =>
    Effect.gen(function* () {
      const loaded = yield* loadTree;
      const pending = (yield* storage.get<Pending>("pending")) ?? { kind: "idle" as const };
      const lead = input.lead.trim();
      const prefix = lead === "" ? "" : `${lead}\n\n`;
      if (pending.kind !== "remember_filter") {
        return yield* applyScout({
          userText: input.userText,
          reply:
            `${prefix}There is no one-off filter waiting to be saved. Your criteria ` +
            `(v${loaded.tree.version}) are:\n${describe(loaded.tree.graph)}`,
        });
      }
      yield* storage.put("pending", { kind: "idle" } satisfies Pending);
      if (!input.remember) {
        return yield* applyScout({
          userText: input.userText,
          reply:
            `${prefix}Dropped it. Your saved criteria (v${loaded.tree.version}) still ` +
            `stand:\n${describe(loaded.tree.graph)}`,
        });
      }
      return yield* applyScout({
        userText: input.userText,
        reply: `${prefix}Saved. These are your criteria from now on:\n${describe(pending.graph)}`,
        graph: pending.graph,
      });
    });

  return {
    /// Everything this thread remembers: the tree, the turns, the folded
    /// summary, the pending step and the county. Clearing the rows in
    /// Postgres without this leaves the object still holding a compacted
    /// history, so the next message would not look like a first one.
    forget: () =>
      Effect.gen(function* () {
        for (const key of [
          "tree",
          "criteria",
          "turns",
          "summary",
          "pending",
          "county",
          "onboarding",
        ]) {
          yield* storage.delete(key);
        }
      }),

    handleMessage: (input: HandleMessageInput) =>
      Effect.gen(function* () {
        const loaded = yield* loadTree;
        let tree: TreeDoc = loaded.tree;
        let changedTree: TreeDoc | undefined = loaded.created ? loaded.tree : undefined;

        const turns = (yield* storage.get<readonly Turn[]>("turns")) ?? [];
        const summary = (yield* storage.get<string>("summary")) ?? "";
        const rawPending =
          (yield* storage.get<Pending>("pending")) ?? { kind: "idle" as const };
        // Pre-graph deploys stored review matches without traces; drop them.
        const pending: Pending =
          rawPending.kind === "review" && rawPending.matches.some((m) => !("match" in m))
            ? { kind: "idle" }
            : rawPending;

        const adopt = (spec: CriteriaSpec) =>
          Effect.gen(function* () {
            tree = compileSpec(spec, TREE_NAME, tree.version + 1);
            yield* storage.put("tree", tree);
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
            yield* storage.put("pending", nextPending);
            yield* record(userText, reply, tree, turns, summary);
            const toPersist = changedTree ?? (evaluations.length > 0 ? tree : undefined);
            return {
              reply,
              evaluations,
              ...(toPersist === undefined ? {} : { tree: toPersist }),
              ...(outreach === undefined ? {} : { outreach }),
            } satisfies HandleOutcome;
          });

        const evaluateAll = () => evaluateAgainst(tree.graph, input.candidates);

        const asEvaluations = (
          pairs: readonly EvaluatedPair[],
        ): readonly PropertyEvaluation[] =>
          pairs.map(({ match, verdict }) => ({
            propertyKey: match.propertyKey,
            outcome: verdict.outcome,
            requiresApproval: verdict.outcome === "match" && verdict.requiresApproval,
            treeName: tree.name,
            treeVersion: tree.version,
            trace: verdict.trace,
          }));

        const text = input.text.trim();
        const lower = text.toLowerCase();

        if (pending.kind === "remember_filter") {
          if (AFFIRMATIONS.includes(lower)) {
            return yield* rememberFilter({ userText: text, lead: "", remember: true });
          }
          if (DECLINES.includes(lower)) {
            return yield* rememberFilter({ userText: text, lead: "", remember: false });
          }
        }

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

        if (pending.kind === "approve_tree") {
          if (lower === "approve" || lower === "yes") {
            tree = { name: tree.name, version: tree.version + 1, graph: pending.graph };
            yield* storage.put("tree", tree);
            yield* storage.delete("onboarding");
            changedTree = tree;
            const pairs = evaluateAll();
            const evaluations = asEvaluations(pairs);
            const qualifiedCount = qualifiedOf(pairs).length;
            return yield* respond(
              text,
              `Your decision tree v${tree.version} is active. I evaluated ` +
                `${input.candidates.length} ` +
                `county propert${input.candidates.length === 1 ? "y" : "ies"} and found ` +
                `${qualifiedCount} lead${qualifiedCount === 1 ? "" : "s"} that pass it. ` +
                `Ask me to show the strongest leads, explain why one matched, or change ` +
                `any part of the rule.`,
              { kind: "idle" },
              evaluations,
            );
          }
          if (lower === "reject" || lower === "no") {
            yield* storage.delete("onboarding");
            return yield* respond(
              text,
              `Discarded. Nothing was applied. Tell me what you want changed, or say ` +
                `"start over" and I will rebuild the questions with you. Your active ` +
                `criteria remain v${tree.version}.`,
              { kind: "idle" },
            );
          }
        }

        if (
          loaded.tree.version === 1 &&
          input.codexAccount !== undefined &&
          input.codexAccount !== ""
        ) {
          const onboarding = yield* storage.get<OnboardingState>("onboarding");
          if (onboarding === undefined) {
            return yield* startOnboarding(text);
          }
          if (onboarding.step === "market") {
            const county = parseMarket(text);
            if (county === undefined) {
              return yield* respond(
                text,
                `Please include the county or city and its two-letter state. ` +
                  `Example: Norfolk, VA`,
                { kind: "idle" },
              );
            }
            yield* storage.put("county", county);
            yield* storage.put("onboarding", {
              ...onboarding,
              step: "min_owed",
              county,
            } satisfies OnboardingState);
            return yield* respond(
              text,
              `Got it: ${county}. I will use that market's public records.\n\n` +
                `2/6 — What minimum recorded taxes or liens makes a property worth ` +
                `your time? Example: $20k`,
              { kind: "idle" },
            );
          }
          if (onboarding.step === "min_owed") {
            const minOwed = parseMoney(text);
            if (minOwed === undefined) {
              return yield* respond(
                text,
                `Give me a dollar amount such as $20k, $75,000, or "no minimum."`,
                { kind: "idle" },
              );
            }
            yield* storage.put("onboarding", {
              ...onboarding,
              step: "min_assessed",
              minOwed,
            } satisfies OnboardingState);
            return yield* respond(
              text,
              `Recorded debt floor: ${money(minOwed)}.\n\n` +
                `3/6 — What minimum county-assessed property value do you want? ` +
                `Example: $150k, or NONE`,
              { kind: "idle" },
            );
          }
          if (onboarding.step === "min_assessed") {
            const minAssessed = parseMoney(text);
            if (minAssessed === undefined) {
              return yield* respond(
                text,
                `Give me a value such as $150k, $300,000, or "none."`,
                { kind: "idle" },
              );
            }
            yield* storage.put("onboarding", {
              ...onboarding,
              step: "evidence",
              minAssessed,
            } satisfies OnboardingState);
            return yield* respond(
              text,
              `Assessed-value floor: ${money(minAssessed)}.\n\n` +
                `4/6 — What extra evidence must a lead have: MULTIPLE SOURCES, ` +
                `an OPEN VIOLATION, BOTH, or NEITHER?`,
              { kind: "idle" },
            );
          }
          if (onboarding.step === "evidence") {
            const evidence = parseEvidence(text);
            if (evidence === undefined) {
              return yield* respond(
                text,
                `Reply MULTIPLE SOURCES, OPEN VIOLATION, BOTH, or NEITHER.`,
                { kind: "idle" },
              );
            }
            yield* storage.put("onboarding", {
              ...onboarding,
              ...evidence,
              step: "recent",
            } satisfies OnboardingState);
            return yield* respond(
              text,
              `Evidence rule saved.\n\n5/6 — How recent must the latest county event be? ` +
                `Example: 180 days, 1 year, or ANY`,
              { kind: "idle" },
            );
          }
          if (onboarding.step === "recent") {
            const maxDaysSinceEvent = parseRecency(text);
            if (maxDaysSinceEvent === "invalid") {
              return yield* respond(
                text,
                `Give me a window such as 90 days, 6 months, 1 year, or ANY.`,
                { kind: "idle" },
              );
            }
            yield* storage.put("onboarding", {
              ...onboarding,
              step: "approval",
              ...(maxDaysSinceEvent === undefined ? {} : { maxDaysSinceEvent }),
            } satisfies OnboardingState);
            return yield* respond(
              text,
              `Recency rule saved.\n\n6/6 — Must you approve every outreach message ` +
                `before anything is scheduled? YES or NO`,
              { kind: "idle" },
            );
          }
          if (onboarding.step === "approval") {
            const requireApproval = parseChoice(text);
            if (requireApproval === undefined) {
              return yield* respond(
                text,
                `Reply YES to require your approval, or NO to let matching leads ` +
                  `move directly to simulated scheduling.`,
                { kind: "idle" },
              );
            }
            const completed = {
              ...onboarding,
              step: "confirm" as const,
              requireApproval,
            };
            yield* storage.put("onboarding", completed satisfies OnboardingState);
            const graph = onboardingGraph(completed);
            const evidence = [
              completed.requireMultiSource === true ? "2+ independent sources" : "",
              completed.requireViolation === true ? "an open violation" : "",
            ].filter((value) => value !== "");
            const summary =
              `I built your private lead rule for ${completed.county}: ` +
              `owed ≥ ${money(completed.minOwed ?? 0)}, assessed ≥ ` +
              `${money(completed.minAssessed ?? 0)}, ` +
              `${evidence.length === 0 ? "no extra evidence requirement" : evidence.join(" and ")}, ` +
              `${completed.maxDaysSinceEvent === undefined ? "any event age" : `activity within ${completed.maxDaysSinceEvent} days`}, ` +
              `${requireApproval ? "your approval before outreach" : "no outreach approval gate"}.`;
            return yield* proposeTree({
              userText: text,
              lead: summary,
              graph,
              candidates: input.candidates,
            });
          }
          return yield* respond(
            text,
            `Your decision tree is waiting for confirmation. Reply APPROVE to use ` +
              `it or REJECT to discard it.`,
            pending,
          );
        }

        if (
          (pending.kind === "review" || pending.kind === "remember_filter") &&
          /^\d+$/.test(lower)
        ) {
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
          const basis =
            pending.kind === "remember_filter"
              ? `the one-off filter, not your saved criteria v${tree.version}`
              : `criteria v${tree.version}`;
          const explanation =
            `${match.address} — recorded owner: ${match.owner || "unknown"}.\n` +
            `${contactSummary(match)}\n\n` +
            `${worthSummary(match)}\n\n` +
            `${linkBlock(match.address, match.propertyKey.split("|")[0] ?? "")}\n\n` +
            `Why it matched (${basis}):\n${explainTrace(evaluated.trace)}`;
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
              `Open this link to connect your ChatGPT account to Data Diver:\n` +
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
          `I have your decision tree and ${qualified.length} current ` +
            `lead${qualified.length === 1 ? "" : "s"} pass it. Ask me to show the ` +
            `strongest leads, explain your criteria, change a rule, or inspect a ` +
            `specific property.`,
          pending,
        );
      }),

    snapshot: () =>
      Effect.gen(function* () {
        const loaded = yield* loadTree;
        const turns = (yield* storage.get<readonly Turn[]>("turns")) ?? [];
        const summary = (yield* storage.get<string>("summary")) ?? "";
        const county = (yield* storage.get<string>("county")) ?? "norfolk";
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
        const turns = (yield* storage.get<readonly Turn[]>("turns")) ?? [];
        const summary = (yield* storage.get<string>("summary")) ?? "";
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
        yield* storage.put("pending", nextPending);
        yield* record(input.userText, reply, tree, turns, summary);
        return {
          reply,
          evaluations: [],
          ...(changedTree === undefined ? {} : { tree: changedTree }),
          ...(outreach === undefined ? {} : { outreach }),
        } satisfies HandleOutcome;
      }),

    applyScout,
    previewFilter,
    proposeTree,
    rememberFilter,
  };
};

export const ConversationThreadLive = ConversationThread.make<never>(
  Effect.gen(function* () {
    const state = yield* Cloudflare.DurableObjectState;
    return Effect.succeed(
      makeThread({
        get: <T>(key: string) => state.storage.get<T>(key),
        put: (key: string, value: unknown) => state.storage.put(key, value),
        delete: (key: string) => state.storage.delete(key).pipe(Effect.asVoid),
      }),
    );
  }),
);
