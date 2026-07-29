import weights from "./engine/config/valuation.json";

/// Mirrors ml/valuation_states.py: the same features in the same order, the
/// same normalisation, and the same three layers. Drift here is silent, so
/// valuation.test.ts pins it against vectors produced by the trainer.

const USE_GROUPS = weights.use_groups;

export interface ValuationInput {
  readonly state: string;
  readonly assessed: number;
  readonly improvementValue: number;
  readonly landValue: number;
  readonly sqft: number;
  readonly yearBuilt: number;
  readonly landArea: number;
  readonly use: string;
  readonly year: number;
}

export interface Valuation {
  readonly estimate: number;
  readonly low: number;
  readonly high: number;
  readonly state: string;
}

export const groupUse = (raw: string): string => {
  const low = raw.toLowerCase();
  if (/resid|condo|single|town home/.test(low)) return "residential";
  if (low.includes("apart")) return "apartments";
  if (low.includes("commerc")) return "commercial";
  if (low.includes("industr")) return "industrial";
  if (low.includes("vacant") || low.includes("land")) return "vacant";
  return "other";
};

/// A state whose assessment ratio was never learned cannot be valued: the
/// model would be extrapolating across assessment law it has not seen, which
/// is exactly the cross-state case that failed its holdout.
export const isCalibrated = (state: string): boolean =>
  Object.prototype.hasOwnProperty.call(weights.state_ratios, state.toUpperCase());

export const calibratedStates = (): readonly string[] =>
  Object.keys(weights.state_ratios).sort();

const features = (input: ValuationInput): readonly number[] => {
  const ratios: Record<string, number> = weights.state_ratios;
  const ratio = ratios[input.state.toUpperCase()] ?? 1;
  const assessed = Math.max(input.assessed, 1);
  const age =
    input.yearBuilt > 1700 && input.yearBuilt <= input.year
      ? input.year - input.yearBuilt
      : -1;
  const use = groupUse(input.use);
  return [
    Math.log(assessed * ratio),
    Math.log(assessed),
    input.improvementValue > 0 ? input.improvementValue / assessed : 0,
    input.landValue > 0 ? input.landValue / assessed : 0,
    input.improvementValue <= 0 ? 1 : 0,
    Math.log1p(input.sqft),
    input.sqft <= 100 ? 1 : 0,
    Math.log1p(input.sqft > 100 ? assessed / input.sqft : 0),
    Math.log1p(Math.max(age, 0)),
    age < 0 ? 1 : 0,
    Math.log1p(Math.max(input.landArea, 0)),
    (input.year - 2021) / 5,
    ...USE_GROUPS.map((g) => (use === g ? 1 : 0)),
  ];
};

const forward = (x: readonly number[]): number => {
  let current = [...x];
  weights.layers.forEach((layer, index) => {
    const next = layer.b.map((bias, j) => {
      let sum = bias;
      for (let i = 0; i < current.length; i += 1) sum += current[i]! * layer.w[i]![j]!;
      return sum;
    });
    current = index < weights.layers.length - 1 ? next.map((v) => (v > 0 ? v : 0)) : next;
  });
  return current[0] ?? 0;
};

export const estimate = (input: ValuationInput): Valuation | undefined => {
  if (!isCalibrated(input.state) || input.assessed <= 0) return undefined;
  const raw = features(input);
  const normalised = raw.map((v, i) => (v - weights.mean[i]!) / weights.std[i]!);
  // The training target is log(price / calibrated assessment); the same clamp
  // the trainer applies keeps a stray input from producing a silly number.
  const correction = Math.max(-3, Math.min(3, forward(normalised)));
  const ratios: Record<string, number> = weights.state_ratios;
  const base = input.assessed * (ratios[input.state.toUpperCase()] ?? 1);
  const value = base * Math.exp(correction);
  // The band is the model's own measured holdout error, not a guess.
  const spread = weights.holdout_mdape;
  return {
    estimate: value,
    low: value * (1 - spread),
    high: value * (1 + spread),
    state: input.state.toUpperCase(),
  };
};

export const holdoutError = (): number => weights.holdout_mdape;
