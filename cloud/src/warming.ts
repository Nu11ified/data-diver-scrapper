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

export const shouldReuseWarm = (
  status: CountyWarmStatus | undefined,
  stamp: string,
  now: number,
): boolean =>
  status !== undefined &&
  status.stamp === stamp &&
  (status.state === "queued" || status.state === "running") &&
  now - Date.parse(status.updatedAt) < ACTIVE_WARM_MS;
