import { describe, expect, test } from "bun:test";

import { extractOutputText } from "../codex/client.ts";
import { DEFAULT_SPEC, compileSpec } from "../decision/graph.ts";
import { buildInstructions, parseScoutDecision } from "./scout.ts";

describe("extractOutputText", () => {
  test("prefers the completed event over deltas", () => {
    const sse = [
      `data: {"type":"response.output_text.delta","delta":"par"}`,
      `data: {"type":"response.output_text.delta","delta":"tial"}`,
      `data: {"type":"response.completed","response":{"output":[{"type":"message","content":[{"type":"output_text","text":"full answer"}]}]}}`,
      `data: [DONE]`,
    ].join("\n");
    expect(extractOutputText(sse)).toBe("full answer");
  });

  test("falls back to concatenated deltas", () => {
    const sse = [
      `data: {"type":"response.output_text.delta","delta":"hello "}`,
      `data: {"type":"response.output_text.delta","delta":"world"}`,
    ].join("\n");
    expect(extractOutputText(sse)).toBe("hello world");
  });

  test("ignores unparseable lines", () => {
    expect(extractOutputText("data: not json\nnoise\n")).toBe("");
  });
});

describe("parseScoutDecision", () => {
  test("reads a reply decision", () => {
    const decision = parseScoutDecision(`{"kind":"reply","text":"What is your minimum owed?"}`);
    expect(decision).toEqual({ kind: "reply", text: "What is your minimum owed?" });
  });

  test("reads a set_tree decision with a full graph", () => {
    const graph = compileSpec(DEFAULT_SPEC, "acquisition", 1).graph;
    const decision = parseScoutDecision(
      JSON.stringify({ kind: "set_tree", text: "Done.", graph }),
    );
    expect(decision.kind).toBe("set_tree");
    if (decision.kind !== "set_tree") throw new Error("unreachable");
    expect(decision.graph.entry).toBe("owed_floor");
  });

  test("strips markdown fences", () => {
    const decision = parseScoutDecision('```json\n{"kind":"reply","text":"hi"}\n```');
    expect(decision).toEqual({ kind: "reply", text: "hi" });
  });

  test("reads a discover decision with candidate sources", () => {
    const decision = parseScoutDecision(
      JSON.stringify({
        kind: "discover",
        text: "Validating Chesterfield sources now.",
        jurisdiction: "chesterfield_county_va",
        candidates: [
          {
            id: "chesterfield_county_va_tax",
            name: "Chesterfield delinquent taxes",
            url: "https://data.chesterfield.gov/resource/abcd-1234.csv",
          },
        ],
      }),
    );
    expect(decision.kind).toBe("discover");
    if (decision.kind !== "discover") throw new Error("unreachable");
    expect(decision.jurisdiction).toBe("chesterfield_county_va");
    expect(decision.candidates).toHaveLength(1);
  });

  test("non-contract output becomes a plain reply", () => {
    const decision = parseScoutDecision("I think you should raise the floor.");
    expect(decision).toEqual({ kind: "reply", text: "I think you should raise the floor." });
  });

  test("malformed graph falls back to plain reply", () => {
    const decision = parseScoutDecision(`{"kind":"set_tree","text":"x","graph":{"entry":1}}`);
    expect(decision.kind).toBe("reply");
  });
});

describe("buildInstructions", () => {
  const tree = compileSpec(DEFAULT_SPEC, "acquisition", 1);

  test("carries the signal catalog, tree and counts", () => {
    const text = buildInstructions({
      tree,
      configured: true,
      summary: "",
      recentTurns: [],
      county: "norfolk",
      candidateCount: 12,
      qualifiedCount: 3,
      extraSignals: ["utility_shutoffs"],
    });
    expect(text).toContain("owed:");
    expect(text).toContain("utility_shutoffs");
    expect(text).toContain(`"entry":"owed_floor"`);
    expect(text).toContain(`county "norfolk"; 12 properties`);
    expect(text).toContain("3 currently match");
    expect(text).not.toContain("FIRST-TIME USER");
  });

  test("runs the onboarding interview for unconfigured users", () => {
    const text = buildInstructions({
      tree,
      configured: false,
      summary: "",
      recentTurns: [],
      county: "norfolk",
      candidateCount: 0,
      qualifiedCount: 0,
      extraSignals: [],
    });
    expect(text).toContain("FIRST-TIME USER");
    expect(text).toContain("minimum amount owed");
  });
});
