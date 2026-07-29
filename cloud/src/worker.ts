// The scraping service: TypeScript owns the network, the Data Diver WASM
// engine owns parsing, classification, matching and event building. The cron
// sweeps every source; the fetch API serves on-demand runs and compiled
// county payloads. Bodies and results land in R2 until the database slice;
// nothing here fabricates a result, and a failed source reports its stage.

import type { WorkerEnv } from "../alchemy.run.ts";
import {
  EngineError,
  processDocument,
  type ProcessedDocument,
  type PropertyEvent,
  compileCounty,
  engineVersion,
} from "./engine/datadiver.ts";
import schema from "./engine/config/schema.json";
import classifier from "./engine/config/source_classifier.json";
import columnModel from "./engine/config/column_model.json";
import sourcesConfig from "./engine/config/sources.json";

interface SourceConfig {
  readonly id: string;
  readonly name: string;
  readonly url: string;
  readonly jurisdiction: string;
  readonly as_of?: string;
}

interface RunOutcome {
  readonly sourceId: string;
  readonly ok: boolean;
  readonly stage: "fetch" | "engine" | "store" | "done";
  readonly error?: string;
  readonly classification?: string;
  readonly records?: number;
  readonly extractionRate?: number;
  readonly events?: number;
  readonly fetchMs?: number;
  readonly engineMs?: number;
}

const sources: readonly SourceConfig[] = sourcesConfig;
const schemaJson = JSON.stringify(schema);
const classifierJson = JSON.stringify(classifier);
const columnModelJson = JSON.stringify(columnModel);

const json = (value: unknown, status = 200): Response =>
  new Response(JSON.stringify(value, null, 1), {
    status,
    headers: { "content-type": "application/json" },
  });

const runSource = async (
  source: SourceConfig,
  env: WorkerEnv,
  runId: string,
): Promise<RunOutcome> => {
  const fetchStart = Date.now();
  let response: Response;
  try {
    response = await fetch(source.url, {
      headers: { "user-agent": "DataDiver/0.1 (public-record research)" },
    });
  } catch (cause) {
    return {
      sourceId: source.id,
      ok: false,
      stage: "fetch",
      error: cause instanceof Error ? cause.message : String(cause),
    };
  }
  if (!response.ok) {
    return {
      sourceId: source.id,
      ok: false,
      stage: "fetch",
      error: `http status ${response.status}`,
    };
  }
  const body = await response.text();
  const contentType = response.headers.get("content-type") ?? "";
  const fetchMs = Date.now() - fetchStart;

  const engineStart = Date.now();
  let processed: ProcessedDocument;
  try {
    processed = await processDocument({
      schemaJson,
      classifierJson,
      columnModelJson,
      contentType,
      body,
      sourceId: source.id,
      jurisdiction: source.jurisdiction,
      asOf: source.as_of ?? "",
      runId,
      nowIso: new Date().toISOString(),
    });
  } catch (cause) {
    return {
      sourceId: source.id,
      ok: false,
      stage: "engine",
      error: cause instanceof EngineError ? cause.message : String(cause),
      fetchMs,
    };
  }
  const engineMs = Date.now() - engineStart;

  try {
    await env.bucket.put(`bodies/${source.id}/${runId}`, body, {
      httpMetadata: { contentType: contentType || "application/octet-stream" },
    });
    await env.bucket.put(
      `events/${source.id}/${runId}.json`,
      JSON.stringify(processed.events),
      { httpMetadata: { contentType: "application/json" } },
    );
    await env.bucket.put(
      `runs/${source.id}/${runId}.json`,
      JSON.stringify({
        runId,
        sourceId: source.id,
        startedAt: new Date(fetchStart).toISOString(),
        fetchMs,
        engineMs,
        classification: processed.classification,
        classConfidence: processed.class_confidence,
        extractionRate: processed.extraction_rate,
        records: processed.records,
        fingerprint: processed.fingerprint,
        newestRecordDate: processed.newest_record_date,
        mapping: processed.mapping,
      }),
      { httpMetadata: { contentType: "application/json" } },
    );
  } catch (cause) {
    return {
      sourceId: source.id,
      ok: false,
      stage: "store",
      error: cause instanceof Error ? cause.message : String(cause),
      fetchMs,
      engineMs,
    };
  }

  return {
    sourceId: source.id,
    ok: true,
    stage: "done",
    classification: processed.classification,
    records: processed.records,
    extractionRate: processed.extraction_rate,
    events: processed.events.length,
    fetchMs,
    engineMs,
  };
};

const runAll = async (env: WorkerEnv): Promise<readonly RunOutcome[]> => {
  const runId = `run_${Date.now().toString(36)}`;
  return Promise.all(sources.map((source) => runSource(source, env, runId)));
};

const loadCountyEvents = async (
  env: WorkerEnv,
  county: string,
): Promise<readonly PropertyEvent[]> => {
  const events: PropertyEvent[] = [];
  const seen = new Set<string>();
  for (const source of sources) {
    const listing = await env.bucket.list({ prefix: `events/${source.id}/` });
    const newest = listing.objects.at(-1);
    if (newest === undefined) continue;
    const object = await env.bucket.get(newest.key);
    if (object === null) continue;
    const parsed = JSON.parse(await object.text()) as readonly PropertyEvent[];
    for (const event of parsed) {
      if (seen.has(event.id)) continue;
      seen.add(event.id);
      if (event.property_key.split("|")[0]?.includes(county)) events.push(event);
    }
  }
  return events;
};

export default {
  async fetch(request: Request, env: WorkerEnv): Promise<Response> {
    const url = new URL(request.url);

    if (url.pathname === "/health") {
      const version = await engineVersion();
      return json({ ok: true, ...version, sources: sources.length });
    }

    if (url.pathname === "/run" && request.method === "POST") {
      const outcomes = await runAll(env);
      return json({ ok: outcomes.every((o) => o.ok), outcomes });
    }

    const runMatch = /^\/run\/([a-z0-9_]+)$/.exec(url.pathname);
    if (runMatch !== null && request.method === "POST") {
      const source = sources.find((s) => s.id === runMatch[1]);
      if (source === undefined) return json({ ok: false, error: "unknown source" }, 404);
      const outcome = await runSource(source, env, `run_${Date.now().toString(36)}`);
      return json(outcome, outcome.ok ? 200 : 502);
    }

    const countyMatch = /^\/county\/([a-z0-9_]+)$/.exec(url.pathname);
    if (countyMatch !== null) {
      const county = countyMatch[1] ?? "";
      const events = await loadCountyEvents(env, county);
      if (events.length === 0) {
        return json({ ok: false, error: "no events for county; run the sources first" }, 404);
      }
      const payload = await compileCounty(schemaJson, events, {}, county);
      return new Response(payload, { headers: { "content-type": "application/json" } });
    }

    return json({ ok: false, error: "not found" }, 404);
  },

  async scheduled(_controller: unknown, env: WorkerEnv): Promise<void> {
    const outcomes = await runAll(env);
    const failed = outcomes.filter((o) => !o.ok);
    console.log(
      `cron sweep: ${outcomes.length - failed.length}/${outcomes.length} sources ok` +
        (failed.length > 0
          ? `; failed: ${failed.map((o) => `${o.sourceId}@${o.stage}`).join(", ")}`
          : ""),
    );
  },
};
