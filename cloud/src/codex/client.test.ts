import { describe, expect, test } from "bun:test";

import {
  compactOutputStream,
  extractOutputText,
  requestUsesTools,
} from "./client.ts";

describe("compactOutputStream", () => {
  test("keeps only the output text from a large Codex stream", () => {
    const stream = [
      `data: ${JSON.stringify({ type: "response.reasoning.delta", delta: "x".repeat(100_000) })}`,
      `data: ${JSON.stringify({ type: "response.output_text.delta", delta: "result" })}`,
      "data: [DONE]",
      "",
    ].join("\n\n");
    const compacted = compactOutputStream(stream);
    expect(compacted.length).toBeLessThan(200);
    expect(extractOutputText(compacted)).toBe("result");
  });

  test("caps an unrecognized response before it crosses the container RPC", () => {
    expect(compactOutputStream("x".repeat(100_000))).toHaveLength(64 * 1024);
  });
});

describe("requestUsesTools", () => {
  test("preserves the full tool-call protocol for Pi", () => {
    expect(
      requestUsesTools(
        new TextEncoder().encode(
          JSON.stringify({ tools: [{ type: "function", name: "reply" }] }),
        ),
      ),
    ).toBe(true);
  });

  test("allows plain research responses to be compacted", () => {
    expect(
      requestUsesTools(new TextEncoder().encode(JSON.stringify({ input: [] }))),
    ).toBe(false);
  });

  test("preserves an unknown request shape", () => {
    expect(requestUsesTools(new TextEncoder().encode("not json"))).toBe(true);
  });
});
