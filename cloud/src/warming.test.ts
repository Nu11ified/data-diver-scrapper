import { describe, expect, test } from "bun:test";

import { ACTIVE_WARM_MS, shouldReuseWarm, type CountyWarmStatus } from "./warming.ts";

const status = (
  overrides: Partial<CountyWarmStatus> = {},
): CountyWarmStatus => ({
  county: "city_of_norfolk_va",
  canonical: "city_of_norfolk_va",
  stamp: "5127:2026-07-29T20:00:00.000Z",
  state: "running",
  instanceId: "warm-1",
  requestedAt: "2026-07-29T20:00:00.000Z",
  updatedAt: "2026-07-29T20:00:00.000Z",
  ...overrides,
});

describe("shouldReuseWarm", () => {
  test("deduplicates an active job for the same county data", () => {
    expect(
      shouldReuseWarm(status(), "5127:2026-07-29T20:00:00.000Z", Date.parse(
        "2026-07-29T20:05:00.000Z",
      )),
    ).toBe(true);
  });

  test("starts a new job when ingestion changed the county stamp", () => {
    expect(
      shouldReuseWarm(status(), "5200:2026-07-29T21:00:00.000Z", Date.parse(
        "2026-07-29T20:05:00.000Z",
      )),
    ).toBe(false);
  });

  test("starts a new job when the prior job stopped making progress", () => {
    expect(
      shouldReuseWarm(
        status(),
        "5127:2026-07-29T20:00:00.000Z",
        Date.parse("2026-07-29T20:00:00.000Z") + ACTIVE_WARM_MS,
      ),
    ).toBe(false);
  });

  test("does not treat a completed job as active", () => {
    expect(
      shouldReuseWarm(
        status({ state: "complete" }),
        "5127:2026-07-29T20:00:00.000Z",
        Date.parse("2026-07-29T20:05:00.000Z"),
      ),
    ).toBe(false);
  });
});
