import { describe, expect, test } from "bun:test";

import {
  compactOutputStream,
  extractOutputText,
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
