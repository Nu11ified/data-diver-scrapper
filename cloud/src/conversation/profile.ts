import { DEFAULT_SPEC } from "../decision/graph.ts";

export type EvidencePolicy =
  | "multiple_sources"
  | "open_violation"
  | "both"
  | "neither";

export interface AcquisitionProfile {
  readonly markets?: readonly string[];
  readonly county?: string;
  readonly minOwed?: number;
  readonly minAssessed?: number;
  readonly evidence?: EvidencePolicy;
  readonly maxDaysSinceEvent?: number;
  readonly recencyAnswered?: boolean;
  readonly requireApproval?: boolean;
}

export interface ProfileUpdate {
  readonly markets?: readonly string[];
  readonly county?: string;
  readonly minOwed?: number;
  readonly minAssessed?: number;
  readonly evidence?: EvidencePolicy;
  readonly maxDaysSinceEvent?: number;
  readonly anyEventAge?: boolean;
  readonly requireApproval?: boolean;
}

export const DEFAULT_ACQUISITION_PROFILE: AcquisitionProfile = {
  minOwed: DEFAULT_SPEC.minOwed,
  minAssessed: 0,
  evidence: "multiple_sources",
  recencyAnswered: true,
  requireApproval: true,
};

export const applyProfileUpdate = (
  current: AcquisitionProfile | undefined,
  update: ProfileUpdate,
  completeWithDefaults = false,
): AcquisitionProfile => {
  const base = {
    ...(completeWithDefaults ? DEFAULT_ACQUISITION_PROFILE : {}),
    ...current,
  };
  const maxDaysSinceEvent =
    update.anyEventAge === true
      ? undefined
      : update.maxDaysSinceEvent ?? base.maxDaysSinceEvent;
  const recencyAnswered =
    update.anyEventAge === true || update.maxDaysSinceEvent !== undefined
      ? true
      : base.recencyAnswered;
  const markets =
    update.markets ??
    (update.county === undefined ? base.markets : [update.county]);
  const county = markets?.[0] ?? update.county ?? base.county;
  const minOwed = update.minOwed ?? base.minOwed;
  const minAssessed = update.minAssessed ?? base.minAssessed;
  const evidence = update.evidence ?? base.evidence;
  const requireApproval = update.requireApproval ?? base.requireApproval;
  return {
    ...(markets === undefined ? {} : { markets }),
    ...(county === undefined ? {} : { county }),
    ...(minOwed === undefined ? {} : { minOwed }),
    ...(minAssessed === undefined ? {} : { minAssessed }),
    ...(evidence === undefined ? {} : { evidence }),
    ...(maxDaysSinceEvent === undefined ? {} : { maxDaysSinceEvent }),
    ...(recencyAnswered === undefined ? {} : { recencyAnswered }),
    ...(requireApproval === undefined ? {} : { requireApproval }),
  };
};

export type ProfileField =
  | "county"
  | "minOwed"
  | "minAssessed"
  | "evidence"
  | "recency"
  | "requireApproval";

export const nextProfileField = (
  profile: AcquisitionProfile | undefined,
): ProfileField | undefined => {
  if (
    (profile?.markets === undefined || profile.markets.length === 0) &&
    profile?.county === undefined
  ) {
    return "county";
  }
  if (profile.minOwed === undefined) return "minOwed";
  if (profile.minAssessed === undefined) return "minAssessed";
  if (profile.evidence === undefined) return "evidence";
  if (profile.recencyAnswered !== true) return "recency";
  if (profile.requireApproval === undefined) return "requireApproval";
  return undefined;
};

export const legacyProfileText = (
  update: ProfileUpdate,
  fallback: string,
): string => {
  if (update.markets !== undefined) return update.markets.join(" and ");
  if (update.county !== undefined) return update.county;
  if (update.minOwed !== undefined) return String(update.minOwed);
  if (update.minAssessed !== undefined) return String(update.minAssessed);
  if (update.evidence !== undefined) {
    return update.evidence.replace(/_/g, " ");
  }
  if (update.maxDaysSinceEvent !== undefined) {
    return `${update.maxDaysSinceEvent} days`;
  }
  if (update.anyEventAge === true) return "any";
  if (update.requireApproval !== undefined) {
    return update.requireApproval ? "yes" : "no";
  }
  return fallback;
};

export const marketsOf = (
  profile: AcquisitionProfile | undefined,
): readonly string[] =>
  profile?.markets !== undefined && profile.markets.length > 0
    ? profile.markets
    : profile?.county === undefined
      ? []
      : [profile.county];
