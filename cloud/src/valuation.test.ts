import { describe, expect, test } from "bun:test";

import vectors from "./valuation.vectors.json";
import { calibratedStates, estimate, groupUse, holdoutError, isCalibrated } from "./valuation.ts";

describe("valuation parity with the trainer", () => {
  test("reproduces every vector the trainer produced", () => {
    expect(vectors.length).toBeGreaterThan(0);
    for (const v of vectors) {
      const got = estimate({
        state: v.state,
        assessed: v.assessed,
        improvementValue: v.improvementValue,
        landValue: v.landValue,
        sqft: v.sqft,
        yearBuilt: v.yearBuilt,
        landArea: v.landArea,
        use: v.use,
        year: v.year,
      });
      expect(got).toBeDefined();
      // The trainer normalises in float32 and this runs in float64, so the
      // floor here is about 1e-6 of pure precision. What the check exists to
      // catch - a reordered feature, a transposed layer, a stale ratio - moves
      // the estimate by percent, so 1e-4 is still three orders inside it.
      expect(Math.abs(got!.estimate - v.expected) / v.expected).toBeLessThan(1e-4);
    }
  });
});

describe("valuation refuses what it cannot know", () => {
  test("an uncalibrated state gets no number at all", () => {
    expect(isCalibrated("TX")).toBe(false);
    expect(
      estimate({
        state: "TX",
        assessed: 250_000,
        improvementValue: 180_000,
        landValue: 70_000,
        sqft: 1800,
        yearBuilt: 1990,
        landArea: 7000,
        use: "residential",
        year: 2026,
      }),
    ).toBeUndefined();
  });

  test("the calibrated states are the ones the model was trained on", () => {
    expect(calibratedStates()).toEqual(["CT", "MD", "VA"]);
    for (const state of calibratedStates()) expect(isCalibrated(state)).toBe(true);
    expect(isCalibrated("va")).toBe(true); // case is not a reason to refuse
  });

  test("a property with no assessment cannot be valued", () => {
    expect(
      estimate({
        state: "VA",
        assessed: 0,
        improvementValue: 0,
        landValue: 0,
        sqft: 0,
        yearBuilt: 0,
        landArea: 0,
        use: "residential",
        year: 2026,
      }),
    ).toBeUndefined();
  });
});

describe("valuation band and use grouping", () => {
  test("the band is the measured holdout error, not a guess", () => {
    const got = estimate({
      state: "VA",
      assessed: 239_800,
      improvementValue: 161_300,
      landValue: 78_500,
      sqft: 1528,
      yearBuilt: 1942,
      landArea: 7692,
      use: "residential",
      year: 2026,
    });
    expect(got).toBeDefined();
    expect(holdoutError()).toBeGreaterThan(0.05);
    expect(holdoutError()).toBeLessThan(0.30);
    expect(got!.low).toBeCloseTo(got!.estimate * (1 - holdoutError()), 6);
    expect(got!.high).toBeCloseTo(got!.estimate * (1 + holdoutError()), 6);
    expect(got!.low).toBeLessThan(got!.estimate);
  });

  test("use strings collapse the way the trainer collapsed them", () => {
    expect(groupUse("Single Family - Detached")).toBe("residential");
    expect(groupUse("Condominium")).toBe("residential");
    expect(groupUse("Exempt Commercial (EC)")).toBe("commercial");
    expect(groupUse("Residential Vacant lot")).toBe("residential");
    expect(groupUse("Apartments")).toBe("apartments");
    expect(groupUse("something unheard of")).toBe("other");
  });
});
