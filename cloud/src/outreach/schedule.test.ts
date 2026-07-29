import { describe, expect, test } from "bun:test";

import { classifyOwner, formatInZone, nextSendWindow, zoneFor } from "./schedule.ts";

const ET = "America/New_York";

describe("classifyOwner", () => {
  test("entity suffixes read as business", () => {
    expect(classifyOwner("OCEANVIEW HOLDINGS LLC")).toBe("business");
    expect(classifyOwner("Acme Co.")).toBe("business");
    expect(classifyOwner("CITY OF NORFOLK")).toBe("business");
    expect(classifyOwner("SMITH FAMILY TRUST")).toBe("business");
  });

  test("personal names read as person", () => {
    expect(classifyOwner("SMITH, JOHN A")).toBe("person");
    expect(classifyOwner("Maria Colon-Ortiz")).toBe("person");
    expect(classifyOwner("ESTATE OF MARY JONES")).toBe("person");
  });

  test("empty owner is unknown", () => {
    expect(classifyOwner("")).toBe("unknown");
    expect(classifyOwner("   ")).toBe("unknown");
  });
});

describe("zoneFor", () => {
  test("maps the jurisdiction state suffix", () => {
    expect(zoneFor("city_of_norfolk_va")).toBe("America/New_York");
    expect(zoneFor("harris_county_tx")).toBe("America/Chicago");
    expect(zoneFor("somewhere_unmapped_zz")).toBe("America/New_York");
  });
});

// 2026-07-28 is a Tuesday; America/New_York is UTC-4 in July.
describe("nextSendWindow", () => {
  test("business inside business hours sends now", () => {
    const now = new Date("2026-07-28T18:00:00Z");
    const window = nextSendWindow("business", now, ET);
    expect(window.at).toEqual(now);
    expect(window.reason).toContain("business hours");
  });

  test("business after close rolls to 9am next weekday", () => {
    const now = new Date("2026-07-28T23:00:00Z");
    const window = nextSendWindow("business", now, ET);
    expect(window.at.toISOString()).toBe("2026-07-29T13:00:00.000Z");
  });

  test("business on Saturday rolls to Monday 9am", () => {
    const now = new Date("2026-08-01T15:00:00Z");
    const window = nextSendWindow("business", now, ET);
    expect(window.at.toISOString()).toBe("2026-08-03T13:00:00.000Z");
  });

  test("person during work hours waits for the evening window", () => {
    const now = new Date("2026-07-28T18:00:00Z");
    const window = nextSendWindow("person", now, ET);
    expect(window.at.toISOString()).toBe("2026-07-28T21:30:00.000Z");
    expect(window.reason).toContain("after-work");
  });

  test("person late evening rolls to the next evening", () => {
    const now = new Date("2026-07-29T01:00:00Z");
    const window = nextSendWindow("person", now, ET);
    expect(window.at.toISOString()).toBe("2026-07-29T21:30:00.000Z");
  });

  test("person early Saturday waits for weekend daytime", () => {
    const now = new Date("2026-08-01T13:00:00Z");
    const window = nextSendWindow("person", now, ET);
    expect(window.at.toISOString()).toBe("2026-08-01T14:00:00.000Z");
    expect(window.reason).toContain("weekend");
  });

  test("person midday Saturday sends now", () => {
    const now = new Date("2026-08-01T16:00:00Z");
    expect(nextSendWindow("person", now, ET).at).toEqual(now);
  });
});

describe("formatInZone", () => {
  test("renders county-local time", () => {
    const text = formatInZone(new Date("2026-07-28T21:30:00Z"), ET);
    expect(text).toContain("Tue");
    expect(text).toContain("5:30");
    expect(text).toContain("EDT");
  });
});
