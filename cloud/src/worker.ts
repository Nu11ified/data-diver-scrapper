
import * as Cloudflare from "alchemy/Cloudflare";
import * as Config from "effect/Config";
import * as Effect from "effect/Effect";
import * as Redacted from "effect/Redacted";
import * as HttpServerRequest from "effect/unstable/http/HttpServerRequest";
import * as HttpServerResponse from "effect/unstable/http/HttpServerResponse";

import { Bucket } from "./resources.ts";
import { makeClient, type Db } from "./db.ts";
import {
  ConversationThread,
  ConversationThreadLive,
  type PropertyMatch,
} from "./conversation/thread.ts";
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
}

interface CountyRecord {
  readonly keys: readonly string[];
  readonly lifecycle_state: string;
  readonly fields: Readonly<
    Record<string, { readonly value: string; readonly source: string }>
  >;
  readonly signals: {
    readonly delinquent_amount?: number;
    readonly assessed_value?: number;
    readonly debt_to_value?: number;
    readonly code_violations?: number;
  };
  readonly events: ReadonlyArray<{ readonly source: string }>;
}

const sources: readonly SourceConfig[] = sourcesConfig;
const schemaJson = JSON.stringify(schema);
const classifierJson = JSON.stringify(classifier);
const columnModelJson = JSON.stringify(columnModel);

const json = (value: unknown, status = 200): HttpServerResponse.HttpServerResponse =>
  HttpServerResponse.text(JSON.stringify(value, null, 1), {
    status,
    headers: { "content-type": "application/json" },
  });

export default class Scraper extends Cloudflare.Worker<Scraper>()(
  "Scraper",
  {
    main: import.meta.url,
    env: { DATABASE_URL: Config.redacted("DATABASE_URL") },
  },
  Effect.gen(function* () {
    const bucket = yield* Cloudflare.R2.ReadWriteBucket(Bucket);
    const threads = yield* ConversationThread;
    const databaseUrl = yield* Config.redacted("DATABASE_URL");
    let client: Db | undefined;
    const db = (): Db => (client ??= makeClient(Redacted.value(databaseUrl)));

    const runSource = (source: SourceConfig, runId: string) =>
      Effect.gen(function* () {
        const fetchStart = Date.now();
        const response = yield* Effect.tryPromise({
          try: () =>
            fetch(source.url, {
              headers: { "user-agent": "DataDiver/0.1 (public-record research)" },
            }),
          catch: (cause): Error =>
            cause instanceof Error ? cause : new Error(String(cause)),
        }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
        if (response instanceof Error || !response.ok) {
          return {
            sourceId: source.id,
            ok: false,
            stage: "fetch",
            error:
              response instanceof Error
                ? response.message
                : `http status ${response.status}`,
          } satisfies RunOutcome;
        }
        const body: string = yield* Effect.promise(() => response.text());
        const contentType = response.headers.get("content-type") ?? "";
        const fetchMs = Date.now() - fetchStart;

        const processed = yield* Effect.tryPromise({
          try: (): Promise<ProcessedDocument> =>
            processDocument({
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
            }),
          catch: (cause) =>
            new EngineError(cause instanceof Error ? cause.message : String(cause)),
        }).pipe(Effect.catch((cause: EngineError) => Effect.succeed(cause)));
        if (processed instanceof EngineError) {
          return {
            sourceId: source.id,
            ok: false,
            stage: "engine",
            error: processed.message,
            fetchMs,
          } satisfies RunOutcome;
        }

        yield* bucket.put(`bodies/${source.id}/${runId}`, body);

        const stored = yield* Effect.tryPromise({
          try: async () => {
            const prisma = db();
            await prisma.source.upsert({
              where: { id: source.id },
              create: {
                id: source.id,
                name: source.name,
                url: source.url,
                jurisdiction: source.jurisdiction,
                asOf: source.as_of ?? null,
              },
              update: { name: source.name, url: source.url },
            });
            await prisma.run.create({
              data: {
                id: `${runId}_${source.id}`,
                sourceId: source.id,
                startedAt: new Date(fetchStart),
                ok: true,
                stage: "done",
                classification: processed.classification,
                classConfidence: processed.class_confidence,
                extractionRate: processed.extraction_rate,
                records: processed.records,
                fingerprint: processed.fingerprint,
                newestRecordDate: processed.newest_record_date,
                fetchMs,
                mapping: JSON.parse(JSON.stringify(processed.mapping)) as object,
              },
            });
            await prisma.property.createMany({
              data: [
                ...new Map(
                  processed.events.map((e) => [
                    e.property_key,
                    {
                      key: e.property_key,
                      jurisdiction: source.jurisdiction,
                      lifecycleState: "NORMAL",
                      fields: {},
                      conflicts: [],
                      mergedKeys: [],
                    },
                  ]),
                ).values(),
              ],
              skipDuplicates: true,
            });
            await prisma.event.createMany({
              data: processed.events.map((e) => ({
                id: e.id,
                propertyKey: e.property_key,
                sourceId: source.id,
                runId: `${runId}_${source.id}`,
                kind: e.kind,
                eventDate: e.event_date || null,
                recordedAt: new Date(e.recorded_at),
                asOf: e.as_of || null,
                amount: e.amount,
                confidence: e.confidence,
                details: JSON.parse(JSON.stringify(e.details)) as object,
              })),
              skipDuplicates: true,
            });
          },
          catch: (cause): Error => (cause instanceof Error ? cause : new Error(String(cause))),
        }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
        if (stored instanceof Error) {
          return {
            sourceId: source.id,
            ok: false,
            stage: "store",
            error: stored.message,
            fetchMs,
          } satisfies RunOutcome;
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
        } satisfies RunOutcome;
      });

    const runAll = Effect.gen(function* () {
      const runId = `run_${Date.now().toString(36)}`;
      return yield* Effect.all(
        sources.map((source) => runSource(source, runId)),
        { concurrency: 6 },
      );
    });

    const loadCountyEvents = (county: string) =>
      Effect.promise(async (): Promise<readonly PropertyEvent[]> => {
        const rows = await db().event.findMany({
          where: { propertyKey: { contains: county, mode: "insensitive" } },
          orderBy: { eventDate: "asc" },
          take: 20_000,
        });
        return rows.map((row) => ({
          id: row.id,
          property_key: row.propertyKey,
          kind: row.kind,
          event_date: row.eventDate ?? "",
          recorded_at: row.recordedAt.toISOString(),
          source_id: row.sourceId,
          as_of: row.asOf ?? "",
          run_id: row.runId,
          amount: row.amount === null ? 0 : Number(row.amount),
          confidence: row.confidence,
          details: row.details as Record<string, string>,
        }));
      });

    const countyMatches = (county: string) =>
      Effect.gen(function* () {
        const events = yield* loadCountyEvents(county);
        if (events.length === 0) return [] as readonly PropertyMatch[];
        const payload = yield* Effect.promise(() =>
          compileCounty(schemaJson, events, {}, county),
        );
        const parsed = JSON.parse(payload) as { readonly records: readonly CountyRecord[] };
        return parsed.records
          .map((record): PropertyMatch => {
            const sourceIds = new Set(record.events.map((e) => e.source));
            return {
              propertyKey: record.keys[0] ?? "",
              address: record.fields.address?.value ?? "",
              owner: record.fields.owner?.value ?? "",
              lifecycleState: record.lifecycle_state,
              owed: record.signals.delinquent_amount ?? 0,
              assessed: record.signals.assessed_value ?? 0,
              debtToValue: record.signals.debt_to_value ?? 0,
              violations: record.signals.code_violations ?? 0,
              sources: sourceIds.size,
            };
          })
          .filter((match) => match.address !== "");
      });

    yield* Cloudflare.Workers.cron("0 * * * *", () =>
      Effect.gen(function* () {
        const outcomes = yield* runAll;
        const failed = outcomes.filter((o) => !o.ok);
        yield* Effect.log(
          `cron sweep: ${outcomes.length - failed.length}/${outcomes.length} ok` +
            (failed.length > 0
              ? `; failed: ${failed.map((o) => `${o.sourceId}@${o.stage}`).join(", ")}`
              : ""),
        );
      }),
    );

    const handleFetch = Effect.gen(function* () {
        const request = yield* HttpServerRequest.HttpServerRequest;
        const url = new URL(request.url, "http://worker");

        if (url.pathname === "/health") {
          const version = yield* Effect.tryPromise({
            try: () => engineVersion(),
            catch: (cause): Error =>
              cause instanceof Error ? cause : new Error(String(cause)),
          }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
          if (version instanceof Error) {
            return json({ ok: false, error: version.message }, 500);
          }
          return json({ ok: true, ...version, sources: sources.length });
        }

        if (url.pathname === "/run" && request.method === "POST") {
          const outcomes = yield* runAll;
          return json({ ok: outcomes.every((o) => o.ok), outcomes });
        }

        const runMatch = /^\/run\/([a-z0-9_]+)$/.exec(url.pathname);
        if (runMatch !== null && request.method === "POST") {
          const source = sources.find((s) => s.id === runMatch[1]);
          if (source === undefined) return json({ ok: false, error: "unknown source" }, 404);
          const outcome = yield* runSource(source, `run_${Date.now().toString(36)}`);
          return json(outcome, outcome.ok ? 200 : 502);
        }

        const countyMatch = /^\/county\/([a-z0-9_]+)$/.exec(url.pathname);
        if (countyMatch !== null) {
          const county = countyMatch[1] ?? "";
          const events = yield* loadCountyEvents(county);
          if (events.length === 0) {
            return json({ ok: false, error: "no events; run the sources first" }, 404);
          }
          const payload = yield* Effect.promise(() =>
            compileCounty(schemaJson, events, {}, county),
          );
          return HttpServerResponse.text(payload, {
            headers: { "content-type": "application/json" },
          });
        }

        if (url.pathname === "/sms" && request.method === "POST") {
          const bodyText = yield* request.text;
          const parsed = JSON.parse(bodyText) as {
            readonly from_number?: string;
            readonly content?: string;
          };
          const phone = (parsed.from_number ?? "").replace(/[^+0-9]/g, "");
          const text = parsed.content ?? "";
          if (phone === "" || text === "") {
            return json({ ok: false, error: "from_number and content are required" }, 400);
          }
          const candidates = yield* countyMatches("norfolk");
          const thread = threads.getByName(phone);
          const reply = yield* thread.handleMessage({ text, candidates });
          return json({ ok: true, phone, reply });
        }

        return json({ ok: false, error: "not found" }, 404);
      });

    return {
      fetch: handleFetch.pipe(
        Effect.catchTag("R2Error", (cause) =>
          Effect.succeed(json({ ok: false, error: cause.message }, 500)),
        ),
      ),
    };
  }).pipe(
    Effect.provide(ConversationThreadLive),
    Effect.provide(Cloudflare.Workers.CronEventSourceLive),
    Effect.provide(Cloudflare.R2.ReadWriteBucketBinding),
  ),
) {}
