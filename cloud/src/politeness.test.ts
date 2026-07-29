import { describe, expect, test } from "bun:test";

import { makeLimiter } from "./politeness.ts";

const clock = () => {
  let at = 1_000;
  return {
    now: () => at,
    advance: (ms: number) => {
      at += ms;
    },
    sleep: async (ms: number) => {
      at += ms;
    },
  };
};

describe("host budget", () => {
  test("spends the budget then refuses, per host", async () => {
    const c = clock();
    const limiter = makeLimiter({ perHost: 3, windowMs: 60_000, minGapMs: 0 }, c.now, c.sleep);
    for (let i = 0; i < 3; i += 1) {
      expect(await limiter.take("https://county.gov/a")).toBe(true);
    }
    expect(await limiter.take("https://county.gov/b")).toBe(false);
    // A different host has its own budget.
    expect(await limiter.take("https://other.gov/a")).toBe(true);
    expect(limiter.spent("county.gov")).toBe(3);
  });

  test("the budget refills once the window passes", async () => {
    const c = clock();
    const limiter = makeLimiter({ perHost: 1, windowMs: 1_000, minGapMs: 0 }, c.now, c.sleep);
    expect(await limiter.take("https://county.gov/a")).toBe(true);
    expect(await limiter.take("https://county.gov/a")).toBe(false);
    c.advance(1_001);
    expect(await limiter.take("https://county.gov/a")).toBe(true);
  });

  test("waits out the minimum gap between requests to one host", async () => {
    const c = clock();
    const limiter = makeLimiter({ perHost: 10, windowMs: 60_000, minGapMs: 250 }, c.now, c.sleep);
    await limiter.take("https://county.gov/a");
    const before = c.now();
    await limiter.take("https://county.gov/b");
    expect(c.now() - before).toBeGreaterThanOrEqual(250);
  });

  test("host comparison ignores case, and a bad url is refused", async () => {
    const c = clock();
    const limiter = makeLimiter({ perHost: 1, windowMs: 60_000, minGapMs: 0 }, c.now, c.sleep);
    expect(await limiter.take("https://County.GOV/a")).toBe(true);
    expect(await limiter.take("https://county.gov/b")).toBe(false);
    expect(await limiter.take("not a url")).toBe(false);
  });
});
