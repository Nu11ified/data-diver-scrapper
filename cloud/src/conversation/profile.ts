export type EvidencePolicy =
  | "multiple_sources"
  | "open_violation"
  | "both"
  | "neither";

export interface AcquisitionProfile {
  readonly county?: string;
  readonly minOwed?: number;
  readonly minAssessed?: number;
  readonly evidence?: EvidencePolicy;
  readonly maxDaysSinceEvent?: number;
  readonly recencyAnswered?: boolean;
  readonly requireApproval?: boolean;
}

export interface ProfileUpdate {
  readonly county?: string;
  readonly minOwed?: number;
  readonly minAssessed?: number;
  readonly evidence?: EvidencePolicy;
  readonly maxDaysSinceEvent?: number;
  readonly anyEventAge?: boolean;
  readonly requireApproval?: boolean;
}

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
  if (profile?.county === undefined) return "county";
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
