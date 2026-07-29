import { describe as suite, expect, test } from "bun:test";
import * as Schema from "effect/Schema";

import {
  DEFAULT_SPEC,
  SIGNAL_CATALOG,
  TreeDoc,
  compileSpec,
  describe,
  evaluate,
  explainTrace,
  validateGraph,
  type Graph,
  type Subject,
} from "./graph.ts";

const subject = (overrides: Partial<Subject>): Subject => ({
  owed: 0,
  assessed: 0,
  debtToValue: 0,
  violations: 0,
  sources: 1,
  ...overrides,
});

suite("compileSpec", () => {
  test("default spec has one condition then approval then match", () => {
    const tree = compileSpec(DEFAULT_SPEC, "acquisition", 1);
    expect(tree.graph.entry).toBe("owed_floor");
    expect(tree.graph.nodes.map((n) => n.id)).toEqual([
      "owed_floor",
      "needs_approval",
      "match",
      "discard",
    ]);
  });

  test("all criteria produce a chained condition path", () => {
    const tree = compileSpec(
      { minOwed: 25_000, requireMultiSource: true, minDebtToValue: 0.5 },
      "acquisition",
      3,
    );
    const conditions = tree.graph.nodes.filter((n) => n.kind === "condition");
    expect(conditions.map((n) => n.id)).toEqual(["owed_floor", "multi_source", "debt_ratio"]);
    expect(conditions[0]?.onPass).toBe("multi_source");
    expect(conditions[1]?.onPass).toBe("debt_ratio");
    expect(conditions[2]?.onPass).toBe("needs_approval");
    expect(conditions.every((n) => n.onFail === "discard")).toBe(true);
  });

  test("a freshness window becomes a days_since_event ceiling", () => {
    const tree = compileSpec({ ...DEFAULT_SPEC, maxDaysSinceEvent: 30 }, "acquisition", 2);
    const conditions = tree.graph.nodes.filter((n) => n.kind === "condition");
    expect(conditions.map((n) => n.id)).toEqual(["owed_floor", "recent_activity"]);
    expect(conditions[1]).toMatchObject({
      field: "days_since_event",
      op: "lte",
      value: 30,
      onPass: "needs_approval",
      onFail: "discard",
    });
    expect(validateGraph(tree.graph, Object.keys(SIGNAL_CATALOG))).toEqual([]);

    const owed = { owed: 50_000 };
    expect(evaluate(tree.graph, subject({ ...owed, days_since_event: 4 })).outcome).toBe("match");
    expect(evaluate(tree.graph, subject({ ...owed, days_since_event: 30 })).outcome).toBe("match");
    expect(evaluate(tree.graph, subject({ ...owed, days_since_event: 401 })).outcome).toBe(
      "discard",
    );
    expect(evaluate(tree.graph, subject(owed)).outcome).toBe("discard");
  });

  test("an undated property is reported as unmeasured, never as fresh", () => {
    const tree = compileSpec({ ...DEFAULT_SPEC, maxDaysSinceEvent: 30 }, "acquisition", 2);
    const verdict = evaluate(tree.graph, subject({ owed: 50_000 }));
    const step = verdict.trace.find((s) => s.kind === "condition" && s.field === "days_since_event");
    expect(step).toMatchObject({ present: false, passed: false });
    expect(explainTrace(verdict.trace)).toContain(
      "✗ days since last event is not measured for this property (needs at most 30 days)",
    );
    expect(describe(tree.graph)).toContain("days since last event at most 30 days");
  });

  test("round-trips through its schema", () => {
    const tree = compileSpec(DEFAULT_SPEC, "acquisition", 1);
    const decoded = Schema.decodeUnknownExit(TreeDoc)(JSON.parse(JSON.stringify(tree)));
    expect(decoded._tag).toBe("Success");
  });
});

suite("evaluate", () => {
  const tree = compileSpec(
    { minOwed: 10_000, requireMultiSource: true, minDebtToValue: 0 },
    "acquisition",
    2,
  );

  test("match requires approval and records every hop", () => {
    const verdict = evaluate(tree.graph, subject({ owed: 32_000, sources: 2 }));
    expect(verdict.outcome).toBe("match");
    if (verdict.outcome !== "match") throw new Error("unreachable");
    expect(verdict.requiresApproval).toBe(true);
    expect(verdict.trace.map((s) => s.node)).toEqual([
      "owed_floor",
      "multi_source",
      "needs_approval",
      "match",
    ]);
  });

  test("failed condition routes to discard with the failing value in the trace", () => {
    const verdict = evaluate(tree.graph, subject({ owed: 9_999, sources: 2 }));
    expect(verdict.outcome).toBe("discard");
    const first = verdict.trace[0];
    if (first?.kind !== "condition") throw new Error("expected condition step");
    expect(first.passed).toBe(false);
    expect(first.actual).toBe(9_999);
    expect(first.value).toBe(10_000);
  });

  test("dangling node reference yields invalid, not a throw", () => {
    const broken = {
      entry: "owed_floor",
      nodes: [
        {
          kind: "condition",
          id: "owed_floor",
          field: "owed",
          op: "gte",
          value: 1,
          onPass: "missing",
          onFail: "discard",
        },
      ],
    } as const;
    const verdict = evaluate(broken, subject({ owed: 5 }));
    expect(verdict.outcome).toBe("invalid");
    if (verdict.outcome !== "invalid") throw new Error("unreachable");
    expect(verdict.reason).toContain("missing");
  });

  test("cycle yields invalid instead of looping", () => {
    const cyclic = {
      entry: "a",
      nodes: [
        { kind: "approval", id: "a", prompt: "loop", next: "b" },
        { kind: "approval", id: "b", prompt: "loop", next: "a" },
      ],
    } as const;
    expect(evaluate(cyclic, subject({})).outcome).toBe("invalid");
  });
});

suite("dynamic signals", () => {
  const customTree: Graph = {
    entry: "night_activity",
    nodes: [
      {
        kind: "condition",
        id: "night_activity",
        field: "utility_shutoffs",
        op: "gte",
        value: 1,
        onPass: "match",
        onFail: "discard",
      },
      { kind: "action", id: "match", action: "match" },
      { kind: "action", id: "discard", action: "discard" },
    ],
  };

  test("conditions can reference any signal key the subject carries", () => {
    const verdict = evaluate(customTree, { utility_shutoffs: 2 });
    expect(verdict.outcome).toBe("match");
  });

  test("an unmeasured signal fails the condition and the trace says so", () => {
    const verdict = evaluate(customTree, subject({ owed: 99_999 }));
    expect(verdict.outcome).toBe("discard");
    const first = verdict.trace[0];
    if (first?.kind !== "condition") throw new Error("expected condition step");
    expect(first.present).toBe(false);
    expect(first.passed).toBe(false);
    expect(explainTrace(verdict.trace)).toContain("not measured for this property");
  });

  test("validateGraph accepts a compiled tree", () => {
    const tree = compileSpec(DEFAULT_SPEC, "acquisition", 1);
    expect(validateGraph(tree.graph, Object.keys(SIGNAL_CATALOG))).toEqual([]);
  });

  test("validateGraph reports unknown signals, dangling refs and missing match", () => {
    const broken: Graph = {
      entry: "gone",
      nodes: [
        {
          kind: "condition",
          id: "c1",
          field: "mystery_signal",
          op: "gte",
          value: 1,
          onPass: "nowhere",
          onFail: "discard",
        },
        { kind: "action", id: "discard", action: "discard" },
      ],
    };
    const problems = validateGraph(broken, Object.keys(SIGNAL_CATALOG));
    expect(problems.some((p) => p.includes('entry "gone"'))).toBe(true);
    expect(problems.some((p) => p.includes("mystery_signal"))).toBe(true);
    expect(problems.some((p) => p.includes('missing "nowhere"'))).toBe(true);
    expect(problems.some((p) => p.includes("no match action"))).toBe(true);
  });
});

suite("rendering", () => {
  test("describe lists the pass path", () => {
    const tree = compileSpec(
      { minOwed: 25_000, requireMultiSource: true, minDebtToValue: 0.5 },
      "acquisition",
      1,
    );
    const text = describe(tree.graph);
    expect(text).toContain("amount owed at least $25,000");
    expect(text).toContain("corroborating sources more than 1");
    expect(text).toContain("debt to value at least 0.5x");
    expect(text).toContain("outreach requires your approval");
  });

  test("explainTrace shows measured values against thresholds", () => {
    const tree = compileSpec(DEFAULT_SPEC, "acquisition", 1);
    const verdict = evaluate(tree.graph, subject({ owed: 32_100 }));
    const text = explainTrace(verdict.trace);
    expect(text).toContain("✓ amount owed $32,100 (needs at least $10,000)");
    expect(text).toContain("→ outreach requires your approval");
  });
});
