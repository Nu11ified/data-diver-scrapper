import { describe, expect, test } from "bun:test";

import { linkBlock, linksFor, placeOf } from "./links.ts";

describe("property lookup links", () => {
  test("turns a jurisdiction slug into something a map can search", () => {
    expect(placeOf("city_of_norfolk_va")).toBe("Norfolk VA");
    expect(placeOf("harris_county_tx")).toBe("Harris TX");
    expect(placeOf("")).toBe("");
  });

  test("builds searchable urls from the recorded address", () => {
    const links = linksFor("436 W 31ST ST", "city_of_norfolk_va");
    expect(links).toBeDefined();
    expect(links!.maps).toContain("436%20W%2031ST%20ST%2C%20Norfolk%20VA");
    expect(links!.zillow).toContain("436-W-31ST-ST-Norfolk-VA");
    // A url that would break if pasted is worse than no url.
    for (const url of Object.values(links!)) expect(url).not.toContain(" ");
  });

  test("no address means no links rather than a link to nowhere", () => {
    expect(linksFor("", "city_of_norfolk_va")).toBeUndefined();
    expect(linkBlock("   ", "city_of_norfolk_va")).toBe("");
  });
});
