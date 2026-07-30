import { describe, expect, test } from "bun:test";

import { bundledSeed } from "./seed.ts";
import {
  configFromRow,
  marketFromJurisdiction,
  parseSeed,
  planSeed,
  slugId,
} from "./sources.ts";

describe("marketFromJurisdiction", () => {
  test("turns a model jurisdiction into a county market", () => {
    expect(marketFromJurisdiction("denton_county_tx")).toBe("Denton County, TX");
  });

  test("keeps a human market and normalizes its state", () => {
    expect(marketFromJurisdiction("Denton, tx")).toBe("Denton, TX");
  });

  test("renders city-of jurisdictions without exposing a slug", () => {
    expect(marketFromJurisdiction("city_of_norfolk_va")).toBe(
      "City of Norfolk, VA",
    );
  });
});

describe("configFromRow", () => {
  const row = {
    id: "norfolk_delinquent",
    name: "Norfolk Delinquent Taxes",
    url: "https://data.norfolk.gov/resource/7qie-z5gv.json",
    jurisdiction: "city_of_norfolk_va",
    asOf: null,
    headers: null,
    method: null,
    body: null,
  };

  test("a row the database returns is a runnable source", () => {
    expect(configFromRow({ ...row, asOf: "2026-06-30" })).toEqual({
      id: row.id,
      name: row.name,
      url: row.url,
      jurisdiction: row.jurisdiction,
      as_of: "2026-06-30",
    });
  });

  test("an unset as_of is absent rather than null or empty", () => {
    const base = {
      id: row.id,
      name: row.name,
      url: row.url,
      jurisdiction: row.jurisdiction,
    };
    expect(configFromRow({ ...row, asOf: null })).toEqual(base);
    expect(configFromRow({ ...row, asOf: "" })).toEqual(base);
    expect("as_of" in configFromRow({ ...row, asOf: null })).toBe(false);
  });

  test("headers, method and body round-trip from the row when a source needs them", () => {
    const withRequestShape = configFromRow({
      ...row,
      headers: { "X-Api-Key": "secret", "X-Ignored": 7 },
      method: "POST",
      body: '{"query":"select * from parcels"}',
    });
    expect(withRequestShape.headers).toEqual({ "X-Api-Key": "secret" });
    expect(withRequestShape.method).toBe("POST");
    expect(withRequestShape.body).toBe('{"query":"select * from parcels"}');
  });

  test("a GET source with no headers or body carries none of those keys", () => {
    const plain = configFromRow(row);
    expect("headers" in plain).toBe(false);
    expect("method" in plain).toBe(false);
    expect("body" in plain).toBe(false);
  });

  test("a method other than POST collapses to the GET default", () => {
    expect(configFromRow({ ...row, method: "DELETE" })).toEqual({
      id: row.id,
      name: row.name,
      url: row.url,
      jurisdiction: row.jurisdiction,
    });
  });

  test("empty headers and blank body are absent, not empty", () => {
    expect(configFromRow({ ...row, headers: {}, body: "" })).toEqual({
      id: row.id,
      name: row.name,
      url: row.url,
      jurisdiction: row.jurisdiction,
    });
  });
});

describe("parseSeed", () => {
  const good = {
    id: "cook_tax_sale",
    name: "Cook County IL Annual Tax Sale",
    url: "https://datacatalog.cookcountyil.gov/resource/55ju-2fs9.json",
    jurisdiction: "Cook County IL",
  };

  test("keeps well-formed entries and normalises the id", () => {
    const parsed = parseSeed([{ ...good, id: "Cook Tax-Sale " }]);
    expect(parsed.rejected).toEqual([]);
    expect(parsed.sources).toEqual([{ ...good, id: "cook_tax_sale" }]);
  });

  test("carries as_of through when the seed states one", () => {
    expect(parseSeed([{ ...good, as_of: "2026-01-15" }]).sources[0]?.as_of).toBe("2026-01-15");
  });

  test("carries headers, method and body through for a source that needs auth", () => {
    const parsed = parseSeed([
      { ...good, headers: { "X-Api-Key": "secret" }, method: "post", body: "q=1" },
    ]);
    expect(parsed.rejected).toEqual([]);
    expect(parsed.sources[0]?.headers).toEqual({ "X-Api-Key": "secret" });
    expect(parsed.sources[0]?.method).toBe("POST");
    expect(parsed.sources[0]?.body).toBe("q=1");
  });

  test("a plain GET source carries none of the request-shape keys", () => {
    const source = parseSeed([good]).sources[0];
    expect(source).toBeDefined();
    expect("headers" in (source ?? {})).toBe(false);
    expect("method" in (source ?? {})).toBe(false);
    expect("body" in (source ?? {})).toBe(false);
  });

  test("rejects headers that are not an object of strings", () => {
    expect(parseSeed([{ ...good, headers: "not-an-object" }]).rejected).toEqual([
      "cook_tax_sale: headers must be an object of strings",
    ]);
    expect(parseSeed([{ ...good, headers: { key: 7 } }]).rejected).toEqual([
      "cook_tax_sale: headers must be an object of strings",
    ]);
  });

  test("rejects a method that is neither GET nor POST", () => {
    expect(parseSeed([{ ...good, method: "PUT" }]).rejected).toEqual([
      "cook_tax_sale: method must be GET or POST",
    ]);
  });

  test("refuses entries it cannot run and says why", () => {
    const parsed = parseSeed([
      good,
      { ...good, id: "no_url", url: "http://insecure.example.gov/list" },
      { ...good, id: "", url: "https://x.gov/a" },
      { ...good, id: "no_name", name: "  " },
      { ...good, id: "no_place", jurisdiction: "" },
      "not an object",
    ]);
    expect(parsed.sources.map((s) => s.id)).toEqual(["cook_tax_sale"]);
    expect(parsed.rejected).toEqual([
      "no_url: url is not https",
      "entry 2: id is missing",
      "no_name: name is missing",
      "no_place: jurisdiction is missing",
      "entry 5: not an object",
    ]);
  });

  test("anything that is not a list of sources yields no sources at all", () => {
    for (const raw of [undefined, null, 7, "[]", { id: "x" }]) {
      expect(parseSeed(raw).sources).toEqual([]);
      expect(parseSeed(raw).rejected).toEqual(["seed is not an array"]);
    }
  });
});

describe("planSeed", () => {
  const seed = parseSeed([
    { id: "a", name: "A", url: "https://a.gov/a.json", jurisdiction: "A County" },
    { id: "b", name: "B", url: "https://b.gov/b.json", jurisdiction: "B County" },
    { id: "c", name: "C", url: "https://c.gov/c.json", jurisdiction: "C County" },
  ]).sources;

  test("imports only what the database does not already hold", () => {
    const plan = planSeed(seed, ["b"]);
    expect(plan.create.map((s) => s.id)).toEqual(["a", "c"]);
    expect(plan.skipped).toEqual(["b"]);
  });

  test("a second import of the same seed changes nothing", () => {
    const first = planSeed(seed, []);
    expect(first.create).toHaveLength(3);
    const second = planSeed(
      seed,
      first.create.map((s) => s.id),
    );
    expect(second.create).toEqual([]);
    expect(second.skipped).toEqual(["a", "b", "c"]);
  });

  test("a seed that repeats an id imports it once", () => {
    const plan = planSeed([...seed, ...seed], []);
    expect(plan.create.map((s) => s.id)).toEqual(["a", "b", "c"]);
    expect(plan.skipped).toEqual(["a", "b", "c"]);
  });

  test("a discovered source already in the database is never overwritten", () => {
    const discovered = "b";
    expect(planSeed(seed, [discovered]).create.map((s) => s.id)).not.toContain(discovered);
  });
});

describe("slugId", () => {
  test("turns whatever the model named a source into a usable id", () => {
    expect(slugId("Harris County, TX — Tax Roll")).toBe("harris_county__tx___tax_roll");
    expect(slugId("  Leading ")).toBe("leading");
    expect(slugId("!!!")).toBe("");
  });
});

describe("the bundled seed file", () => {
  test("is optional but importable, and every entry is runnable", () => {
    expect(bundledSeed.rejected).toEqual([]);
    expect(bundledSeed.sources.length).toBeGreaterThan(0);
    for (const source of bundledSeed.sources) {
      expect(source.url.startsWith("https://")).toBe(true);
      expect(slugId(source.id)).toBe(source.id);
    }
  });

  test("seeding it twice is a no-op the second time", () => {
    const first = planSeed(bundledSeed.sources, []);
    expect(first.create).toHaveLength(bundledSeed.sources.length);
    expect(planSeed(bundledSeed.sources, first.create.map((s) => s.id)).create).toEqual([]);
  });
});
