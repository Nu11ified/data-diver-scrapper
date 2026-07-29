import * as Schema from "effect/Schema";

export const ConditionOp = Schema.Literals(["gte", "gt", "lte", "lt", "eq"]);
export type ConditionOp = (typeof ConditionOp)["Type"];

export const ConditionNode = Schema.Struct({
  kind: Schema.Literal("condition"),
  id: Schema.String,
  field: Schema.String,
  op: ConditionOp,
  value: Schema.Number,
  onPass: Schema.String,
  onFail: Schema.String,
});
export type ConditionNode = (typeof ConditionNode)["Type"];

export const ActionNode = Schema.Struct({
  kind: Schema.Literal("action"),
  id: Schema.String,
  action: Schema.Literals(["match", "discard"]),
});
export type ActionNode = (typeof ActionNode)["Type"];

export const ApprovalNode = Schema.Struct({
  kind: Schema.Literal("approval"),
  id: Schema.String,
  prompt: Schema.String,
  next: Schema.String,
});
export type ApprovalNode = (typeof ApprovalNode)["Type"];

export const DecisionNode = Schema.Union([ConditionNode, ActionNode, ApprovalNode]);
export type DecisionNode = (typeof DecisionNode)["Type"];

export const Graph = Schema.Struct({
  entry: Schema.String,
  nodes: Schema.Array(DecisionNode),
});
export type Graph = (typeof Graph)["Type"];

export const CriteriaSpec = Schema.Struct({
  minOwed: Schema.Number,
  requireMultiSource: Schema.Boolean,
  minDebtToValue: Schema.Number,
  maxDaysSinceEvent: Schema.optional(Schema.Number),
});
export type CriteriaSpec = (typeof CriteriaSpec)["Type"];

export const TreeDoc = Schema.Struct({
  name: Schema.String,
  version: Schema.Int,
  spec: Schema.optional(CriteriaSpec),
  graph: Graph,
});
export type TreeDoc = (typeof TreeDoc)["Type"];

export const DEFAULT_SPEC: CriteriaSpec = {
  minOwed: 10_000,
  requireMultiSource: false,
  minDebtToValue: 0,
};

export interface SignalInfo {
  readonly label: string;
  readonly format: "money" | "ratio" | "count" | "days";
  readonly description: string;
}

/// Signals the engine measures today; condition fields are open strings so
/// new engine signals become usable without touching the graph model.
export const SIGNAL_CATALOG: Readonly<Record<string, SignalInfo>> = {
  owed: {
    label: "amount owed",
    format: "money",
    description: "total delinquent taxes and liens recorded against the property",
  },
  assessed: {
    label: "assessed value",
    format: "money",
    description: "most recent county assessor valuation",
  },
  assessedPrior: {
    label: "prior assessed value",
    format: "money",
    description: "previous edition of the assessor valuation",
  },
  debtToValue: {
    label: "debt to value",
    format: "ratio",
    description: "delinquent amount divided by assessed value",
  },
  violations: {
    label: "open violations",
    format: "count",
    description: "open code enforcement cases",
  },
  sources: {
    label: "corroborating sources",
    format: "count",
    description: "independent county sources that reported this property",
  },
  days_since_event: {
    label: "days since last event",
    format: "days",
    description:
      "whole days between the newest dated county event for this property and the " +
      "compile; absent when no event carries a date the engine can read, and an " +
      "absent signal fails its condition",
  },
};

export const compileSpec = (spec: CriteriaSpec, name: string, version: number): TreeDoc => {
  const conditions: ConditionNode[] = [
    {
      kind: "condition",
      id: "owed_floor",
      field: "owed",
      op: "gte",
      value: spec.minOwed,
      onPass: "",
      onFail: "discard",
    },
  ];
  if (spec.requireMultiSource) {
    conditions.push({
      kind: "condition",
      id: "multi_source",
      field: "sources",
      op: "gt",
      value: 1,
      onPass: "",
      onFail: "discard",
    });
  }
  if (spec.minDebtToValue > 0) {
    conditions.push({
      kind: "condition",
      id: "debt_ratio",
      field: "debtToValue",
      op: "gte",
      value: spec.minDebtToValue,
      onPass: "",
      onFail: "discard",
    });
  }
  if (spec.maxDaysSinceEvent !== undefined && spec.maxDaysSinceEvent > 0) {
    conditions.push({
      kind: "condition",
      id: "recent_activity",
      field: "days_since_event",
      op: "lte",
      value: spec.maxDaysSinceEvent,
      onPass: "",
      onFail: "discard",
    });
  }
  const chained = conditions.map((node, index) => ({
    ...node,
    onPass: conditions[index + 1]?.id ?? "needs_approval",
  }));
  return {
    name,
    version,
    spec,
    graph: {
      entry: chained[0]?.id ?? "needs_approval",
      nodes: [
        ...chained,
        {
          kind: "approval",
          id: "needs_approval",
          prompt: "Outreach requires your approval",
          next: "match",
        },
        { kind: "action", id: "match", action: "match" },
        { kind: "action", id: "discard", action: "discard" },
      ],
    },
  };
};

export type Subject = Readonly<Record<string, number>>;

export interface ConditionStep {
  readonly node: string;
  readonly kind: "condition";
  readonly field: string;
  readonly op: ConditionOp;
  readonly value: number;
  readonly actual: number;
  readonly present: boolean;
  readonly passed: boolean;
}
export interface ApprovalStep {
  readonly node: string;
  readonly kind: "approval";
  readonly prompt: string;
}
export interface ActionStep {
  readonly node: string;
  readonly kind: "action";
  readonly action: "match" | "discard";
}
export type TraceStep = ConditionStep | ApprovalStep | ActionStep;

export type Verdict =
  | {
      readonly outcome: "match";
      readonly requiresApproval: boolean;
      readonly trace: readonly TraceStep[];
    }
  | { readonly outcome: "discard"; readonly trace: readonly TraceStep[] }
  | {
      readonly outcome: "invalid";
      readonly reason: string;
      readonly trace: readonly TraceStep[];
    };

const holds = (op: ConditionOp, actual: number, value: number): boolean => {
  switch (op) {
    case "gte":
      return actual >= value;
    case "gt":
      return actual > value;
    case "lte":
      return actual <= value;
    case "lt":
      return actual < value;
    case "eq":
      return actual === value;
  }
};

export const evaluate = (graph: Graph, subject: Subject): Verdict => {
  const byId = new Map(graph.nodes.map((node) => [node.id, node]));
  const trace: TraceStep[] = [];
  let requiresApproval = false;
  let currentId = graph.entry;
  for (let steps = 0; steps <= graph.nodes.length; steps += 1) {
    const node = byId.get(currentId);
    if (node === undefined) {
      return { outcome: "invalid", reason: `node "${currentId}" not found`, trace };
    }
    switch (node.kind) {
      case "condition": {
        const measured = subject[node.field];
        const present = measured !== undefined;
        // An absent signal fails the condition: unmeasured never means qualified.
        const actual = measured ?? 0;
        const passed = present && holds(node.op, actual, node.value);
        trace.push({
          node: node.id,
          kind: "condition",
          field: node.field,
          op: node.op,
          value: node.value,
          actual,
          present,
          passed,
        });
        currentId = passed ? node.onPass : node.onFail;
        break;
      }
      case "approval": {
        requiresApproval = true;
        trace.push({ node: node.id, kind: "approval", prompt: node.prompt });
        currentId = node.next;
        break;
      }
      case "action": {
        trace.push({ node: node.id, kind: "action", action: node.action });
        return node.action === "match"
          ? { outcome: "match", requiresApproval, trace }
          : { outcome: "discard", trace };
      }
    }
  }
  return { outcome: "invalid", reason: "graph cycled without reaching an action", trace };
};

export const validateGraph = (
  graph: Graph,
  knownSignals: readonly string[],
): readonly string[] => {
  const ids = new Set(graph.nodes.map((node) => node.id));
  const known = new Set(knownSignals);
  const problems: string[] = [];
  if (!ids.has(graph.entry)) problems.push(`entry "${graph.entry}" is not a node`);
  if (ids.size !== graph.nodes.length) problems.push("duplicate node ids");
  for (const node of graph.nodes) {
    if (node.kind === "condition") {
      if (!known.has(node.field)) problems.push(`condition "${node.id}" uses unknown signal "${node.field}"`);
      if (!ids.has(node.onPass)) problems.push(`condition "${node.id}" onPass -> missing "${node.onPass}"`);
      if (!ids.has(node.onFail)) problems.push(`condition "${node.id}" onFail -> missing "${node.onFail}"`);
    }
    if (node.kind === "approval" && !ids.has(node.next)) {
      problems.push(`approval "${node.id}" next -> missing "${node.next}"`);
    }
  }
  if (!graph.nodes.some((node) => node.kind === "action" && node.action === "match")) {
    problems.push("no match action: nothing can ever qualify");
  }
  return problems;
};

const OP_TEXT: Record<ConditionOp, string> = {
  gte: "at least",
  gt: "more than",
  lte: "at most",
  lt: "less than",
  eq: "exactly",
};

const money = (value: number): string => `$${Math.round(value).toLocaleString("en-US")}`;

const signalLabel = (field: string): string =>
  SIGNAL_CATALOG[field]?.label ?? field.replace(/_/g, " ");

export const formatValue = (field: string, value: number): string => {
  switch (SIGNAL_CATALOG[field]?.format) {
    case "money":
      return money(value);
    case "ratio":
      return `${value}x`;
    case "days":
      return `${value} day${value === 1 ? "" : "s"}`;
    default:
      return String(value);
  }
};

export const describe = (graph: Graph): string => {
  const byId = new Map(graph.nodes.map((node) => [node.id, node]));
  const lines: string[] = [];
  let currentId = graph.entry;
  for (let steps = 0; steps <= graph.nodes.length; steps += 1) {
    const node = byId.get(currentId);
    if (node === undefined) return [...lines, `- broken link to "${currentId}"`].join("\n");
    if (node.kind === "condition") {
      lines.push(`- ${signalLabel(node.field)} ${OP_TEXT[node.op]} ${formatValue(node.field, node.value)}`);
      currentId = node.onPass;
    } else if (node.kind === "approval") {
      lines.push(`- ${node.prompt.toLowerCase()}`);
      currentId = node.next;
    } else {
      return lines.join("\n");
    }
  }
  return lines.join("\n");
};

export const explainTrace = (trace: readonly TraceStep[]): string =>
  trace
    .flatMap((step) => {
      if (step.kind === "condition") {
        if (!step.present) {
          return [
            `✗ ${signalLabel(step.field)} is not measured for this property ` +
              `(needs ${OP_TEXT[step.op]} ${formatValue(step.field, step.value)})`,
          ];
        }
        return [
          `${step.passed ? "✓" : "✗"} ${signalLabel(step.field)} ` +
            `${formatValue(step.field, step.actual)} (needs ${OP_TEXT[step.op]} ` +
            `${formatValue(step.field, step.value)})`,
        ];
      }
      if (step.kind === "approval") return [`→ ${step.prompt.toLowerCase()}`];
      return [];
    })
    .join("\n");
