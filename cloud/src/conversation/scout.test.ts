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
    expect(text).toContain("12 properties compiled across the active markets");
    expect(text).toContain("- norfolk: 12 loaded, 3 match");
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
    expect(text).toContain("approve, reject, or correct it");
    expect(text).toContain("A correction is not a rejection");
    expect(text).toContain("Use revise_outreach");
    expect(text).toContain("PENDING ACTION: idle");
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
    expect(text).toContain("Never answer a broad question with a dense list");
    expect(text).toContain("give their market and every must-have in the same message");
    expect(text).toContain("Do not explain the whole process");
    expect(text).toContain("line breaks that are easy to scan");
    expect(text).toContain("Never mention decision-tree");
    expect(text).toContain("Do not claim you are checking");
    expect(text).toContain('"queued", "running", "processing"');
    expect(text).toContain('"captured", "noted", or "I have"');
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
        partial: false,
      },
    });
    expect(text).toContain("source coverage is incomplete");
    expect(text).toContain("amount owed, corroborating sources");
    expect(text).toContain("No combined lead count is available yet");
    expect(text).not.toContain("0 currently match");
  });

  test("tells the model to show verified partial leads instead of refusing them", () => {
    const text = buildInstructions({
      tree,
      configured: true,
      summary: "",
      recentTurns: [],
      county: "city_of_norfolk_va",
      candidateCount: 12_512,
      qualifiedCount: 3,
      extraSignals: [],
      coverage: {
        ready: true,
        missing: [],
        partial: true,
      },
    });
    expect(text).toContain("verified partial set");
    expect(text).toContain("use show_matches");
    expect(text).toContain("Treat the current SCAN STATE as authoritative");
  });

  test("collects onboarding preferences without a serial interview", () => {
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
    expect(text).toContain("one update_profile call");
    expect(text).toContain("NEXT ONBOARDING FIELD: county");
    expect(text).toContain("Extract every search preference");
    expect(text).toContain("completeWithDefaults=true");
    expect(text).toContain("Never discard or re-ask");
    expect(text).toContain("run a serial field-by-field interview");
    expect(text).toContain("never guess a missing state");
    expect(text).toContain("complete user-facing SMS");
    expect(text).toContain("Never expose command menus");
    expect(text).toContain("Support one to five simultaneous markets");
    expect(text).toContain("state-only phrase");
    expect(text).toContain('"why" question');
  });

  test("shows independent state and coverage for simultaneous markets", () => {
    const text = buildInstructions({
      tree,
      configured: true,
      summary: "",
      recentTurns: [],
      county: "Norfolk, VA",
      markets: [
        {
          market: "Norfolk, VA",
          candidateCount: 12_512,
          qualifiedCount: 3_271,
          coverage: { ready: true, missing: [], partial: true },
        },
        {
          market: "Cincinnati, OH",
          candidateCount: 3_441,
          qualifiedCount: 3_441,
          coverage: { ready: true, missing: [], partial: false },
        },
      ],
      candidateCount: 15_953,
      qualifiedCount: 6_712,
      extraSignals: [],
      coverage: { ready: true, missing: [], partial: true },
    });

    expect(text).toContain("- Norfolk, VA: 12512 loaded, 3271 match");
    expect(text).toContain("- Cincinnati, OH: 3441 loaded, 3441 match");
    expect(text).toContain("Some compiled records are a verified partial set");
  });
});
