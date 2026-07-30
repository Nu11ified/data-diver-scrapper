import { describe, expect, test } from "bun:test";

import {
  ACTIVE_WARM_MS,
  FAILED_WARM_RETRY_MS,
  countyWorkflowId,
  shouldReuseWarm,
  warmRetryKey,
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

describe("warmRetryKey", () => {
  test("retries a workflow whose runtime errored even when its stored state is stale", () => {
    const failed = status({ state: "running" });
    const now = Date.parse("2026-07-29T20:05:37.000Z");
    expect(
      warmRetryKey(failed, "errored", now),
    ).toBe("2026-07-29T20:05:00.000Z");
  });

  test("deduplicates failed-runtime retries within one retry window", () => {
    const failed = status({ state: "running" });
    expect(warmRetryKey(failed, "errored", Date.parse("2026-07-29T20:05:01.000Z"))).toBe(
      warmRetryKey(failed, "errored", Date.parse("2026-07-29T20:05:59.999Z")),
    );
  });

  test("does not replace a live workflow", () => {
    expect(
      warmRetryKey(
        status(),
        "running",
        Date.parse("2026-07-29T20:05:00.000Z"),
      ),
    ).toBe("");
  });

  test("retries a failed task even while its notification step is still running", () => {
    const failed = status({ state: "error" });
    expect(
      warmRetryKey(
        failed,
        "running",
        Date.parse(failed.updatedAt) + FAILED_WARM_RETRY_MS,
      ),
    ).toBe(failed.updatedAt);
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
