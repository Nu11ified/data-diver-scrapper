
import * as Cloudflare from "alchemy/Cloudflare";
import * as Config from "effect/Config";
import * as Effect from "effect/Effect";
import * as Option from "effect/Option";
import * as Redacted from "effect/Redacted";
import * as Schema from "effect/Schema";
import * as HttpServerRequest from "effect/unstable/http/HttpServerRequest";
import * as HttpServerResponse from "effect/unstable/http/HttpServerResponse";

import { Bucket } from "./resources.ts";
import { makeClient, type Db } from "./db.ts";
import { sendblue, simulated, type Sender } from "./outreach.ts";
import {
  ConversationThread,
  ConversationThreadLive,
  type HandleMessageInput,
  type PropertyMatch,
} from "./conversation/thread.ts";
import { importMasterKey, open, seal } from "./codex/envelope.ts";
import {
  CredentialPayload,
  authorizeUrl,
  challengeFor,
  exchangeCode,
  identityFromIdToken,
  makeVerifier,
  refreshTokens,
} from "./codex/oauth.ts";
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
    readonly assessed_value_previous?: number;
    readonly debt_to_value?: number;
    readonly code_violations?: number;
  };
  readonly conflicts?: readonly unknown[];
  readonly events: ReadonlyArray<{ readonly source: string }>;
}

const ConnectState = Schema.Struct({
  phone: Schema.String,
  verifier: Schema.String,
  createdAt: Schema.String,
});

const CONNECT_TTL_MS = 15 * 60_000;

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
    env: {
      DATABASE_URL: Config.redacted("DATABASE_URL"),
      SENDBLUE_API_KEY: Config.redacted("SENDBLUE_API_KEY"),
      SENDBLUE_SECRET_KEY: Config.redacted("SENDBLUE_SECRET_KEY"),
      CREDENTIAL_MASTER_KEY: Config.redacted("CREDENTIAL_MASTER_KEY"),
    },
  },
  Effect.gen(function* () {
    const bucket = yield* Cloudflare.R2.ReadWriteBucket(Bucket);
    const threads = yield* ConversationThread;
    const databaseUrl = yield* Config.redacted("DATABASE_URL");
    let client: Db | undefined;
    const db = (): Db => (client ??= makeClient(Redacted.value(databaseUrl)));

    const sendblueKey = yield* Config.redacted("SENDBLUE_API_KEY");
    const sendblueSecret = yield* Config.redacted("SENDBLUE_SECRET_KEY");
    const sender: Sender =
      Redacted.value(sendblueKey) === ""
        ? simulated((line) => console.log(line))
        : sendblue(Redacted.value(sendblueKey), Redacted.value(sendblueSecret));

    const masterKeyB64 = Redacted.value(yield* Config.redacted("CREDENTIAL_MASTER_KEY"));

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

    const persist = (jurisdiction: string, records: readonly CountyRecord[]) =>
      Effect.promise(async () => {
        const prisma = db();
        for (const record of records) {
          const sourceIds = new Set(record.events.map((e) => e.source));
          const key = record.keys[0];
          if (key === undefined) continue;
          const row = {
            jurisdiction,
            address: record.fields.address?.value ?? null,
            owner: record.fields.owner?.value ?? null,
            parcelId: record.fields.parcel_id?.value ?? null,
            lifecycleState: record.lifecycle_state,
            owed: record.signals.delinquent_amount ?? null,
            assessed: record.signals.assessed_value ?? null,
            assessedPrior: record.signals.assessed_value_previous ?? null,
            debtToValue: record.signals.debt_to_value ?? null,
            violations: record.signals.code_violations ?? 0,
            sourceCount: sourceIds.size,
            mergedKeys: [...record.keys],
            fields: JSON.parse(JSON.stringify(record.fields)) as object,
            conflicts: JSON.parse(JSON.stringify(record.conflicts ?? [])) as object,
          };
          await prisma.property.upsert({
            where: { key },
            create: { key, ...row },
            update: row,
          });
        }
      });

    const compileCountyPayload = (county: string) =>
      Effect.gen(function* () {
        const events = yield* loadCountyEvents(county);
        if (events.length === 0) {
          return Option.none<{
            readonly payload: string;
            readonly records: readonly CountyRecord[];
          }>();
        }
        const payload = yield* Effect.promise(() =>
          compileCounty(schemaJson, events, {}, county),
        );
        const parsed = JSON.parse(payload) as { readonly records: readonly CountyRecord[] };
        const jurisdiction = events[0]?.property_key.split("|")[0] ?? county;
        yield* persist(jurisdiction, parsed.records);
        return Option.some({ payload, records: parsed.records });
      });

    const countyMatches = (county: string) =>
      Effect.gen(function* () {
        const compiled = Option.getOrUndefined(yield* compileCountyPayload(county));
        if (compiled === undefined) return [] as readonly PropertyMatch[];
        return compiled.records
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
          const compiled = Option.getOrUndefined(
            yield* compileCountyPayload(countyMatch[1] ?? ""),
          );
          if (compiled === undefined) {
            return json({ ok: false, error: "no events; run the sources first" }, 404);
          }
          return HttpServerResponse.text(compiled.payload, {
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

          const preflight = yield* Effect.tryPromise({
            try: async () => {
              const prisma = db();
              const tenant = await prisma.tenant.upsert({
                where: { phone },
                create: { phone },
                update: {},
              });
              const credential = await prisma.credential.findUnique({
                where: { tenantId: tenant.id },
              });
              return { tenantId: tenant.id, codexAccount: credential?.accountLabel ?? "" };
            },
            catch: (cause): Error =>
              cause instanceof Error ? cause : new Error(String(cause)),
          }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
          if (preflight instanceof Error) {
            return json({ ok: false, error: preflight.message }, 500);
          }

          const lower = text.trim().toLowerCase();
          let input: HandleMessageInput = {
            text,
            candidates: yield* countyMatches("norfolk"),
            codexAccount: preflight.codexAccount,
          };
          if ((lower === "connect" || lower === "connect codex") && masterKeyB64 !== "") {
            const token = `${crypto.randomUUID()}${crypto.randomUUID()}`.replace(/-/g, "");
            yield* bucket.put(
              `connect/${token}`,
              JSON.stringify({
                phone,
                verifier: makeVerifier(),
                createdAt: new Date().toISOString(),
              }),
            );
            input = { ...input, connectUrl: `${url.origin}/connect/${token}` };
          }

          const thread = threads.getByName(phone);
          const outcome = yield* thread.handleMessage(input);

          const delivery = yield* Effect.tryPromise({
            try: async () => {
              const prisma = db();
              await prisma.message.createMany({
                data: [
                  { tenantId: preflight.tenantId, role: "user", body: text },
                  { tenantId: preflight.tenantId, role: "scout", body: outcome.reply },
                ],
              });
              const tree = outcome.tree;
              if (tree !== undefined) {
                await prisma.decisionTree.upsert({
                  where: {
                    tenantId_name_version: {
                      tenantId: preflight.tenantId,
                      name: tree.name,
                      version: tree.version,
                    },
                  },
                  create: {
                    tenantId: preflight.tenantId,
                    name: tree.name,
                    version: tree.version,
                    status: "active",
                    configuration: JSON.parse(
                      JSON.stringify({ spec: tree.spec, graph: tree.graph }),
                    ) as object,
                  },
                  update: { status: "active" },
                });
                await prisma.decisionTree.updateMany({
                  where: {
                    tenantId: preflight.tenantId,
                    name: tree.name,
                    version: { not: tree.version },
                  },
                  data: { status: "superseded" },
                });
              }
              const first = outcome.evaluations[0];
              if (first !== undefined) {
                const treeRow = await prisma.decisionTree.findUnique({
                  where: {
                    tenantId_name_version: {
                      tenantId: preflight.tenantId,
                      name: first.treeName,
                      version: first.treeVersion,
                    },
                  },
                });
                await prisma.evaluation.createMany({
                  data: outcome.evaluations.map((evaluation) => ({
                    tenantId: preflight.tenantId,
                    propertyKey: evaluation.propertyKey,
                    decisionTreeId: treeRow === null ? null : treeRow.id,
                    treeVersion: evaluation.treeVersion,
                    outcome: evaluation.outcome,
                    trace: JSON.parse(JSON.stringify(evaluation.trace)) as object,
                  })),
                });
              }
              await sender.send({ to: phone, body: outcome.reply });
            },
            catch: (cause): Error => (cause instanceof Error ? cause : new Error(String(cause))),
          }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));

          return json({
            ok: !(delivery instanceof Error),
            phone,
            reply: outcome.reply,
            evaluations: outcome.evaluations.length,
            treeVersion: outcome.tree?.version,
            transport: sender.name,
            ...(delivery instanceof Error ? { error: delivery.message } : {}),
          });
        }

        const connectTokenMatch = /^\/connect\/([a-f0-9]{32,})$/.exec(url.pathname);
        if (connectTokenMatch !== null && request.method === "GET") {
          const token = connectTokenMatch[1] ?? "";
          const object = yield* bucket.get(`connect/${token}`);
          if (object === null) {
            return json({ ok: false, error: "connect link is invalid or already used" }, 404);
          }
          const connectState = yield* Schema.decodeUnknownEffect(ConnectState)(
            JSON.parse(yield* object.text()),
          );
          if (Date.now() - Date.parse(connectState.createdAt) > CONNECT_TTL_MS) {
            yield* bucket.delete(`connect/${token}`);
            return json({ ok: false, error: "connect link expired; text CONNECT again" }, 410);
          }
          const challenge = yield* challengeFor(connectState.verifier);
          const location = authorizeUrl({
            redirectUri: `${url.origin}/connect/callback`,
            state: token,
            challenge,
          });
          return HttpServerResponse.text("", { status: 302, headers: { location } });
        }

        if (url.pathname === "/connect/callback" && request.method === "GET") {
          const code = url.searchParams.get("code") ?? "";
          const token = url.searchParams.get("state") ?? "";
          if (code === "" || token === "") {
            return json({ ok: false, error: "code and state are required" }, 400);
          }
          const object = yield* bucket.get(`connect/${token}`);
          if (object === null) {
            return json({ ok: false, error: "connect link is invalid or already used" }, 410);
          }
          const connectState = yield* Schema.decodeUnknownEffect(ConnectState)(
            JSON.parse(yield* object.text()),
          );
          yield* bucket.delete(`connect/${token}`);
          if (Date.now() - Date.parse(connectState.createdAt) > CONNECT_TTL_MS) {
            return json({ ok: false, error: "connect link expired; text CONNECT again" }, 410);
          }
          const masterKey = yield* importMasterKey(masterKeyB64);
          const tokens = yield* exchangeCode({
            code,
            redirectUri: `${url.origin}/connect/callback`,
            verifier: connectState.verifier,
          });
          const identity = yield* identityFromIdToken(tokens.idToken);
          const payload: CredentialPayload = {
            provider: "codex",
            accountId: identity.accountId,
            email: identity.email,
            accessToken: tokens.accessToken,
            refreshToken: tokens.refreshToken,
            idToken: tokens.idToken,
            obtainedAt: new Date().toISOString(),
          };
          const sealed = yield* seal(masterKey, JSON.stringify(payload));
          const stored = yield* Effect.tryPromise({
            try: async () => {
              const prisma = db();
              const tenant = await prisma.tenant.upsert({
                where: { phone: connectState.phone },
                create: { phone: connectState.phone },
                update: {},
              });
              const row = {
                provider: "codex",
                ciphertext: sealed.ciphertext,
                iv: sealed.iv,
                wrappedKey: sealed.wrappedKey,
                accountLabel: identity.email !== "" ? identity.email : identity.accountId,
              };
              await prisma.credential.upsert({
                where: { tenantId: tenant.id },
                create: { tenantId: tenant.id, ...row },
                update: row,
              });
            },
            catch: (cause): Error =>
              cause instanceof Error ? cause : new Error(String(cause)),
          }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
          if (stored instanceof Error) {
            return json({ ok: false, error: stored.message }, 500);
          }
          return HttpServerResponse.text(
            `<!doctype html><meta charset="utf-8"><title>Goliath Scout</title>` +
              `<body style="font-family:system-ui;max-width:32rem;margin:4rem auto">` +
              `<h1>Codex connected</h1><p>${identity.email !== "" ? identity.email : identity.accountId} ` +
              `is now linked to ${connectState.phone}. Tokens are stored encrypted. ` +
              `You can close this tab and return to your text thread.</p></body>`,
            { headers: { "content-type": "text/html; charset=utf-8" } },
          );
        }

        if (url.pathname === "/connect/status" && request.method === "GET") {
          const phone = (url.searchParams.get("phone") ?? "").replace(/[^+0-9]/g, "");
          if (phone === "") return json({ ok: false, error: "phone is required" }, 400);
          const row = yield* Effect.promise(() =>
            db().credential.findFirst({ where: { tenant: { phone } } }),
          );
          if (row === null) return json({ ok: true, connected: false });
          const masterKey = yield* importMasterKey(masterKeyB64);
          const opened = yield* open(masterKey, {
            ciphertext: Uint8Array.from(row.ciphertext),
            iv: Uint8Array.from(row.iv),
            wrappedKey: Uint8Array.from(row.wrappedKey),
          });
          const payload = yield* Schema.decodeUnknownEffect(CredentialPayload)(
            JSON.parse(opened),
          );
          const refreshed = yield* refreshTokens(payload.refreshToken);
          const next: CredentialPayload = {
            ...payload,
            accessToken: refreshed.accessToken,
            refreshToken: refreshed.refreshToken,
            obtainedAt: new Date().toISOString(),
          };
          const resealed = yield* seal(masterKey, JSON.stringify(next));
          yield* Effect.promise(() =>
            db().credential.update({
              where: { id: row.id },
              data: {
                ciphertext: resealed.ciphertext,
                iv: resealed.iv,
                wrappedKey: resealed.wrappedKey,
              },
            }),
          );
          return json({
            ok: true,
            connected: true,
            account: payload.email !== "" ? payload.email : payload.accountId,
            refreshed: true,
          });
        }

        return json({ ok: false, error: "not found" }, 404);
      });

    return {
      fetch: handleFetch.pipe(
        Effect.catchTag("R2Error", (cause) =>
          Effect.succeed(json({ ok: false, error: cause.message }, 500)),
        ),
        Effect.catchTag("OAuthError", (cause) =>
          Effect.succeed(json({ ok: false, error: cause.message }, 502)),
        ),
        Effect.catchTag("EnvelopeError", (cause) =>
          Effect.succeed(json({ ok: false, error: cause.message }, 500)),
        ),
        Effect.catchTag("SchemaError", (cause) =>
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
