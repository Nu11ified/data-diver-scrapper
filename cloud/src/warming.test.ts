import { describe, expect, test } from "bun:test";

import {
  ACTIVE_WARM_MS,
  countyWorkflowId,
  shouldReuseWarm,
  type CountyWarmStatus,
} from "./warming.ts";

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
      shouldReuseWarm(status(), Date.parse(
        "2026-07-29T20:05:00.000Z",
      )),
    ).toBe(true);
  });

  test("keeps one active job while ingestion changes the county stamp", () => {
    expect(
      shouldReuseWarm(status(), Date.parse(
        "2026-07-29T20:05:00.000Z",
      )),
    ).toBe(true);
  });

  test("starts a new job when the prior job stopped making progress", () => {
    expect(
      shouldReuseWarm(
        status(),
        Date.parse("2026-07-29T20:00:00.000Z") + ACTIVE_WARM_MS,
      ),
    ).toBe(false);
  });

  test("does not treat a completed job as active", () => {
    expect(
      shouldReuseWarm(
        status({ state: "complete" }),
        Date.parse("2026-07-29T20:05:00.000Z"),
      ),
    ).toBe(false);
  });
});

describe("countyWorkflowId", () => {
  test("is stable for one county data revision", async () => {
    const first = await countyWorkflowId("denton_tx", "0:none", 7);
    const second = await countyWorkflowId("denton_tx", "0:none", 7);
    expect(second).toBe(first);
    expect(first).toMatch(/^county-denton_tx-[a-f0-9]{20}$/);
  });

  test("changes when data or engine semantics change", async () => {
    const base = await countyWorkflowId("denton_tx", "0:none", 7);
    expect(await countyWorkflowId("denton_tx", "1:2026-07-29", 7)).not.toBe(base);
    expect(await countyWorkflowId("denton_tx", "0:none", 8)).not.toBe(base);
  });

  test("changes for a retry while remaining stable within that retry", async () => {
    const first = await countyWorkflowId("denton_tx", "0:none", 7, "failed-at");
    expect(await countyWorkflowId("denton_tx", "0:none", 7, "failed-at")).toBe(first);
    expect(await countyWorkflowId("denton_tx", "0:none", 7, "failed-later")).not.toBe(first);
  });

  test("uses a separate stable job when more source coverage is required", async () => {
    const base = await countyWorkflowId("denton_tx", "100:now", 3);
    const coverage = await countyWorkflowId(
      "denton_tx",
      "100:now",
      3,
      "",
      "amount owed,corroborating sources",
    );
    expect(coverage).not.toBe(base);
    expect(
      await countyWorkflowId(
        "denton_tx",
        "100:now",
        3,
        "",
        "amount owed,corroborating sources",
      ),
    ).toBe(coverage);
  });
});
