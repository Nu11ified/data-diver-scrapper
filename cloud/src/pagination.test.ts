import { describe, expect, test } from "bun:test";

import { makeLimiter, type Limiter } from "./politeness.ts";
import {
  RECORD_CAP,
  appTokenHeaders,
  fetchPaginated,
  fetchSingle,
  isSocrataUrl,
  socrataLimit,
  withOffset,
  type PageFetch,
} from "./pagination.ts";

const alwaysAllow: Limiter = { take: async () => true, spent: () => 0 };

const jsonPage = (records: readonly unknown[], contentType = "application/json"): PageFetch => ({
  ok: true,
  status: 200,
  contentType,
  body: JSON.stringify(records),
});

describe("isSocrataUrl", () => {
  test("recognises the /resource/ marker the engine's harvester also keys on", () => {
    expect(isSocrataUrl("https://data.norfolk.gov/resource/abcd-1234.json")).toBe(true);
    expect(isSocrataUrl("https://www.tarrantcountytx.gov/en/constables/list.html")).toBe(false);
  });
});

describe("socrataLimit", () => {
  test("reads an explicit $limit", () => {
    expect(socrataLimit("https://data.norfolk.gov/resource/x.json?$limit=50")).toBe(50);
  });

  test("defaults to 1000 when absent, zero, negative or malformed", () => {
    expect(socrataLimit("https://data.norfolk.gov/resource/x.json")).toBe(1000);
    expect(socrataLimit("https://data.norfolk.gov/resource/x.json?$limit=0")).toBe(1000);
    expect(socrataLimit("https://data.norfolk.gov/resource/x.json?$limit=-5")).toBe(1000);
    expect(socrataLimit("https://data.norfolk.gov/resource/x.json?$limit=nope")).toBe(1000);
    expect(socrataLimit("not a url")).toBe(1000);
  });
});

describe("withOffset", () => {
  test("sets $limit and $offset without disturbing other query params", () => {
    const next = withOffset("https://data.norfolk.gov/resource/x.json?$order=name", 50, 100);
    const url = new URL(next);
    expect(url.searchParams.get("$limit")).toBe("50");
    expect(url.searchParams.get("$offset")).toBe("100");
    expect(url.searchParams.get("$order")).toBe("name");
  });
});

describe("appTokenHeaders", () => {
  test("sends X-App-Token only for Socrata urls with a configured token", () => {
    expect(appTokenHeaders("https://data.norfolk.gov/resource/x.json", "tok123")).toEqual({
      "X-App-Token": "tok123",
    });
  });

  test("an empty token adds no header", () => {
    expect(appTokenHeaders("https://data.norfolk.gov/resource/x.json", "")).toEqual({});
  });

  test("a non-Socrata url gets no header even with a token configured", () => {
    expect(appTokenHeaders("https://www.tarrantcountytx.gov/list.html", "tok123")).toEqual({});
  });
});

describe("fetchSingle", () => {
  test("returns the body on a plain successful fetch", async () => {
    const result = await fetchSingle("https://county.gov/list.html", alwaysAllow, async () => ({
      ok: true,
      status: 200,
      contentType: "text/html",
      body: "<html></html>",
    }));
    expect(result).toEqual({
      ok: true,
      body: "<html></html>",
      contentType: "text/html",
      truncated: false,
      pages: 1,
    });
  });

  test("reports the http status on a non-ok response", async () => {
    const result = await fetchSingle("https://county.gov/list.html", alwaysAllow, async () => ({
      ok: false,
      status: 503,
      contentType: "",
      body: "",
    }));
    expect(result.ok).toBe(false);
    expect(result.error).toBe("http status 503");
  });

  test("refuses when the host budget is spent", async () => {
    const denyAll: Limiter = { take: async () => false, spent: () => 99 };
    const result = await fetchSingle("https://county.gov/list.html", denyAll, async () => {
      throw new Error("should not be called");
    });
    expect(result).toEqual({
      ok: false,
      body: "",
      contentType: "",
      truncated: false,
      pages: 0,
      error: "host request budget spent; try again shortly",
    });
  });

  test("turns a network throw into a fetch-stage error instead of rejecting", async () => {
    const result = await fetchSingle("https://county.gov/list.html", alwaysAllow, async () => {
      throw new Error("DNS lookup failed");
    });
    expect(result.ok).toBe(false);
    expect(result.error).toBe("DNS lookup failed");
  });
});

describe("fetchPaginated", () => {
  const url = "https://data.norfolk.gov/resource/abcd-1234.json?$limit=2";

  test("pages through $offset in steps of $limit and stops on a short page", async () => {
    const calls: string[] = [];
    const pages = [
      jsonPage([{ id: 1 }, { id: 2 }]),
      jsonPage([{ id: 3 }, { id: 4 }]),
      jsonPage([{ id: 5 }]), // short page: the real end of the dataset
    ];
    const result = await fetchPaginated(url, alwaysAllow, async (pageUrl) => {
      calls.push(pageUrl);
      const page = pages[calls.length - 1];
      if (page === undefined) throw new Error("no more pages expected");
      return page;
    });
    expect(result.ok).toBe(true);
    expect(result.truncated).toBe(false);
    expect(result.pages).toBe(3);
    expect(JSON.parse(result.body)).toEqual([
      { id: 1 },
      { id: 2 },
      { id: 3 },
      { id: 4 },
      { id: 5 },
    ]);
    // page 0 keeps the caller's own url; later pages carry $offset in steps of $limit.
    expect(calls[0]).toBe(url);
    expect(new URL(calls[1] ?? "").searchParams.get("$offset")).toBe("2");
    expect(new URL(calls[2] ?? "").searchParams.get("$offset")).toBe("4");
  });

  test("fetches pages strictly sequentially, never overlapping", async () => {
    let inFlight = 0;
    let maxInFlight = 0;
    let calls = 0;
    const result = await fetchPaginated(url, alwaysAllow, async () => {
      calls += 1;
      inFlight += 1;
      maxInFlight = Math.max(maxInFlight, inFlight);
      await new Promise((resolve) => setTimeout(resolve, 1));
      inFlight -= 1;
      // Two full pages, then a short one, so three sequential fetches happen.
      return calls < 3 ? jsonPage([{ id: 1 }, { id: 2 }]) : jsonPage([{ id: 5 }]);
    });
    expect(result.ok).toBe(true);
    expect(calls).toBe(3);
    expect(maxInFlight).toBe(1);
  });

  test("can raise small configured pages without changing the record cap", async () => {
    const calls: string[] = [];
    const result = await fetchPaginated(
      url,
      alwaysAllow,
      async (pageUrl) => {
        calls.push(pageUrl);
        return calls.length === 1
          ? jsonPage(Array.from({ length: 1000 }, (_, id) => ({ id })))
          : jsonPage([{ id: 1000 }]);
      },
      RECORD_CAP,
      1000,
    );
    expect(result.ok).toBe(true);
    expect(result.truncated).toBe(false);
    expect(JSON.parse(result.body)).toHaveLength(1001);
    expect(new URL(calls[0] ?? "").searchParams.get("$limit")).toBe("1000");
    expect(new URL(calls[0] ?? "").searchParams.get("$offset")).toBe("0");
    expect(new URL(calls[1] ?? "").searchParams.get("$offset")).toBe("1000");
  });

  test("stops at the hard cap and reports the run as truncated", async () => {
    const bigUrl = "https://data.norfolk.gov/resource/abcd-1234.json?$limit=2000";
    let calls = 0;
    const result = await fetchPaginated(bigUrl, alwaysAllow, async () => {
      calls += 1;
      // Every page is full, so the source looks like it never runs out.
      return jsonPage(Array.from({ length: 2000 }, (_, i) => ({ id: calls * 10_000 + i })));
    });
    expect(result.ok).toBe(true);
    expect(result.truncated).toBe(true);
    expect(JSON.parse(result.body)).toHaveLength(RECORD_CAP);
    // 2000 + 2000 + 2000 would exceed the cap on the third page, so it stops there.
    expect(calls).toBe(3);
  });

  test("a cap reached by a genuinely final short page is not marked truncated", async () => {
    const smallCapUrl = "https://data.norfolk.gov/resource/abcd-1234.json?$limit=2";
    const result = await fetchPaginated(
      smallCapUrl,
      alwaysAllow,
      async () => jsonPage([{ id: 1 }]),
      1,
    );
    expect(result.ok).toBe(true);
    expect(result.truncated).toBe(false);
    expect(JSON.parse(result.body)).toEqual([{ id: 1 }]);
  });

  test("a first-page failure is a fetch error, not a truncated empty success", async () => {
    const result = await fetchPaginated(url, alwaysAllow, async () => ({
      ok: false,
      status: 500,
      contentType: "",
      body: "",
    }));
    expect(result.ok).toBe(false);
    expect(result.error).toBe("http status 500");
  });

  test("a later-page failure keeps what was already fetched and marks it truncated", async () => {
    let calls = 0;
    const result = await fetchPaginated(url, alwaysAllow, async () => {
      calls += 1;
      if (calls === 1) return jsonPage([{ id: 1 }, { id: 2 }]);
      return { ok: false, status: 502, contentType: "", body: "" };
    });
    expect(result.ok).toBe(true);
    expect(result.truncated).toBe(true);
    expect(JSON.parse(result.body)).toEqual([{ id: 1 }, { id: 2 }]);
  });

  test("a non-array first page is rejected as not a record set", async () => {
    const result = await fetchPaginated(url, alwaysAllow, async () => ({
      ok: true,
      status: 200,
      contentType: "text/html",
      body: "<html>not json</html>",
    }));
    expect(result.ok).toBe(false);
    expect(result.error).toBe("expected a json array of records");
  });

  test("running out of host budget mid-pagination truncates rather than fails", async () => {
    const limiter = makeLimiter({ perHost: 1, windowMs: 60_000, minGapMs: 0 });
    const result = await fetchPaginated(url, limiter, async () => jsonPage([{ id: 1 }, { id: 2 }]));
    expect(result.ok).toBe(true);
    expect(result.truncated).toBe(true);
    expect(result.pages).toBe(1);
  });
});
