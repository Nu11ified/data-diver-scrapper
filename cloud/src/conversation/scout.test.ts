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

  test("gives every reply a clear outcome, progression and next move", () => {
    const text = buildInstructions({
      tree,
      configured: true,
      summary: "",
      recentTurns: [],
      county: "dallas_tx",
      candidateCount: 50,
      qualifiedCount: 8,
      extraSignals: [],
    });
    expect(text).toContain("Guide the user like an acquisition scout");
    expect(text).toContain("1. OUTCOME");
    expect(text).toContain("2. PROGRESS");
    expect(text).toContain("3. NEXT MOVE");
    expect(text).toContain(
      "end with exactly one contextual question or action",
    );
    expect(text).toContain("ranked call list");
    expect(text).toContain("Never answer a broad question with a dense list");
    expect(text).toContain("use three blocks");
    expect(text).toContain("separated by blank lines");
    expect(text).toContain("Line breaks should make the");
    expect(text).toContain("Never mention decision-tree");
    expect(text).toContain("Do not claim you are checking");
    expect(text).toContain("Only report zero matches");
    expect(text).toContain("source coverage is complete");
  });

  test("withholds lead counts while required county coverage is incomplete", () => {
    const text = buildInstructions({
      tree,
      configured: true,
      summary: "",
      recentTurns: [],
      county: "denton_tx",
      candidateCount: 100,
      qualifiedCount: 0,
      extraSignals: [],
      coverage: {
        ready: false,
        missing: ["amount owed", "corroborating sources"],
      },
    });
    expect(text).toContain("source coverage is incomplete");
    expect(text).toContain("amount owed, corroborating sources");
    expect(text).toContain("No valid lead count is available yet");
    expect(text).not.toContain("0 currently match");
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
    expect(text).toContain("use update_profile");
    expect(text).toContain("NEXT ONBOARDING FIELD: county");
    expect(text).toContain("never drop a supplied state");
    expect(text).toContain("complete user-facing SMS");
    expect(text).toContain("Never expose command menus");
  });
});
