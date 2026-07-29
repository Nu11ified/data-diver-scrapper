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
