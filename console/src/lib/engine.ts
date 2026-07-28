/**
 * Typed server-side client for the C++ engine HTTP API (docs/architecture.md).
 * Every shape here mirrors the contract; the engine is the source of truth.
 * Server-only: routes call this, the browser talks only to Next API routes.
 */

const ENGINE_URL = process.env.ENGINE_URL ?? "http://127.0.0.1:8080";

/** Engine timestamps may arrive as ISO strings or unix epochs. */
export type EngineTime = string | number;

// ---------------------------------------------------------------------------
// Contract shapes
// ---------------------------------------------------------------------------

export interface RunRecord {
  id: string;
  source_id: string;
  started_at: EngineTime;
  ok: boolean;
  error: string | null;
  stage: string;
  http_status: number;
  bytes: number;
  fetch_ms: number;
  parse_ms: number;
  classify_ms: number;
  map_ms: number;
  total_ms: number;
  format: string;
  classification: string;
  class_confidence: number;
  records_extracted: number;
  events_new: number;
  extraction_rate: number;
  mapping_confidence: number;
  baseline_rate: number;
  drift_detected: boolean;
  repair_attempted: boolean;
  repair_accepted: boolean;
  structure_fingerprint: string;
  rss_bytes: number;
  cpu_ms: number;
}

export interface MappingField {
  field: string;
  source_label: string;
  label_similarity: number;
  value_pass_rate: number;
  confidence: number;
}

export interface Mapping {
  confidence: number;
  fields: MappingField[];
}

export interface Source {
  id: string;
  name: string;
  url: string;
  jurisdiction: string;
  added_at: EngineTime;
  enabled: boolean;
  demo: boolean;
  classification: string | null;
  has_mapping: boolean;
  mapping_confidence: number | null;
  baseline_rate: number | null;
  good_runs: number;
  mapping: Mapping | null;
  last_run: RunRecord | null;
}

export interface CountySummary {
  jurisdiction: string;
  slug: string;
  sources: number;
  ok_sources: number;
  properties: number;
  corroborated: number;
  events: number;
  repairs: number;
  avg_extraction: number;
  last_run_at: EngineTime | null;
}

export interface Overview {
  engine: {
    rss_bytes: number;
    peak_rss_bytes: number;
    cpu_ms: number;
    http_transport: string;
    now: EngineTime;
    benchmark_replays_cache?: boolean;
    auto_refresh_seconds?: number;
  };
  model: {
    trained_at: EngineTime | null;
    leave_one_out_accuracy: number;
    examples: number;
  };
  totals: {
    sources: number;
    runs: number;
    events: number;
    properties: number;
    repairs: number;
  };
  per_source: Array<{
    id: string;
    name: string;
    runs: number;
    ok_runs: number;
    bytes_total: number;
    avg_total_ms: number;
    avg_fetch_ms: number;
    last_run: RunRecord | null;
  }>;
}

export interface ModelInfo {
  trained_at: EngineTime | null;
  leave_one_out_accuracy: number;
  examples: number;
  vocabulary: number;
  kind: string;
  classes: Array<{
    name: string;
    documents: number;
    tokens: number;
    top_tokens: Array<{ token: string; count: number; lift: number }>;
  }>;
}

/** One extracted record: canonical fields plus `_completeness`. */
export type SnapshotRecord = Record<string, unknown> & {
  _completeness: number;
};

export interface Snapshot {
  at: EngineTime;
  run_id: string;
  format: string;
  container: string;
  fingerprint: string;
  labels: string[];
  raw_sample: Array<{ label: string; value: string }>;
  classification: {
    label: string;
    confidence: number;
    distribution: Array<{ label: string; probability: number }>;
  };
  mapping: Mapping;
  field_rates: Record<string, number>;
  records: SnapshotRecord[];
}

export interface SchemaResponse {
  source_id: string;
  name: string;
  jurisdiction: string;
  baseline_rate: number;
  good_runs: number;
  snapshot: Snapshot | null;
}

export interface PropertySummary {
  key: string;
  state: string;
  events: number;
  owner: string | null;
  address: string | null;
  parcel_id: string | null;
  last_event_date: string | null;
  sources: number;
}

export interface EngineEvent {
  id: string;
  property_key: string;
  kind: string;
  event_date: string | null;
  recorded_at: EngineTime;
  source_id: string | null;
  run_id: string;
  amount: number;
  confidence: number;
  details: Record<string, unknown>;
}

export interface PropertyDetail {
  key: string;
  state: string;
  transitions: Array<{ state: string; event_id: string; event_date: string }>;
  events: EngineEvent[];
}

export interface Repair {
  id: string;
  source_id: string;
  at: EngineTime;
  reason: string;
  before_mapping: Mapping | null;
  after_mapping: Mapping | null;
  before_rate: number;
  after_rate: number;
  confidence: number;
  accepted: boolean;
  /** auto | pending | approved | rejected (absent on legacy records). */
  resolution?: string;
  changes: unknown[];
}

export interface BenchmarkResult {
  rounds: number;
  runs: number;
  ok_runs: number;
  records_processed: number;
  events_new: number;
  bytes_processed: number;
  total_ms: number;
  runs_per_sec: number;
  records_per_sec: number;
  cpu_ms: number;
  rss_before_bytes: number;
  rss_after_bytes: number;
}

export interface TrainingReport {
  at: EngineTime;
  duration_ms: number;
  examples: number;
  classes: number;
  accuracy: number;
  per_class: Array<{ name: string; examples: number; correct: number }>;
  confusion: Array<{ actual: string; predicted: string; count: number }>;
}

export interface ExportProperty {
  key: string;
  jurisdiction_slug: string;
  state: string;
  transitions: Array<{ state: string; event_id: string; event_date: string }>;
  owner: string | null;
  address: string | null;
  parcel_id: string | null;
  last_event_date: string | null;
  sources: string[];
}

export interface ExportPayload {
  sources: Source[];
  runs: RunRecord[];
  events: EngineEvent[];
  repairs: Repair[];
  properties: ExportProperty[];
  counties: CountySummary[];
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

/** The engine process is unreachable (connection refused, DNS, timeout). */
export class EngineDownError extends Error {
  constructor(cause: unknown) {
    super(`engine unreachable at ${ENGINE_URL}`);
    this.name = "EngineDownError";
    this.cause = cause;
  }
}

/** The engine answered with a non-2xx status. */
export class EngineHttpError extends Error {
  constructor(
    public readonly status: number,
    public readonly body: string,
    path: string,
  ) {
    super(`engine ${path} responded ${status}: ${body.slice(0, 200)}`);
    this.name = "EngineHttpError";
  }
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

async function engineFetch<T>(path: string, init?: RequestInit): Promise<T> {
  let res: Response;
  try {
    res = await fetch(`${ENGINE_URL}${path}`, {
      cache: "no-store",
      ...init,
    });
  } catch (err) {
    throw new EngineDownError(err);
  }
  if (!res.ok) {
    throw new EngineHttpError(res.status, await res.text(), path);
  }
  return (await res.json()) as T;
}

function post<T>(path: string, body?: unknown): Promise<T> {
  return engineFetch<T>(path, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
}

// ---------------------------------------------------------------------------
// Endpoints
// ---------------------------------------------------------------------------

export function getOverview(): Promise<Overview> {
  return engineFetch("/api/overview");
}

export function getCounties(): Promise<CountySummary[]> {
  return engineFetch("/api/counties");
}

export function getModel(): Promise<ModelInfo> {
  return engineFetch("/api/model");
}

export function getSources(): Promise<Source[]> {
  return engineFetch("/api/sources");
}

export function getSchema(sourceId: string): Promise<SchemaResponse> {
  return engineFetch(`/api/schema?source=${encodeURIComponent(sourceId)}`);
}

export function getRuns(opts?: {
  limit?: number;
  source?: string;
}): Promise<RunRecord[]> {
  const q = new URLSearchParams();
  if (opts?.limit != null) q.set("limit", String(opts.limit));
  if (opts?.source) q.set("source", opts.source);
  const qs = q.toString();
  return engineFetch(`/api/runs${qs ? `?${qs}` : ""}`);
}

export function getRecords(sourceId: string): Promise<SnapshotRecord[]> {
  return engineFetch(`/api/records?source=${encodeURIComponent(sourceId)}`);
}

export function getProperties(): Promise<PropertySummary[]> {
  return engineFetch("/api/properties");
}

export function getProperty(key: string): Promise<PropertyDetail> {
  return engineFetch(`/api/property?key=${encodeURIComponent(key)}`);
}

export function getRepairs(sourceId?: string): Promise<Repair[]> {
  return engineFetch(
    sourceId
      ? `/api/repairs?source=${encodeURIComponent(sourceId)}`
      : "/api/repairs",
  );
}

export function createSource(body: {
  name: string;
  url: string;
  jurisdiction: string;
}): Promise<Source> {
  return post("/api/sources", body);
}

export function updateSource(body: {
  id: string;
  name?: string;
  url?: string;
  jurisdiction?: string;
  enabled?: boolean;
}): Promise<Source> {
  return post("/api/sources/update", body);
}

export function deleteSource(id: string): Promise<unknown> {
  return post("/api/sources/delete", { id });
}

export function runSource(sourceId: string): Promise<RunRecord> {
  return post(`/api/run?source=${encodeURIComponent(sourceId)}`);
}

export function runAll(): Promise<unknown> {
  return post("/api/run_all");
}

/**
 * Save operator mapping overrides (field -> source label, "" = force-unmap),
 * then run the source once. Returns that run's record.
 */
export function setMapping(
  source: string,
  overrides: Record<string, string>,
): Promise<RunRecord> {
  return post("/api/mapping", { source, overrides });
}

export function resolveRepair(id: string, approve: boolean): Promise<unknown> {
  return post("/api/repairs/resolve", { id, approve });
}

export function benchmark(rounds: number): Promise<BenchmarkResult> {
  return post(`/api/benchmark?rounds=${rounds}`);
}

export function train(): Promise<TrainingReport> {
  return post("/api/train");
}

/** Last saved training report; the engine 404s when none exists. */
export function getTrainReport(): Promise<TrainingReport> {
  return engineFetch("/api/train/report");
}

export function getExport(): Promise<ExportPayload> {
  return engineFetch("/api/export");
}
