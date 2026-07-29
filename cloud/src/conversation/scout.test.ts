import { describe, expect, test } from "bun:test";

import { extractOutputText } from "../codex/client.ts";
import { DEFAULT_SPEC, compileSpec } from "../decision/graph.ts";
import { buildInstructions } from "./scout.ts";

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
    expect(text).not.toContain("The user is onboarding");
  });

  test("tells the model to keep provisional requests out of the saved tree", () => {
    const text = buildInstructions({
      tree,
      configured: true,
      summary: "",
      recentTurns: [],
      county: "norfolk",
      candidateCount: 4,
      qualifiedCount: 1,
      extraSignals: [],
    });
    expect(text).toContain("temporary_filter");
    expect(text).toContain("remember_filter");
    expect(text).toContain("post-onboarding saved criteria changes");
    expect(text).toContain("request is provisional");
  });

  test("tells the model set_tree is a proposal requiring approval", () => {
    const text = buildInstructions({
      tree,
      configured: true,
      summary: "",
      recentTurns: [],
      county: "norfolk",
      candidateCount: 4,
      qualifiedCount: 1,
      extraSignals: [],
    });
    expect(text).toContain("A proposed tree never applies immediately");
    expect(text).toContain("reply APPROVE or REJECT");
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
    expect(text).toContain("The user is onboarding");
    expect(text).toContain("Use update_profile for every answer");
    expect(text).toContain("Never reject an answer merely because");
  });
});
