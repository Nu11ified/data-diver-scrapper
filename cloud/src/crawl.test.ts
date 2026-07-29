import { describe, expect, test } from "bun:test";

import { disallowedPaths, linksFrom, resolveUrl } from "./crawl.ts";

describe("resolveUrl", () => {
  const base = "https://county.gov/tax/sales/june.html?year=2026";

  test("resolves relative, rooted and protocol-relative hrefs", () => {
    expect(resolveUrl(base, "list.html")).toBe("https://county.gov/tax/sales/list.html");
    expect(resolveUrl(base, "/parcels/1")).toBe("https://county.gov/parcels/1");
    expect(resolveUrl(base, "../roll.csv")).toBe("https://county.gov/tax/roll.csv");
    expect(resolveUrl(base, "//cdn.county.gov/a.json")).toBe("https://cdn.county.gov/a.json");
  });

  test("drops fragments and refuses schemes a crawler cannot follow", () => {
    expect(resolveUrl(base, "page.html#row3")).toBe("https://county.gov/tax/sales/page.html");
    expect(resolveUrl(base, "#top")).toBe("");
    expect(resolveUrl(base, "mailto:clerk@county.gov")).toBe("");
    expect(resolveUrl(base, "javascript:void(0)")).toBe("");
    expect(resolveUrl(base, "not a url at all %%%")).not.toContain("javascript");
  });
});

describe("linksFrom", () => {
  const page =
    "<html><body>" +
    "<a href='/help'>Help</a>" +
    "<table><tr><td><a href=\"/parcel/1\">101-22</a></td></tr></table>" +
    "<a href='/list?page=2' rel='next'>Next</a>" +
    "<a href=/list?page=3 >3</a>" +
    "</body></html>";

  test("finds links in quoted, double-quoted and bare href forms", () => {
    const links = linksFrom("https://county.gov/list", page, 20);
    expect(links).toContain("https://county.gov/help");
    expect(links).toContain("https://county.gov/parcel/1");
    expect(links).toContain("https://county.gov/list?page=2");
    expect(links).toContain("https://county.gov/list?page=3");
  });

  test("puts pagination first so a multi-page table is walked before depth", () => {
    const links = linksFrom("https://county.gov/list", page, 20);
    expect(links[0]).toContain("page=");
  });

  test("respects the limit and drops duplicates", () => {
    expect(linksFrom("https://county.gov/list", page, 2)).toHaveLength(2);
    const repeated = "<a href='/a'>x</a><a href='/a'>x</a>";
    expect(linksFrom("https://county.gov/", repeated, 10)).toHaveLength(1);
  });
});

describe("disallowedPaths", () => {
  test("reads the wildcard record only", () => {
    const robots =
      "User-agent: *\nDisallow: /private\nDisallow: /search\n\n" +
      "User-agent: EvilBot\nDisallow: /\n";
    expect(disallowedPaths(robots)).toEqual(["/private", "/search"]);
  });

  test("ignores comments and empty disallows", () => {
    expect(disallowedPaths("User-agent: *\n# nothing\nDisallow:\n")).toEqual([]);
    expect(disallowedPaths("")).toEqual([]);
  });
});
