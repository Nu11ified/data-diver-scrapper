import * as Schema from "effect/Schema";

export const CountyWarmStatus = Schema.Struct({
  county: Schema.String,
  canonical: Schema.String,
  stamp: Schema.String,
  state: Schema.Literals(["queued", "running", "complete", "error"]),
  instanceId: Schema.String,
  requestedAt: Schema.String,
  updatedAt: Schema.String,
  properties: Schema.optional(Schema.Number),
  error: Schema.optional(Schema.String),
});
export type CountyWarmStatus = (typeof CountyWarmStatus)["Type"];

export const ACTIVE_WARM_MS = 20 * 60_000;
export const FAILED_WARM_RETRY_MS = 60_000;

export const shouldReuseWarm = (
  status: CountyWarmStatus | undefined,
  now: number,
): boolean =>
  status !== undefined &&
  (status.state === "queued" || status.state === "running") &&
  now - Date.parse(status.updatedAt) < ACTIVE_WARM_MS;

export const countyWorkflowId = async (
  canonical: string,
  stamp: string,
  abi: number,
  retry = "",
  coverage = "",
): Promise<string> => {
  const retryIdentity =
    retry === ""
      ? `${canonical}:${stamp}:${abi}`
      : `${canonical}:${stamp}:${abi}:${retry}`;
  const identity =
    coverage === "" ? retryIdentity : `${retryIdentity}:coverage:${coverage}`;
  const bytes = new TextEncoder().encode(identity);
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  const suffix = [...digest.subarray(0, 10)]
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");
  return `county-${canonical.slice(0, 60)}-${suffix}`;
};
