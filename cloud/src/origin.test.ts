import { describe, expect, test } from "bun:test";

import { requestOrigin } from "./origin.ts";

describe("requestOrigin", () => {
  test("uses the routed host instead of a stale configured deployment", () => {
    expect(
      requestOrigin(
        "/connect/abc",
        {
          host: "datadiver.example.workers.dev",
          "x-forwarded-proto": "https",
        },
        "https://old.example.workers.dev",
      ),
    ).toBe("https://datadiver.example.workers.dev");
  });

  test("does not let a forwarded host replace the routed host", () => {
    expect(
      requestOrigin(
        "/connect/abc",
        {
          host: "records.example.com",
          "x-forwarded-host": "attacker.example",
        },
        "",
      ),
    ).toBe("https://records.example.com");
  });

  test("falls back to the configured origin when no routed host is available", () => {
    expect(requestOrigin("/connect/abc", {}, "https://fallback.example/")).toBe(
      "https://fallback.example",
    );
  });
});
