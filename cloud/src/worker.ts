
import * as Cloudflare from "alchemy/Cloudflare";
import * as Cause from "effect/Cause";
import * as Config from "effect/Config";
import * as Effect from "effect/Effect";
import * as Exit from "effect/Exit";
import * as Option from "effect/Option";
import * as Redacted from "effect/Redacted";
import * as Schema from "effect/Schema";
import * as HttpServerRequest from "effect/unstable/http/HttpServerRequest";
import * as HttpServerResponse from "effect/unstable/http/HttpServerResponse";

import { Bucket } from "./resources.ts";
import { crawl } from "./crawl.ts";
import { makeLimiter } from "./politeness.ts";
import {
  RECORD_CAP,
  appTokenHeaders,
  fetchPaginated,
  fetchSingle,
  isSocrataUrl,
  type FetchResult,
  type PageFetch,
} from "./pagination.ts";
import { makeClient, type Db } from "./db.ts";
import { sendblue, simulated, type Sender } from "./outreach.ts";
import {
  coverageFor,
  makeThread,
  slashCommand,
  subjectOf,
  templateDraft,
  type HandleMessageInput,
  type HandleOutcome,
  type PropertyMatch,
  type ThreadSnapshot,
} from "./conversation/thread.ts";
import { runPiScout } from "./conversation/pi.ts";
import {
  legacyProfileText,
  type ProfileUpdate,
} from "./conversation/profile.ts";
import type {
  ScoutDecision,
  SourceCandidate,
} from "./conversation/scout.ts";
import { SIGNAL_CATALOG, evaluate, validateGraph } from "./decision/graph.ts";
import {
  CodexError,
  RESEARCH_MODEL,
  complete,
} from "./codex/client.ts";
import { CodexEgressHost, CodexEgressHostLive } from "./codex/egress-host.ts";
import { makeCodexFetch } from "./codex/egress-fetch.ts";
import { importMasterKey, open, seal } from "./codex/envelope.ts";
import {
  CredentialPayload,
  DEVICE_VERIFICATION_URL,
  exchangeDeviceCode,
  identityFromIdToken,
  jwtExpiresAt,
  pollDeviceCode,
  refreshTokens,
  requestDeviceCode,
  type TokenSet,
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
import { bundledSeed } from "./seed.ts";
import { requestOrigin } from "./origin.ts";
import {
  ACTIVE_WARM_MS,
  CountyWarmStatus,
  FAILED_WARM_RETRY_MS,
  countyWorkflowId,
  shouldReuseWarm,
  warmRetryKey,
  type CountyWarmStatus as CountyWarmStatusValue,
} from "./warming.ts";
import {
  configFromRow,
  parseSeed,
  planSeed,
  slugId,
  type SourceConfig,
} from "./sources.ts";

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
  readonly truncated?: boolean;
}

interface Probe {
  readonly outcome: RunOutcome;
  readonly attempted: readonly RunOutcome[];
  readonly crawledFrom?: string;
}

interface SmsPayload {
  readonly from_number?: string;
  readonly to_number?: string;
  readonly number?: string;
  readonly content?: string;
  readonly message_handle?: string;
}

interface WarmNotificationPayload {
  readonly tenantId?: string;
  readonly county?: string;
  readonly canonical?: string;
  readonly instanceId?: string;
  readonly ok?: boolean;
  readonly error?: string;
}

interface CountyRecord {
  readonly keys: readonly string[];
  readonly lifecycle_state: string;
  readonly fields: Readonly<
    Record<string, { readonly value: string; readonly source: string }>
  >;
  readonly signals: Readonly<Record<string, number>>;
  readonly conflicts?: readonly unknown[];
  readonly events: ReadonlyArray<{ readonly source: string }>;
}

const ConnectState = Schema.Struct({
  phone: Schema.String,
  deviceAuthId: Schema.String,
  userCode: Schema.String,
  intervalSeconds: Schema.Number,
  createdAt: Schema.String,
});

const CountySourceDiscovery = Schema.Struct({
  jurisdiction: Schema.String,
  candidates: Schema.Array(
    Schema.Struct({
      id: Schema.String,
      name: Schema.String,
      url: Schema.String,
    }),
  ),
});

const SweepManifest = Schema.Struct({
  id: Schema.String,
  runId: Schema.String,
  createdAt: Schema.String,
  sourceIds: Schema.Array(Schema.String),
});

const CONNECT_TTL_MS = 15 * 60_000;
const USER_AGENT = "DataDiver/0.1 (public-record research; contact site operator)";
const MODEL_UNAVAILABLE_REPLY =
  `I could not reach the reasoning service, so I did not change your ` +
  `criteria or run anything. Please try again in a few minutes.`;
const DEFAULT_SOURCE_COVERAGE = [
  "recorded taxes and liens",
  "assessed values",
  "code or court events",
  "dated events",
  "independent corroborating sources",
  "property addresses",
] as const;

const schemaJson = JSON.stringify(schema);
const classifierJson = JSON.stringify(classifier);
const columnModelJson = JSON.stringify(columnModel);

const json = (value: unknown, status = 200): HttpServerResponse.HttpServerResponse =>
  HttpServerResponse.text(JSON.stringify(value, null, 1), {
    status,
    headers: { "content-type": "application/json" },
  });

const escapeHtml = (value: string): string =>
  value
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");

const smsWorkflowId = async (messageHandle: string): Promise<string> => {
  const identity =
    messageHandle.trim() === "" ? crypto.randomUUID() : messageHandle.trim();
  const digest = new Uint8Array(
    await crypto.subtle.digest("SHA-256", new TextEncoder().encode(identity)),
  );
  return `sms_${Array.from(digest.slice(0, 20), (byte) =>
    byte.toString(16).padStart(2, "0"),
  ).join("")}`;
};

export default class Scraper extends Cloudflare.Worker<Scraper>()(
  "Scraper",
  {
    main: import.meta.url,
    build: {
      output: {
        codeSplitting: false,
      },
    },
    env: {
      DATABASE_URL: Config.redacted("DATABASE_URL"),
      SENDBLUE_API_KEY: Config.redacted("SENDBLUE_API_KEY"),
      SENDBLUE_SECRET_KEY: Config.redacted("SENDBLUE_SECRET_KEY"),
      CREDENTIAL_MASTER_KEY: Config.redacted("CREDENTIAL_MASTER_KEY"),
      PUBLIC_ORIGIN: Config.redacted("PUBLIC_ORIGIN"),
      SOCRATA_APP_TOKEN: Config.redacted("SOCRATA_APP_TOKEN").pipe(
        Config.withDefault(Redacted.make("")),
      ),
    },
  },
  Effect.gen(function* () {
    const bucket = yield* Cloudflare.R2.ReadWriteBucket(Bucket);
    const codexEgress = yield* CodexEgressHost;
    const codexFetch = makeCodexFetch((request) =>
      Effect.runPromise(
        codexEgress
          .getByName("codex")
          .request(request)
          .pipe(Effect.timeout("60 seconds")),
      ),
    );
    const databaseUrl = yield* Config.redacted("DATABASE_URL");
    const db = (): Db => makeClient(Redacted.value(databaseUrl));
    const threadFor = (tenantId: string) => {
      const prisma = db();
      return makeThread({
        get: <T>(key: string) =>
          Effect.promise(async () => {
            const row = await prisma.conversationState.findUnique({
              where: { tenantId_key: { tenantId, key } },
            });
            return row?.value as T | undefined;
          }),
        put: (key: string, value: unknown) =>
          Effect.promise(async () => {
            const encoded = JSON.parse(JSON.stringify(value));
            await prisma.conversationState.upsert({
              where: { tenantId_key: { tenantId, key } },
              create: { tenantId, key, value: encoded },
              update: { value: encoded },
            });
          }),
        delete: (key: string) =>
          Effect.promise(async () => {
            await prisma.conversationState.deleteMany({ where: { tenantId, key } });
          }),
      });
    };

    const sendblueKey = yield* Config.redacted("SENDBLUE_API_KEY");
    const sendblueSecret = yield* Config.redacted("SENDBLUE_SECRET_KEY");
    const socrataAppToken = Redacted.value(
      yield* Config.redacted("SOCRATA_APP_TOKEN").pipe(Config.withDefault(Redacted.make(""))),
    );
    const sender: Sender =
      Redacted.value(sendblueKey) === ""
        ? simulated((line) => console.log(line))
        : sendblue(Redacted.value(sendblueKey), Redacted.value(sendblueSecret));

    const masterKeyB64 = Redacted.value(yield* Config.redacted("CREDENTIAL_MASTER_KEY"));
    // Requests arrive with a relative url, so nothing in them knows the public
    // address of this worker. A link built from a guess is a link that does
    // not open, which is worse than no link.
    const publicOrigin = Redacted.value(yield* Config.redacted("PUBLIC_ORIGIN")).replace(
      /\/+$/,
      "",
    );

    // One budget for the worker's lifetime. Every county fetch, crawl and
    // discovery attempt draws on it, so no single request can turn this into a
    // battering ram against someone else's server.
    const limiter = makeLimiter();

    const CREDENTIAL_FRESH_MS = 45 * 60_000;
    const CREDENTIAL_EXPIRY_MARGIN_MS = 2 * 60_000;

    const freshCredential = (tenantId: string, force = false) =>
      Effect.gen(function* () {
        const row = yield* Effect.promise(() =>
          db().credential.findUnique({ where: { tenantId } }),
        );
        if (row === null) return Option.none<CredentialPayload>();
        const masterKey = yield* importMasterKey(masterKeyB64);
        const opened = yield* open(masterKey, {
          ciphertext: Uint8Array.from(row.ciphertext),
          iv: Uint8Array.from(row.iv),
          wrappedKey: Uint8Array.from(row.wrappedKey),
        });
        const payload = yield* Schema.decodeUnknownEffect(CredentialPayload)(
          JSON.parse(opened),
        );
        const age = Date.now() - Date.parse(payload.obtainedAt);
        const expiresAt = jwtExpiresAt(payload.accessToken);
        const usable =
          expiresAt === undefined
            ? age < CREDENTIAL_FRESH_MS
            : expiresAt - Date.now() > CREDENTIAL_EXPIRY_MARGIN_MS;
        if (!force && usable) return Option.some(payload);
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
        return Option.some(next);
      });

    const storeCredential = (phone: string, tokens: TokenSet) =>
      Effect.gen(function* () {
        const identity = yield* identityFromIdToken(tokens.idToken);
        const masterKey = yield* importMasterKey(masterKeyB64);
        const payload: CredentialPayload = {
          provider: "codex",
          accountId: identity.accountId,
          email: identity.email,
          accessToken: tokens.accessToken,
          refreshToken: tokens.refreshToken,
          idToken: tokens.idToken,
          obtainedAt: new Date().toISOString(),
        };
        const encrypted = yield* seal(masterKey, JSON.stringify(payload));
        yield* Effect.tryPromise({
          try: async () => {
            const prisma = db();
            const tenant = await prisma.tenant.upsert({
              where: { phone },
              create: { phone },
              update: {},
            });
            const row = {
              provider: "codex",
              ciphertext: encrypted.ciphertext,
              iv: encrypted.iv,
              wrappedKey: encrypted.wrappedKey,
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
        }).pipe(Effect.orDie);
        return identity;
      });

    const loadConnectState = (token: string) =>
      Effect.gen(function* () {
        const object = yield* bucket.get(`connect/${token}`);
        if (object === null) return Option.none<(typeof ConnectState)["Type"]>();
        return yield* Schema.decodeUnknownEffect(ConnectState)(
          JSON.parse(yield* object.text()),
        ).pipe(
          Effect.map(Option.some),
          Effect.catch(() => Effect.succeed(Option.none<(typeof ConnectState)["Type"]>())),
        );
      });

    const scoutTurn = (
      tenantId: string,
      text: string,
      candidates: readonly PropertyMatch[],
      snap: ThreadSnapshot,
      onDecision: (decision: ScoutDecision) => void,
      dryRun = false,
    ) =>
      Effect.gen(function* () {
        const thread = threadFor(tenantId);
        const credential = Option.getOrUndefined(yield* freshCredential(tenantId));
        if (credential === undefined) {
          return yield* new CodexError({
            message: "no codex credential stored",
            status: 0,
          });
        }
        const extraSignals = [
          ...new Set(candidates.flatMap((candidate) => Object.keys(candidate.signals))),
        ];
        const qualifiedCount = candidates.filter(
          (candidate) =>
            evaluate(snap.tree.graph, subjectOf(candidate)).outcome === "match",
        ).length;
        const coverage = coverageFor(snap.tree.graph, candidates);
        const discoverableMissing = coverage.missing.filter(
          (item) => item !== "complete county pagination",
        );
        const context = {
          tree: snap.tree,
          configured: snap.configured,
          summary: snap.summary,
          recentTurns: snap.recentTurns,
          county: snap.county,
          candidateCount: candidates.length,
          qualifiedCount,
          extraSignals,
          coverage,
          ...(snap.profile === undefined ? {} : { profile: snap.profile }),
        };
        const askPi = (accessToken: string) =>
          Effect.tryPromise({
            try: () =>
              runPiScout({
                accessToken,
                sessionId: tenantId,
                userText: text,
                context,
                fetch: codexFetch,
              }),
            catch: (cause): CodexError =>
              new CodexError({
                message: cause instanceof Error ? cause.message : String(cause),
                status: 0,
              }),
          });
        const piResult = yield* askPi(credential.accessToken).pipe(
          Effect.catchTag("CodexError", (firstError) =>
            /\b401\b|unauthorized|authentication|token expired/i.test(
              firstError.message,
            )
              ? Effect.gen(function* () {
                  const forced = Option.getOrUndefined(
                    yield* freshCredential(tenantId, true),
                  );
                  if (forced === undefined) return yield* Effect.fail(firstError);
                  return yield* askPi(forced.accessToken);
                })
              : Effect.fail(firstError),
          ),
        );
        const decision = piResult.decision;
        onDecision(decision);
        if (dryRun) {
          return {
            reply: decision.text,
            evaluations: [],
          } satisfies HandleOutcome;
        }
        if (
          discoverableMissing.length > 0 &&
          snap.county !== "" &&
          decision.kind !== "discover" &&
          !(decision.kind === "update_profile" && decision.update.county !== undefined)
        ) {
          yield* ensureCountyWarm(
            snap.county,
            tenantId,
            discoverableMissing,
            snap.configured,
          );
        }
        const asInternal =
          decision.kind === "show_matches"
            ? "review"
            : decision.kind === "show_property"
              ? String(decision.index)
              : decision.kind === "resolve_pending"
                ? decision.approved
                  ? "approve"
                  : "reject"
                : "";
        if (asInternal !== "") {
          const acted = yield* thread.handleMessage({
            text: asInternal,
            candidates,
            codexAccount: "",
            coverage,
          });
          const lead =
            decision.kind === "show_matches" && !coverage.ready
              ? ""
              : decision.text.trim();
          return {
            ...acted,
            reply: lead === "" ? acted.reply : `${lead}\n\n${acted.reply}`,
          };
        }
        if (decision.kind === "update_profile") {
          const outcome = yield* thread.handleMessage({
            text:
              snap.profile === undefined
                ? legacyProfileText(decision.update, text)
                : text,
            candidates,
            codexAccount: credential.accountId,
            onboardingUpdate: decision.update,
            onboardingLead: decision.text,
            coverage,
          });
          if (decision.update.county !== undefined) {
            yield* ensureCountyWarm(
              decision.update.county,
              tenantId,
              DEFAULT_SOURCE_COVERAGE,
            );
          }
          return outcome;
        }
        if (decision.kind === "discover") {
          const jurisdiction =
            snap.profile?.county ??
            (snap.configured ? snap.county : decision.jurisdiction);
          const status = yield* ensureCountyWarm(
            jurisdiction,
            tenantId,
            DEFAULT_SOURCE_COVERAGE,
            true,
          );
          return yield* thread.applyScout({
            userText: text,
            reply:
              status.state === "error"
                ? `I could not start the county record scan: ${status.error ?? "the workflow was unavailable"}. Nothing was changed.`
                : `${decision.text}\n\nThe county record scan is ${status.state}. ` +
                  `I will only report matches after the required official sources are verified.`,
            ...(status.state === "error" ? {} : { county: jurisdiction }),
          });
        }
        if (decision.kind === "temp_filter") {
          return yield* thread.previewFilter({
            userText: text,
            lead: decision.text,
            graph: decision.graph,
            candidates,
            ...(decision.limit === undefined ? {} : { limit: decision.limit }),
          });
        }
        if (decision.kind === "remember_filter") {
          return yield* thread.rememberFilter({
            userText: text,
            lead: decision.text,
            remember: decision.remember,
          });
        }
        if (decision.kind === "set_tree") {
          const problems = validateGraph(decision.graph, [
            ...Object.keys(SIGNAL_CATALOG),
            ...extraSignals,
          ]);
          if (problems.length > 0) {
            return yield* thread.applyScout({
              userText: text,
              reply:
                `I drafted a criteria change but it failed validation:\n` +
                problems.map((p) => `- ${p}`).join("\n") +
                `\nNothing was changed; try rephrasing.`,
            });
          }
          return yield* thread.proposeTree({
            userText: text,
            lead: decision.text,
            graph: decision.graph,
            candidates,
          });
        }
        return yield* thread.applyScout({ userText: text, reply: decision.text });
      });

    const draftOutreach = (tenantId: string, match: PropertyMatch) =>
      Effect.gen(function* () {
        const credential = Option.getOrUndefined(yield* freshCredential(tenantId));
        if (credential === undefined) {
          return yield* new CodexError({
            message: "no codex credential stored",
            status: 0,
          });
        }
        const facts = [
          `property address: ${match.address}`,
          `recorded owner: ${match.owner}`,
          match.mailing !== "" ? `owner mailing address: ${match.mailing}` : "",
          match.assessed > 0 ? `county assessed value: $${Math.round(match.assessed)}` : "",
        ]
          .filter((line) => line !== "")
          .join("\n");
        const instructions = [
          `You write the first outreach message from a property acquisition team to`,
          `a property owner. Use only the facts provided; never invent names,`,
          `numbers or circumstances. Address the owner naturally by name. Reference`,
          `the property address. Express interest in buying and invite a`,
          `conversation. Do not mention taxes, debts, delinquency, violations or`,
          `hardship of any kind. 2 to 4 sentences. Output the message as plain`,
          `text only: no placeholders, no signature block, no quotes, no JSON.`,
        ].join("\n");
        const raw = yield* complete(
          {
            accessToken: credential.accessToken,
            accountId: credential.accountId,
            instructions,
            userText: facts,
          },
          codexFetch,
        );
        return raw.trim();
      });

    const runSource = (source: SourceConfig, runId: string) =>
      Effect.gen(function* () {
        const fetchStart = Date.now();
        const requestHeaders: Record<string, string> = {
          "user-agent": "DataDiver/0.1 (public-record research)",
          ...source.headers,
          ...appTokenHeaders(source.url, socrataAppToken),
        };
        const method: "GET" | "POST" = source.method ?? "GET";
        const requestBody = method === "POST" ? source.body : undefined;
        const fetchPage = (pageUrl: string): Promise<PageFetch> =>
          fetch(pageUrl, {
            method,
            headers: requestHeaders,
            ...(requestBody === undefined ? {} : { body: requestBody }),
          }).then(async (response) => ({
            ok: response.ok,
            status: response.status,
            contentType: response.headers.get("content-type") ?? "",
            body: await response.text(),
          }));

        const fetched: FetchResult = yield* Effect.tryPromise({
          try: () =>
            isSocrataUrl(source.url)
              ? fetchPaginated(source.url, limiter, fetchPage, RECORD_CAP, 1000)
              : fetchSingle(source.url, limiter, fetchPage),
          catch: (cause): Error =>
            cause instanceof Error ? cause : new Error(String(cause)),
        }).pipe(
          Effect.catch(
            (cause: Error): Effect.Effect<FetchResult> =>
              Effect.succeed({
                ok: false,
                body: "",
                contentType: "",
                truncated: false,
                pages: 0,
                error: cause.message,
              }),
          ),
        );
        if (!fetched.ok) {
          return {
            sourceId: source.id,
            ok: false,
            stage: "fetch",
            error: fetched.error ?? "fetch failed",
          } satisfies RunOutcome;
        }
        const body = fetched.body;
        const contentType = fetched.contentType;
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
                headers: source.headers ?? undefined,
                method: source.method ?? null,
                body: source.body ?? null,
              },
              update: { name: source.name, url: source.url },
            });
            await prisma.run.create({
              data: {
                id: `${runId}_${source.id}`,
                sourceId: source.id,
                startedAt: new Date(fetchStart),
                // Written as unfinished. Only the update at the end of this
                // block, after the properties and events actually land, marks
                // it done; a crash in between leaves a record that says so
                // instead of a run that claims a success it never had.
                ok: false,
                stage: "store",
                classification: processed.classification,
                classConfidence: processed.class_confidence,
                extractionRate: processed.extraction_rate,
                records: processed.records,
                fingerprint: processed.fingerprint,
                newestRecordDate: processed.newest_record_date,
                fetchMs,
                truncated: fetched.truncated,
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
            // Everything landed, so the run may finally claim it.
            await prisma.run.update({
              where: { id: `${runId}_${source.id}` },
              data: { ok: true, stage: "done" },
            });
          },
          catch: (cause): Error => (cause instanceof Error ? cause : new Error(String(cause))),
        }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
        if (stored instanceof Error) {
          // Record why, rather than leaving a run stuck at "store" with no
          // explanation for whoever reads the table later.
          yield* Effect.tryPromise({
            try: () =>
              db().run.update({
                where: { id: `${runId}_${source.id}` },
                data: { error: stored.message.slice(0, 500) },
              }),
            catch: (cause): Error =>
              cause instanceof Error ? cause : new Error(String(cause)),
          }).pipe(Effect.catch(() => Effect.void));
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
          ...(fetched.truncated ? { truncated: true } : {}),
        } satisfies RunOutcome;
      });

    const allSources = Effect.gen(function* () {
      const rows = yield* Effect.promise(() =>
        db().source.findMany({ where: { enabled: true }, orderBy: { id: "asc" } }),
      );
      return rows.map(configFromRow);
    });

    const EVENT_LIMIT = 10_000;

    const countySlug = (county: string): string =>
      county.toLowerCase().replace(/[^a-z0-9]+/g, "_").replace(/^_|_$/g, "");

    const discoverCountySources = (
      tenantId: string,
      county: string,
      requiredCoverage: readonly string[] = [],
    ) =>
      Effect.gen(function* () {
        const credential = Option.getOrUndefined(yield* freshCredential(tenantId));
        if (credential === undefined) {
          return yield* new CodexError({
            message: "no codex credential stored",
            status: 0,
          });
        }
        const raw = yield* complete(
          {
            accessToken: credential.accessToken,
            accountId: credential.accountId,
            model: RESEARCH_MODEL,
            instructions: [
              `Find official machine-readable public property-record sources for one`,
              `US county or city. Prefer the county tax office, appraisal district,`,
              `code enforcement, court, and official GIS. Return direct HTTPS CSV,`,
              `JSON, ArcGIS query, or official download-page URLs that can be fetched`,
              `without a browser login. Do not invent an endpoint. Return only JSON:`,
              `{"jurisdiction":"snake_case_state","candidates":[`,
              `{"id":"county_signal","name":"Official source name","url":"https://..."}`,
              `]}. Use county-prefixed ids and at most four candidates.`,
            ].join("\n"),
            userText:
              requiredCoverage.length === 0
                ? county
                : `${county}\nMissing coverage: ${requiredCoverage.join(", ")}`,
          },
          codexFetch,
        );
        const start = raw.indexOf("{");
        const end = raw.lastIndexOf("}");
        if (start < 0 || end <= start) {
          return yield* new CodexError({
            message: "county source research returned no JSON object",
            status: 0,
          });
        }
        return yield* Schema.decodeUnknownEffect(CountySourceDiscovery)(
          JSON.parse(raw.slice(start, end + 1)),
        ).pipe(
          Effect.map((discovery) => ({
            jurisdiction: countySlug(county),
            candidates: discovery.candidates.slice(0, 4),
          })),
          Effect.mapError(
            (cause) =>
              new CodexError({
                message: `county source research returned invalid JSON: ${String(cause)}`,
                status: 0,
              }),
          ),
        );
      });

    const validateSourceCandidates = (
      jurisdiction: string,
      candidates: readonly SourceCandidate[],
    ) =>
      Effect.gen(function* () {
        const runId = `run_${Date.now().toString(36)}`;
        const vetted = candidates.slice(0, 4);
        const known = yield* Effect.promise(() =>
          db().source.findMany({
            where: { id: { in: vetted.map((candidate) => slugId(candidate.id)) } },
            select: { id: true },
          }),
        );
        const preexisting = new Set(known.map((row) => row.id));
        const probes: readonly Probe[] = yield* Effect.all(
          vetted.map((candidate) =>
            candidate.url.startsWith("https://")
              ? Effect.gen(function* () {
                  const id = slugId(candidate.id);
                  const attempted: RunOutcome[] = [];
                  const direct = yield* runSource(
                    { id, name: candidate.name, url: candidate.url, jurisdiction },
                    runId,
                  );
                  attempted.push(direct);
                  if (direct.ok && (direct.records ?? 0) > 0) {
                    return { outcome: direct, attempted } satisfies Probe;
                  }
                  let best: RunOutcome = direct;
                  let bestUrl = "";
                  let page = 0;
                  yield* crawl(
                    candidate.url,
                    { maxPages: 12, maxDepth: 2, userAgent: USER_AGENT, limiter },
                    (visited) =>
                      Effect.gen(function* () {
                        page += 1;
                        const outcome = yield* runSource(
                          {
                            id: `${id}_p${page}`,
                            name: candidate.name,
                            url: visited.url,
                            jurisdiction,
                          },
                          runId,
                        );
                        attempted.push(outcome);
                        if (
                          outcome.ok &&
                          (outcome.records ?? 0) > (best.records ?? 0)
                        ) {
                          best = outcome;
                          bestUrl = visited.url;
                        }
                        return true;
                      }),
                  );
                  return bestUrl === ""
                    ? ({ outcome: direct, attempted } satisfies Probe)
                    : ({ outcome: best, attempted, crawledFrom: bestUrl } satisfies Probe);
                })
              : Effect.succeed({
                  outcome: {
                    sourceId: slugId(candidate.id),
                    ok: false,
                    stage: "fetch",
                    error: "only https urls are validated",
                  },
                  attempted: [],
                } satisfies Probe),
          ),
          { concurrency: 2 },
        );
        const admitted = probes.filter(
          (probe) => probe.outcome.ok && (probe.outcome.records ?? 0) > 0,
        );
        const dead = probes
          .flatMap((probe) => probe.attempted)
          .filter((outcome) => !(outcome.ok && (outcome.records ?? 0) > 0))
          .map((outcome) => outcome.sourceId)
          .filter((id) => !preexisting.has(id));
        const admission = yield* Effect.tryPromise({
          try: async () => {
            const prisma = db();
            if (admitted.length > 0) {
              await prisma.source.updateMany({
                where: {
                  id: { in: admitted.map((probe) => probe.outcome.sourceId) },
                },
                data: { enabled: true },
              });
            }
            if (dead.length > 0) {
              await prisma.source.updateMany({
                where: { id: { in: dead } },
                data: { enabled: false },
              });
            }
          },
          catch: (cause): Error =>
            cause instanceof Error ? cause : new Error(String(cause)),
        }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
        return {
          probes,
          admitted,
          admissionError:
            admission instanceof Error ? admission.message : undefined,
        };
      });

    const countyWhere = (county: string) => {
      const slug = countySlug(county);
      return {
        OR: [
          { propertyKey: { startsWith: `${slug}|` } },
          { propertyKey: { contains: `_${slug}|` } },
          { propertyKey: { contains: `${slug}_`, mode: "insensitive" as const } },
        ],
      };
    };

    /// Every spelling of a county has to land on one cache entry, or warming
    /// "city_of_norfolk_va" leaves a thread asking for "norfolk" to compile
    /// from cold anyway. The jurisdiction that actually prefixes the property
    /// keys is the canonical name.
    const resolveJurisdiction = (county: string) =>
      Effect.promise(async () => {
        const row = await db().event.findFirst({
          where: countyWhere(county),
          select: { propertyKey: true },
        });
        return row === null ? countySlug(county) : (row.propertyKey.split("|")[0] ?? countySlug(county));
      });

    /// Cheap summary of what the county's events currently are. Compiling is
    /// the expensive step, so the cache turns on whether anything was ingested
    /// since, not on a clock: a fresh run changes the count or the newest
    /// recordedAt and the next read recompiles.
    const countyStamp = (county: string) =>
      Effect.promise(async () => {
        const result = await db().event.aggregate({
          where: countyWhere(county),
          _count: { _all: true },
          _max: { recordedAt: true },
        });
        return `${result._count._all}:${result._max.recordedAt?.toISOString() ?? "none"}`;
      });

    const loadCountyEvents = (county: string) =>
      Effect.promise(async (): Promise<readonly PropertyEvent[]> => {
        // A property key is "<jurisdiction>|<id>", so a bare substring let
        // "york" pull New York's events into York's county view. Matching the
        // segment before the pipe keeps jurisdictions apart.
        const rows = await db().event.findMany({
          where: countyWhere(county),
          orderBy: [
            { eventDate: { sort: "desc", nulls: "last" } },
            { recordedAt: "desc" },
          ],
          take: EVENT_LIMIT,
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
        for (let offset = 0; offset < records.length; offset += 50) {
          const rows = records.slice(offset, offset + 50).flatMap((record) => {
            const sourceIds = new Set(record.events.map((e) => e.source));
            const key = record.keys[0];
            if (key === undefined) return [];
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
            return [{ key, row }];
          });
          const prisma = db();
          try {
            await prisma.$transaction(
              rows.map(({ key, row }) =>
                prisma.property.upsert({
                  where: { key },
                  create: { key, ...row },
                  update: row,
                }),
              ),
            );
          } finally {
            await prisma.$disconnect();
          }
        }
      });

    const CompiledCache = Schema.Struct({
      stamp: Schema.String,
      at: Schema.String,
      payload: Schema.String,
      abi: Schema.optional(Schema.Number),
    });

    const compileCountyPayload = (county: string) =>
      Effect.gen(function* () {
        const canonical = yield* resolveJurisdiction(county);
        const cacheKey = `compiled/${canonical}.json`;
        const stamp = yield* countyStamp(county);
        const { abi } = yield* Effect.promise(() => engineVersion());
        const cached = yield* bucket.get(cacheKey);
        if (cached !== null) {
          const decoded = yield* Schema.decodeUnknownEffect(CompiledCache)(
            JSON.parse(yield* cached.text()),
          ).pipe(Effect.catch(() => Effect.succeed(undefined)));
          const fresh =
            decoded !== undefined &&
            decoded.abi === abi &&
            decoded.stamp === stamp;
          if (fresh && decoded !== undefined) {
            const parsed = JSON.parse(decoded.payload) as {
              readonly records: readonly CountyRecord[];
            };
            // The properties table was already written when this was compiled,
            // so re-persisting it would only repeat work the cache exists to
            // avoid.
            return Option.some({ payload: decoded.payload, records: parsed.records });
          }
        }
        const events = yield* loadCountyEvents(county);
        if (events.length >= EVENT_LIMIT) {
          // Compiling a truncated slice and presenting it as the county is how
          // a partial answer becomes a confident one.
          yield* Effect.log(
            `county ${county}: hit the ${EVENT_LIMIT} event ceiling; the compiled ` +
              `view is incomplete and is reported as such`,
          );
        }
        if (events.length === 0) {
          return Option.none<{
            readonly payload: string;
            readonly records: readonly CountyRecord[];
          }>();
        }
        // Each run persisted its learned mapping, field-level confidence and
        // all. Passing {} made the resolver fall back to document-class
        // confidence, so the worker settled conflicting owners and assessments
        // differently from the CLI and independently of measured mapping
        // quality - which is exactly the provenance the payload advertises.
        const trust = yield* Effect.promise(async () => {
          const sourceIds = [...new Set(events.map((e) => e.source_id))];
          if (sourceIds.length === 0) return {};
          const runs = await db().run.findMany({
            where: { sourceId: { in: sourceIds }, ok: true },
            orderBy: { startedAt: "desc" },
            select: { sourceId: true, mapping: true },
          });
          const out: Record<string, Record<string, number>> = {};
          for (const run of runs) {
            if (out[run.sourceId] !== undefined) continue; // newest run wins
            const mapping = run.mapping as
              | { readonly fields?: ReadonlyArray<Record<string, unknown>> }
              | null;
            const fields = mapping?.fields;
            if (fields === undefined) continue;
            const perField: Record<string, number> = {};
            for (const field of fields) {
              const name = field.field;
              const confidence = field.confidence;
              if (typeof name === "string" && typeof confidence === "number") {
                perField[name] = confidence;
              }
            }
            if (Object.keys(perField).length > 0) out[run.sourceId] = perField;
          }
          return out;
        });
        const payload = yield* Effect.promise(() =>
          compileCounty(schemaJson, events, trust, county),
        );
        const parsed = JSON.parse(payload) as { readonly records: readonly CountyRecord[] };
        const jurisdiction = events[0]?.property_key.split("|")[0] ?? county;
        yield* persist(jurisdiction, parsed.records);
        yield* bucket.put(
          cacheKey,
          JSON.stringify({ stamp, at: new Date().toISOString(), payload, abi }),
        );
        return Option.some({ payload, records: parsed.records });
      });

    const warmStatusKey = (canonical: string): string =>
      `warming/${canonical}.json`;

    const readWarmStatus = (canonical: string) =>
      Effect.gen(function* () {
        const object = yield* bucket.get(warmStatusKey(canonical));
        if (object === null) return undefined;
        return yield* Schema.decodeUnknownEffect(CountyWarmStatus)(
          JSON.parse(yield* object.text()),
        ).pipe(Effect.catch(() => Effect.succeed(undefined)));
      }).pipe(Effect.orDie);

    const writeWarmStatus = (status: CountyWarmStatusValue) =>
      bucket.put(warmStatusKey(status.canonical), JSON.stringify(status)).pipe(
        Effect.orDie,
      );

    const CountyWarmWorkflow = Cloudflare.Workflow(
      "CountyWarmWorkflow",
      Effect.succeed(
        Effect.fn(function* (input: {
          readonly county: string;
          readonly tenantId?: string;
          readonly requiredCoverage?: readonly string[];
          readonly notifyTenant?: boolean;
        }) {
          const canonical = yield* resolveJurisdiction(input.county);
          const initialStamp = yield* countyStamp(input.county);
          const event = yield* Cloudflare.Workflows.WorkflowEvent;
          const requestedAt =
            (yield* readWarmStatus(canonical))?.requestedAt ??
            event.timestamp.toISOString();
          const coverageKey = [...new Set(input.requiredCoverage ?? [])]
            .sort()
            .join(",");
          const running: CountyWarmStatusValue = {
            county: input.county,
            canonical,
            stamp: initialStamp,
            state: "running",
            instanceId: event.instanceId,
            requestedAt,
            updatedAt: new Date().toISOString(),
            coverageKey,
            ...(input.notifyTenant === true ? { notifyTenant: true } : {}),
          };
          yield* writeWarmStatus(running);
          const notify = (ok: boolean, error?: string) =>
            input.tenantId === undefined || input.notifyTenant !== true
              ? Effect.void
              : Cloudflare.Workflows.task(
                  "notify-tenant",
                  Effect.tryPromise({
                    try: async () => {
                      const response = await fetch(`${publicOrigin}/warm/complete`, {
                        method: "POST",
                        headers: {
                          "content-type": "application/json",
                          "x-datadiver-internal": Redacted.value(sendblueSecret),
                        },
                        body: JSON.stringify({
                          tenantId: input.tenantId,
                          county: input.county,
                          canonical,
                          instanceId: event.instanceId,
                          ok,
                          ...(error === undefined ? {} : { error }),
                        }),
                      });
                      const body = await response.text();
                      if (!response.ok) {
                        throw new Error(
                          `county notification returned ${response.status}: ${body.slice(0, 400)}`,
                        );
                      }
                    },
                    catch: (cause): Error =>
                      cause instanceof Error ? cause : new Error(String(cause)),
                  }).pipe(Effect.orDie),
                  {
                    retries: {
                      limit: 3,
                      delay: "15 seconds",
                      backoff: "exponential",
                    },
                    timeout: "2 minutes",
                  },
                );

          let admittedSources = 0;
          if (initialStamp === "0:none" && input.tenantId === undefined) {
            const error = `no property records or linked researcher for ${input.county}`;
            yield* writeWarmStatus({
              ...running,
              state: "error",
              updatedAt: new Date().toISOString(),
              error,
            });
            yield* notify(false, error);
            return { ok: false, county: input.county, error };
          }
          const requiredCoverage = input.requiredCoverage ?? [];
          if (
            input.tenantId !== undefined &&
            (initialStamp === "0:none" || requiredCoverage.length > 0)
          ) {
            const discovered = yield* Effect.exit(
              Cloudflare.Workflows.task(
                "discover-county-sources",
                discoverCountySources(
                  input.tenantId,
                  input.county,
                  requiredCoverage,
                ).pipe(Effect.orDie),
                {
                  retries: {
                    limit: 2,
                    delay: "15 seconds",
                    backoff: "exponential",
                  },
                  timeout: "5 minutes",
                },
              ),
            );
            if (Exit.isFailure(discovered)) {
              const error =
                Cause.pretty(discovered.cause).slice(0, 400) ||
                `county source discovery failed`;
              yield* writeWarmStatus({
                ...running,
                state: "error",
                updatedAt: new Date().toISOString(),
                error,
              });
              yield* notify(false, error);
              return { ok: false, county: input.county, error };
            }
            const admitted = yield* Effect.exit(
              Cloudflare.Workflows.task(
                "validate-county-sources",
                validateSourceCandidates(
                  discovered.value.jurisdiction,
                  discovered.value.candidates,
                ).pipe(
                  Effect.map((result) => ({
                    tested: result.probes.length,
                    admitted: result.admitted.length,
                    admissionError: result.admissionError,
                  })),
                  Effect.orDie,
                ),
                {
                  retries: {
                    limit: 2,
                    delay: "20 seconds",
                    backoff: "exponential",
                  },
                  timeout: "10 minutes",
                },
              ),
            );
            if (Exit.isFailure(admitted)) {
              const error =
                Cause.pretty(admitted.cause).slice(0, 400) ||
                `county source validation failed`;
              yield* writeWarmStatus({
                ...running,
                state: "error",
                updatedAt: new Date().toISOString(),
                error,
              });
              yield* notify(false, error);
              return { ok: false, county: input.county, error };
            }
            const admission = admitted.value;
            if (
              initialStamp === "0:none" &&
              (admission.admissionError !== undefined || admission.admitted === 0)
            ) {
              const error =
                admission.admissionError ??
                `no verified machine-readable source found for ${input.county}`;
              yield* writeWarmStatus({
                ...running,
                state: "error",
                updatedAt: new Date().toISOString(),
                error,
              });
              yield* notify(false, error);
              return { ok: false, county: input.county, error };
            }
            admittedSources = admission.admitted;
          }

          let properties = 0;
          let finalStamp = initialStamp;
          let stable = false;
          let compileError = "";
          for (let pass = 1; pass <= 3; pass += 1) {
            const compileStamp = yield* countyStamp(input.county);
            const compiled = yield* Effect.exit(
              Cloudflare.Workflows.task(
                `compile-county-${pass}`,
                Effect.gen(function* () {
                  const result = Option.getOrUndefined(
                    yield* compileCountyPayload(input.county).pipe(Effect.orDie),
                  );
                  if (result === undefined) {
                    return yield* Effect.die(
                      new Error(`no events found for ${input.county}`),
                    );
                  }
                  return { properties: result.records.length };
                }),
                {
                  retries: {
                    limit: 3,
                    delay: "15 seconds",
                    backoff: "exponential",
                  },
                  timeout: "20 minutes",
                },
              ),
            );
            if (Exit.isFailure(compiled)) {
              compileError =
                Cause.pretty(compiled.cause).slice(0, 400) ||
                "county compile failed";
              break;
            }
            properties = compiled.value.properties;
            finalStamp = yield* countyStamp(input.county);
            if (finalStamp === compileStamp) {
              stable = true;
              break;
            }
          }

          if (!stable) {
            const error =
              compileError === ""
                ? `county records changed throughout three compile passes`
                : compileError;
            yield* writeWarmStatus({
              ...running,
              state: "error",
              updatedAt: new Date().toISOString(),
              error,
            });
            yield* notify(false, error);
            return { ok: false, county: input.county, error };
          }

          yield* writeWarmStatus({
            ...running,
            stamp: finalStamp,
            state: "complete",
            updatedAt: new Date().toISOString(),
            properties,
          });
          yield* notify(true);
          return {
            ok: true,
            county: input.county,
            properties,
            admittedSources,
          };
        }),
      ),
    );
    const countyWarmWorkflow = yield* CountyWarmWorkflow;

    const workflowDetails = (instanceId: string) =>
      countyWarmWorkflow.get(instanceId).pipe(
        Effect.flatMap((instance) => instance.status()),
        Effect.catchCause(() => Effect.succeed(undefined)),
      );

    const workflowStatus = (instanceId: string) =>
      workflowDetails(instanceId).pipe(
        Effect.map((status) => status?.status),
      );

    const workflowIsActive = (status: string | undefined): boolean =>
      status === "queued" ||
      status === "running" ||
      status === "paused" ||
      status === "waiting" ||
      status === "waitingForPause" ||
      status === "unknown";

    const ensureCountyWarm = (
      county: string,
      tenantId?: string,
      requiredCoverage: readonly string[] = [],
      notifyTenant = false,
    ) =>
      Effect.gen(function* () {
        const canonical = yield* resolveJurisdiction(county);
        const stamp = yield* countyStamp(county);
        const { abi } = yield* Effect.promise(() => engineVersion());
        const existing = yield* readWarmStatus(canonical);
        const coverageKey = [...new Set(requiredCoverage)].sort().join(",");
        const nowMs = Date.now();
        const existingAge =
          existing === undefined
            ? Number.POSITIVE_INFINITY
            : nowMs - Date.parse(existing.updatedAt);
        const existingRuntime =
          existing === undefined || existing.instanceId === ""
            ? undefined
            : yield* workflowStatus(existing.instanceId);
        const existingCoverageKey = existing?.coverageKey ?? "";
        const existingSatisfiesRequest =
          existingCoverageKey === coverageKey &&
          (!notifyTenant || existing?.notifyTenant === true);
        if (
          existing !== undefined &&
          existingAge < ACTIVE_WARM_MS &&
          existingSatisfiesRequest &&
          existing.state !== "error"
        ) {
          if (workflowIsActive(existingRuntime)) {
            return existing;
          }
          if (existingRuntime === undefined && shouldReuseWarm(existing, nowMs)) {
            return existing;
          }
        }

        const baseInstanceId = yield* Effect.promise(() =>
          countyWorkflowId(canonical, stamp, abi, "", coverageKey),
        );
        if (
          existing !== undefined &&
          existing.state === "error" &&
          existing.instanceId === baseInstanceId &&
          existingAge < FAILED_WARM_RETRY_MS
        ) {
          return existing;
        }
        const retry = warmRetryKey(existing, existingRuntime, nowMs);
        const instanceId =
          retry === ""
            ? baseInstanceId
            : yield* Effect.promise(() =>
                countyWorkflowId(canonical, stamp, abi, retry, coverageKey),
              );

        const now = new Date().toISOString();
        const queued: CountyWarmStatusValue = {
          county,
          canonical,
          stamp,
          state: "queued",
          instanceId: "",
          requestedAt: now,
          updatedAt: now,
          coverageKey,
          ...(notifyTenant ? { notifyTenant: true } : {}),
        };
        let createError = "";
        const instance = yield* countyWarmWorkflow.create({
          id: instanceId,
          params: {
            county,
            ...(tenantId === undefined ? {} : { tenantId }),
            ...(requiredCoverage.length === 0 ? {} : { requiredCoverage }),
            ...(notifyTenant ? { notifyTenant: true } : {}),
          },
        }).pipe(
          Effect.catchCause((cause) => {
            createError = Cause.pretty(cause).slice(0, 400) || "could not queue county warm";
            return Effect.succeed(undefined);
          }),
        );
        if (instance === undefined) {
          const concurrent = yield* countyWarmWorkflow.get(instanceId).pipe(
            Effect.catchCause(() => Effect.succeed(undefined)),
          );
          if (concurrent !== undefined) {
            const current = yield* readWarmStatus(canonical);
            if (
              current !== undefined &&
              current.instanceId === instanceId &&
              (shouldReuseWarm(current, Date.now()) ||
                current.state === "complete" ||
                current.state === "error")
            ) {
              return current;
            }
            const runtime = yield* concurrent.status().pipe(
              Effect.catchCause(() => Effect.succeed(undefined)),
            );
            const state =
              runtime?.status === "complete"
                ? "complete"
                : runtime?.status === "errored" ||
                    runtime?.status === "terminated"
                  ? "error"
                  : runtime?.status === "queued"
                    ? "queued"
                    : "running";
            const output =
              typeof runtime?.output === "object" && runtime.output !== null
                ? runtime.output as { readonly properties?: unknown }
                : undefined;
            const joined: CountyWarmStatusValue = {
              ...queued,
              instanceId,
              state,
              updatedAt: new Date().toISOString(),
              ...(typeof output?.properties === "number"
                ? { properties: output.properties }
                : {}),
              ...(state === "error"
                ? {
                    error:
                      runtime?.error?.message ??
                      "county preparation did not complete",
                  }
                : {}),
            };
            yield* writeWarmStatus(joined);
            return joined;
          }
          const failed: CountyWarmStatusValue = {
            ...queued,
            state: "error",
            updatedAt: new Date().toISOString(),
            error: createError,
          };
          yield* writeWarmStatus(failed);
          return failed;
        }

        const current = yield* readWarmStatus(canonical);
        if (
          current !== undefined &&
          current.instanceId === instanceId &&
          (shouldReuseWarm(current, Date.now()) ||
            current.state === "complete" ||
            current.state === "error")
        ) {
          return current;
        }
        const withInstance: CountyWarmStatusValue = {
          ...queued,
          instanceId: instance.id,
          updatedAt: new Date().toISOString(),
        };
        yield* writeWarmStatus(withInstance);
        return withInstance;
      });

    const SourceRunWorkflow = Cloudflare.Workflow(
      "SourceRunWorkflow",
      Effect.succeed(
        Effect.fn(function* (input: {
          readonly sourceId: string;
          readonly runId: string;
        }) {
          const sources = yield* allSources;
          const source = sources.find((candidate) => candidate.id === input.sourceId);
          if (source === undefined) {
            return {
              outcome: {
                sourceId: input.sourceId,
                ok: false,
                stage: "fetch",
                error: "source is no longer enabled",
              } satisfies RunOutcome,
            };
          }
          const outcome = yield* Cloudflare.Workflows.task(
            "ingest-source",
            Effect.gen(function* () {
              const context = yield* Cloudflare.Workflows.WorkflowStepContext;
              const result = yield* runSource(
                source,
                `${input.runId}_a${context.attempt}`,
              );
              if (!result.ok) {
                return yield* Effect.die(
                  new Error(
                    `${result.sourceId}@${result.stage}: ${result.error ?? "ingestion failed"}`,
                  ),
                );
              }
              return result;
            }).pipe(Effect.orDie),
            {
              retries: {
                limit: 2,
                delay: "20 seconds",
                backoff: "exponential",
              },
              timeout: "5 minutes",
            },
          );
          const warm = yield* Cloudflare.Workflows.task(
            "refresh-county-signals",
            ensureCountyWarm(source.jurisdiction).pipe(Effect.orDie),
            {
              retries: {
                limit: 2,
                delay: "20 seconds",
                backoff: "exponential",
              },
              timeout: "2 minutes",
            },
          );
          return {
            outcome,
            warm: {
              county: source.jurisdiction,
              state: warm.state,
              instanceId: warm.instanceId,
            },
          };
        }),
      ),
    );
    const sourceRunWorkflow = yield* SourceRunWorkflow;

    const SmsTurnWorkflow = Cloudflare.Workflow(
      "SmsTurnWorkflow",
      Effect.succeed(
        Effect.fn(function* (input: { readonly payload: SmsPayload }) {
          return yield* Cloudflare.Workflows.task(
            "process-sms",
            Effect.tryPromise({
              try: async () => {
                const response = await fetch(`${publicOrigin}/sms/process`, {
                  method: "POST",
                  headers: {
                    "content-type": "application/json",
                    "x-datadiver-internal": Redacted.value(sendblueSecret),
                  },
                  body: JSON.stringify(input.payload),
                });
                const body = await response.text();
                if (!response.ok) {
                  throw new Error(
                    `SMS processing returned ${response.status}: ${body.slice(0, 400)}`,
                  );
                }
                return JSON.parse(body) as unknown;
              },
              catch: (cause): Error =>
                cause instanceof Error ? cause : new Error(String(cause)),
            }).pipe(Effect.orDie),
            {
              retries: {
                limit: 2,
                delay: "10 seconds",
                backoff: "exponential",
              },
              timeout: "5 minutes",
            },
          );
        }),
      ),
    );
    const smsTurnWorkflow = yield* SmsTurnWorkflow;

    const queueSweep = Effect.gen(function* () {
      const sources = yield* allSources;
      const createdAt = new Date().toISOString();
      const nonce = crypto.randomUUID().replace(/-/g, "").slice(0, 8);
      const id = `sweep_${Date.now().toString(36)}_${nonce}`;
      const runId = `run_${Date.now().toString(36)}_${nonce}`;
      const sourceIds = sources.map((source) => source.id);
      if (sourceIds.length === 0) {
        return { id, runId, createdAt, sourceIds, instances: [] as string[] };
      }
      const instances = yield* sourceRunWorkflow.createBatch(
        sourceIds.map((sourceId) => ({
          id: `${id}_${sourceId}`,
          params: { sourceId, runId },
          retention: {
            successRetention: "1 day",
            errorRetention: "7 days",
          },
        })),
      );
      yield* bucket.put(
        `sweeps/${id}.json`,
        JSON.stringify({ id, runId, createdAt, sourceIds }),
      );
      return {
        id,
        runId,
        createdAt,
        sourceIds,
        instances: instances.map((instance) => instance.id),
      };
    });

    const sweepStatus = (id: string) =>
      Effect.gen(function* () {
        const object = yield* bucket.get(`sweeps/${id}.json`);
        if (object === null) return undefined;
        const manifest = yield* Schema.decodeUnknownEffect(SweepManifest)(
          JSON.parse(yield* object.text()),
        ).pipe(Effect.catch(() => Effect.succeed(undefined)));
        if (manifest === undefined) return undefined;
        const sources = yield* Effect.all(
          manifest.sourceIds.map((sourceId) =>
            sourceRunWorkflow
              .get(`${manifest.id}_${sourceId}`)
              .pipe(
                Effect.flatMap((instance) => instance.status()),
                Effect.map((status) => ({
                  sourceId,
                  state: status.status,
                  ...(status.output === undefined ? {} : { output: status.output }),
                  ...(status.error === undefined || status.error === null
                    ? {}
                    : { error: status.error.message }),
                })),
                Effect.catchCause((cause) =>
                  Effect.succeed({
                    sourceId,
                    state: "unknown",
                    error: Cause.pretty(cause).slice(0, 300),
                  }),
                ),
              ),
          ),
          { concurrency: 8 },
        );
        return { ...manifest, sources };
      });

    /// True when the county can be answered from R2 without compiling.
    ///
    /// Compiling inline is not merely slow: a cold Norfolk burned about three
    /// minutes of CPU and the request died with a Cloudflare 1101, so the
    /// texter got nothing at all. A reply that admits it is warming beats a
    /// worker that is killed mid-answer.
    const countyIsWarm = (county: string) =>
      Effect.gen(function* () {
        const canonical = yield* resolveJurisdiction(county);
        const cached = yield* bucket.get(`compiled/${canonical}.json`);
        if (cached === null) return false;
        const decoded = yield* Schema.decodeUnknownEffect(CompiledCache)(
          JSON.parse(yield* cached.text()),
        ).pipe(Effect.catch(() => Effect.succeed(undefined)));
        if (decoded === undefined) return false;
        const { abi } = yield* Effect.promise(() => engineVersion());
        return (
          decoded.abi === abi &&
          decoded.stamp === (yield* countyStamp(county))
        );
      }).pipe(Effect.catch(() => Effect.succeed(false)));

    const countyMatches = (county: string) =>
      Effect.gen(function* () {
        const compiled = Option.getOrUndefined(yield* compileCountyPayload(county));
        if (compiled === undefined) return [] as readonly PropertyMatch[];
        const sourceIds = [
          ...new Set(
            compiled.records.flatMap((record) =>
              record.events.map((event) => event.source),
            ),
          ),
        ];
        const runs = sourceIds.length === 0
          ? []
          : yield* Effect.promise(() =>
              db().run.findMany({
                where: { sourceId: { in: sourceIds }, ok: true },
                orderBy: { startedAt: "desc" },
                select: { sourceId: true, truncated: true },
              }),
            );
        const latest = new Map<string, boolean>();
        for (const run of runs) {
          if (!latest.has(run.sourceId)) latest.set(run.sourceId, run.truncated);
        }
        const eventCount = Number((yield* countyStamp(county)).split(":")[0] ?? 0);
        const dataComplete =
          eventCount <= EVENT_LIMIT &&
          sourceIds.length > 0 &&
          sourceIds.every((sourceId) => latest.get(sourceId) === false);
        return compiled.records
          .map((record): PropertyMatch => {
            const sourceIds = new Set(record.events.map((e) => e.source));
            const measured = [
              ...(record.signals.delinquent_amount === undefined ? [] : ["owed"]),
              ...(record.signals.assessed_value === undefined ? [] : ["assessed"]),
              ...(record.signals.debt_to_value === undefined ? [] : ["debtToValue"]),
              ...(record.signals.code_violations === undefined ? [] : ["violations"]),
              "sources",
            ];
            return {
              propertyKey: record.keys[0] ?? "",
              address: record.fields.address?.value ?? "",
              owner: record.fields.owner?.value ?? "",
              mailing: record.fields.mailing_address?.value ?? "",
              phone: record.fields.owner_phone?.value ?? "",
              email: record.fields.owner_email?.value ?? "",
              lifecycleState: record.lifecycle_state,
              owed: record.signals.delinquent_amount ?? 0,
              assessed: record.signals.assessed_value ?? 0,
              debtToValue: record.signals.debt_to_value ?? 0,
              violations: record.signals.code_violations ?? 0,
              sources: sourceIds.size,
              signals: record.signals,
              measured,
              dataComplete,
            };
          })
          .filter((match) => match.address !== "");
      });

    yield* Cloudflare.Workers.cron("0 * * * *", () =>
      Effect.gen(function* () {
        const sweep = yield* queueSweep;
        yield* sweep.sourceIds.length === 0
          ? Effect.logError("cron sweep: no enabled sources; POST /seed to import the bundled list")
          : Effect.log(
              `cron sweep ${sweep.id}: queued ${sweep.sourceIds.length} source workflows`,
            );
      }),
    );

    const handleFetch = Effect.gen(function* () {
        const request = yield* HttpServerRequest.HttpServerRequest;
        const url = new URL(request.url, "http://worker");
        const origin = requestOrigin(request.url, request.headers, publicOrigin);

        if (url.pathname === "/warm/complete" && request.method === "POST") {
          if (
            request.headers["x-datadiver-internal"] !==
            Redacted.value(sendblueSecret)
          ) {
            return json({ ok: false, error: "not found" }, 404);
          }
          const bodyText = yield* request.text;
          const parsed = yield* Effect.try({
            try: () => JSON.parse(bodyText) as WarmNotificationPayload,
            catch: (cause): Error =>
              cause instanceof Error ? cause : new Error(String(cause)),
          }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
          if (
            parsed instanceof Error ||
            parsed.tenantId === undefined ||
            parsed.county === undefined ||
            parsed.canonical === undefined ||
            parsed.instanceId === undefined ||
            parsed.ok === undefined
          ) {
            return json({ ok: false, error: "invalid county notification" }, 400);
          }
          const tenantId = parsed.tenantId;
          const county = parsed.county;
          const canonical = parsed.canonical;
          const instanceId = parsed.instanceId;
          const notificationKey =
            `notifications/county/${instanceId}.json`;
          if ((yield* bucket.get(notificationKey)) !== null) {
            return json({ ok: true, duplicate: true });
          }
          const tenant = yield* Effect.promise(() =>
            db().tenant.findUnique({
              where: { id: tenantId },
              select: { phone: true },
            }),
          );
          if (tenant === null) {
            yield* bucket.put(
              notificationKey,
              JSON.stringify({ skipped: "tenant removed" }),
            );
            return json({ ok: true, skipped: true });
          }
          const thread = threadFor(tenantId);
          const snapshot = yield* thread.snapshot();
          const currentCounty = snapshot.profile?.county ?? snapshot.county;
          const currentCanonical =
            currentCounty === ""
              ? ""
              : yield* resolveJurisdiction(currentCounty);
          if (
            currentCanonical !== "" &&
            currentCanonical !== canonical
          ) {
            yield* bucket.put(
              notificationKey,
              JSON.stringify({ skipped: "market changed" }),
            );
            return json({ ok: true, skipped: true });
          }

          let outcome: HandleOutcome;
          if (!parsed.ok) {
            outcome = yield* thread.applyScout({
              userText: "county scan failed",
              reply:
                `The ${county.replace(/_/g, " ")} source check stopped ` +
                `before I could verify the records your search needs. I did not ` +
                `guess or report a false zero. Reply RETRY to run the source check again.`,
            });
          } else {
            const candidates = yield* countyMatches(county);
            const coverage = coverageFor(snapshot.tree.graph, candidates);
            if (!coverage.ready) {
              const missing = coverage.missing.join(", ");
              outcome = yield* thread.applyScout({
                  userText: "county scan completed",
                  reply:
                  `The ${county.replace(/_/g, " ")} pass loaded ` +
                  `${candidates.length} real propert${candidates.length === 1 ? "y" : "ies"}, ` +
                  `but the official feeds still lack ${missing}. I cannot rank ` +
                  `them as leads without guessing. Reply RETRY to look for another official source.`,
              });
            } else {
              outcome = yield* thread.handleMessage({
                text: "review",
                candidates,
                codexAccount: "",
                coverage,
              });
            }
          }

          const delivered = yield* Effect.tryPromise({
            try: async () => {
              await sender.send({
                to: tenant.phone,
                from: "",
                body: outcome.reply,
              });
              const prisma = db();
              await prisma.message.create({
                data: {
                  tenantId,
                  role: "scout",
                  body: outcome.reply,
                },
              });
              const first = outcome.evaluations[0];
              if (first !== undefined) {
                const treeRow = await prisma.decisionTree.findUnique({
                  where: {
                    tenantId_name_version: {
                      tenantId,
                      name: first.treeName,
                      version: first.treeVersion,
                    },
                  },
                });
                await prisma.evaluation.createMany({
                  data: outcome.evaluations.map((evaluation) => ({
                    tenantId,
                    propertyKey: evaluation.propertyKey,
                    decisionTreeId: treeRow?.id ?? null,
                    treeVersion: evaluation.treeVersion,
                    outcome: evaluation.outcome,
                    trace: JSON.parse(JSON.stringify(evaluation.trace)) as object,
                  })),
                });
              }
            },
            catch: (cause): Error =>
              cause instanceof Error ? cause : new Error(String(cause)),
          }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
          if (delivered instanceof Error) {
            return json({ ok: false, error: delivered.message }, 502);
          }
          yield* bucket.put(
            notificationKey,
            JSON.stringify({ deliveredAt: new Date().toISOString() }),
          );
          return json({ ok: true, notified: true });
        }

        if (url.pathname === "/health") {
          const version = yield* Effect.tryPromise({
            try: () => engineVersion(),
            catch: (cause): Error =>
              cause instanceof Error ? cause : new Error(String(cause)),
          }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
          if (version instanceof Error) {
            return json({ ok: false, error: version.message }, 500);
          }
          const counted = yield* Effect.tryPromise({
            try: () => db().source.count({ where: { enabled: true } }),
            catch: (cause): Error =>
              cause instanceof Error ? cause : new Error(String(cause)),
          }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
          if (counted instanceof Error) {
            return json({ ok: false, ...version, error: counted.message }, 500);
          }
          return json({ ok: true, ...version, sources: counted });
        }

        if (url.pathname === "/seed" && request.method === "POST") {
          const bodyText = yield* request.text;
          const supplied =
            bodyText.trim() === ""
              ? undefined
              : yield* Effect.try({
                  try: (): unknown => JSON.parse(bodyText),
                  catch: (cause): Error =>
                    cause instanceof Error ? cause : new Error(String(cause)),
                }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
          if (supplied instanceof Error) {
            return json({ ok: false, error: `body is not json: ${supplied.message}` }, 400);
          }
          const seed = supplied === undefined ? bundledSeed : parseSeed(supplied);
          if (seed.sources.length === 0) {
            return json({ ok: false, error: "no usable seed sources", rejected: seed.rejected }, 400);
          }
          const planned = yield* Effect.tryPromise({
            try: async () => {
              const prisma = db();
              const existing = await prisma.source.findMany({ select: { id: true } });
              const plan = planSeed(
                seed.sources,
                existing.map((row) => row.id),
              );
              if (plan.create.length > 0) {
                await prisma.source.createMany({
                  data: plan.create.map((source) => ({
                    id: source.id,
                    name: source.name,
                    url: source.url,
                    jurisdiction: source.jurisdiction,
                    asOf: source.as_of ?? null,
                    headers: source.headers ?? undefined,
                    method: source.method ?? null,
                    body: source.body ?? null,
                  })),
                  skipDuplicates: true,
                });
              }
              return plan;
            },
            catch: (cause): Error =>
              cause instanceof Error ? cause : new Error(String(cause)),
          }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
          if (planned instanceof Error) {
            return json({ ok: false, error: planned.message }, 500);
          }
          return json({
            ok: true,
            imported: planned.create.map((source) => source.id),
            skipped: planned.skipped,
            rejected: seed.rejected,
          });
        }

        if (url.pathname === "/warm/status") {
          const asked = url.searchParams.get("county") ?? "";
          if (asked === "") return json({ ok: false, error: "county is required" }, 400);
          const canonical = yield* resolveJurisdiction(asked);
          const status = yield* readWarmStatus(canonical);
          if (status === undefined) {
            return json({ ok: true, county: asked, state: "idle" });
          }
          const runtime =
            status.instanceId === ""
              ? undefined
              : yield* workflowStatus(status.instanceId);
          return json({
            ok: status.state !== "error" && runtime !== "errored",
            ...status,
            ...(runtime === undefined ? {} : { workflowState: runtime }),
          });
        }

        if (url.pathname === "/warm" && request.method === "POST") {
          const asked = url.searchParams.get("county") ?? "";
          const target = yield* asked !== ""
            ? Effect.succeed(asked)
            : Effect.promise(async () => {
                const rows = await db().property.groupBy({
                  by: ["jurisdiction"],
                  _count: { key: true },
                  orderBy: { _count: { key: "desc" } },
                  take: 1,
                });
                return rows[0]?.jurisdiction ?? "";
              });
          if (target === "") return json({ ok: false, error: "no county to warm" }, 404);
          const status = yield* ensureCountyWarm(target);
          return json({
            ok: status.state !== "error",
            ...status,
          }, status.state === "error" ? 503 : 202);
        }

        if (url.pathname === "/run" && request.method === "POST") {
          const sweep = yield* queueSweep;
          if (sweep.sourceIds.length === 0) {
            return json(
              { ok: false, error: "no enabled sources; POST /seed to import the bundled list" },
              503,
            );
          }
          return json({
            ok: true,
            state: "queued",
            sweepId: sweep.id,
            sources: sweep.sourceIds.length,
            statusUrl: `/sweep/${sweep.id}`,
          }, 202);
        }

        const sweepMatch = /^\/sweep\/([a-z0-9_]+)$/.exec(url.pathname);
        if (sweepMatch !== null && request.method === "GET") {
          const status = yield* sweepStatus(sweepMatch[1] ?? "");
          if (status === undefined) {
            return json({ ok: false, error: "unknown sweep" }, 404);
          }
          const complete = status.sources.filter((source) => source.state === "complete");
          const failed = status.sources.filter(
            (source) =>
              source.state === "errored" || source.state === "terminated",
          );
          return json({
            ok: failed.length === 0,
            state:
              complete.length + failed.length === status.sources.length
                ? failed.length === 0
                  ? "complete"
                  : "error"
                : "running",
            sweepId: status.id,
            createdAt: status.createdAt,
            completed: complete.length,
            failed: failed.length,
            total: status.sources.length,
            sources: status.sources,
          });
        }

        const runMatch = /^\/run\/([a-z0-9_]+)$/.exec(url.pathname);
        if (runMatch !== null && request.method === "POST") {
          const merged = yield* allSources;
          const source = merged.find((s) => s.id === runMatch[1]);
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

        if (
          (url.pathname === "/sms" || url.pathname === "/sms/process") &&
          request.method === "POST"
        ) {
          const bodyText = yield* request.text;
          const parsed = yield* Effect.try({
            try: () => JSON.parse(bodyText) as SmsPayload,
            catch: (cause): Error =>
              cause instanceof Error ? cause : new Error(String(cause)),
          }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
          if (parsed instanceof Error) {
            return json({ ok: false, error: "request body must be valid JSON" }, 400);
          }
          const phone = (parsed.from_number ?? "").replace(/[^+0-9]/g, "");
          const ourNumber = (parsed.to_number ?? parsed.number ?? "").replace(/[^+0-9]/g, "");
          const text = parsed.content ?? "";
          const messageHandle = parsed.message_handle?.trim() ?? "";
          const secret = Redacted.value(sendblueSecret);
          const probe =
            request.headers["x-datadiver-probe"] === secret;
          const internal =
            url.pathname === "/sms/process" &&
            request.headers["x-datadiver-internal"] === secret;
          if (url.pathname === "/sms/process" && !internal) {
            return json({ ok: false, error: "not found" }, 404);
          }
          if (phone === "" || text === "") {
            return json({ ok: false, error: "from_number and content are required" }, 400);
          }
          if (url.pathname === "/sms" && !probe) {
            const instanceId = yield* Effect.promise(() =>
              smsWorkflowId(messageHandle),
            );
            const queued = yield* smsTurnWorkflow.create({
              id: instanceId,
              params: { payload: parsed },
              retention: {
                successRetention: "1 day",
                errorRetention: "7 days",
              },
            }).pipe(
              Effect.map(() => true),
              Effect.catchCause(() =>
                smsTurnWorkflow.get(instanceId).pipe(
                  Effect.map(() => true),
                  Effect.catchCause(() => Effect.succeed(false)),
                ),
              ),
            );
            if (!queued) {
              return json(
                { ok: false, error: "could not queue the SMS turn" },
                503,
              );
            }
            return json({ ok: true, queued: true });
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
              if (!probe && messageHandle !== "") {
                try {
                  await prisma.inboundDelivery.create({
                    data: { messageHandle, tenantId: tenant.id },
                  });
                } catch (cause) {
                  if (
                    typeof cause === "object" &&
                    cause !== null &&
                    "code" in cause &&
                    cause.code === "P2002"
                  ) {
                    return {
                      tenantId: tenant.id,
                      codexAccount: credential?.accountLabel ?? "",
                      duplicate: true,
                    };
                  }
                  throw cause;
                }
              }
              return {
                tenantId: tenant.id,
                codexAccount: credential?.accountLabel ?? "",
                duplicate: false,
              };
            },
            catch: (cause): Error =>
              cause instanceof Error ? cause : new Error(String(cause)),
          }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
          if (preflight instanceof Error) {
            return json({ ok: false, error: preflight.message }, 500);
          }
          if (preflight.duplicate) {
            return json({ ok: true, duplicate: true });
          }
          const showTyping = Effect.tryPromise({
            try: () => sender.typing(phone, ourNumber),
            catch: (cause): Error =>
              cause instanceof Error ? cause : new Error(String(cause)),
          }).pipe(Effect.catch(() => Effect.void));
          if (!probe) yield* showTyping;

          const lower = text.trim().toLowerCase();

          // A single-use link, minted whenever the user needs to sign in.
          const mintConnectUrl = Effect.gen(function* () {
            const deviceCode = yield* requestDeviceCode();
            const token = `${crypto.randomUUID()}${crypto.randomUUID()}`.replace(/-/g, "");
            yield* bucket.put(
              `connect/${token}`,
              JSON.stringify({
                phone,
                deviceAuthId: deviceCode.deviceAuthId,
                userCode: deviceCode.userCode,
                intervalSeconds: deviceCode.intervalSeconds,
                createdAt: new Date().toISOString(),
              }),
            );
            return `${origin}/connect/${token}`;
          });

          // Only slash commands bypass the model. Everything else is
          // conversation, including "hi".
          const wants = slashCommand(text);
          if (wants === "help" || wants === "status") {
            const reply =
              preflight.codexAccount === ""
                ? `Data Diver. You are not signed in yet, so text /connect to link ` +
                  `your ChatGPT account. After that just talk to me normally.\n\n` +
                  `/connect  /reset  /delete  /help`
                : `Signed in as ${preflight.codexAccount}. Just talk to me normally - ` +
                  `ask what matches, change your criteria, or name a county to scan.\n\n` +
                  `/logout  /reset  /delete  /help`;
            yield* Effect.tryPromise({
              try: () => sender.send({ to: phone, from: ourNumber, body: reply }),
              catch: (cause): Error =>
                cause instanceof Error ? cause : new Error(String(cause)),
            }).pipe(Effect.catch(() => Effect.void));
            return json({ ok: true, phone, reply });
          }

          if (wants === "connect" || wants === "login") {
            if (masterKeyB64 === "") {
              return json({ ok: false, phone, error: "credential store not configured" }, 503);
            }
            const link = yield* mintConnectUrl;
            const reply =
              preflight.codexAccount !== ""
                ? `Already signed in as ${preflight.codexAccount}. To switch accounts, ` +
                  `open this:\n${link}`
                : `Open this to sign in with ChatGPT:\n${link}\n\n` +
                  `Single use, expires in 15 minutes. Your tokens are stored encrypted.`;
            yield* Effect.tryPromise({
              try: () => sender.send({ to: phone, from: ourNumber, body: reply }),
              catch: (cause): Error =>
                cause instanceof Error ? cause : new Error(String(cause)),
            }).pipe(Effect.catch(() => Effect.void));
            return json({ ok: true, phone, reply });
          }

          if (wants === "logout" || wants === "disconnect") {
            const gone = yield* Effect.tryPromise({
              try: () => db().credential.deleteMany({ where: { tenantId: preflight.tenantId } }),
              catch: (cause): Error =>
                cause instanceof Error ? cause : new Error(String(cause)),
            }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
            const reply =
              gone instanceof Error
                ? `Could not sign you out: ${gone.message}`
                : `Signed out. Your criteria and history are kept; text /connect to ` +
                  `sign back in.`;
            yield* Effect.tryPromise({
              try: () => sender.send({ to: phone, from: ourNumber, body: reply }),
              catch: (cause): Error =>
                cause instanceof Error ? cause : new Error(String(cause)),
            }).pipe(Effect.catch(() => Effect.void));
            return json({ ok: !(gone instanceof Error), phone, reply });
          }

          if (wants === "reset" || wants === "reset-account") {
            const wiped = yield* Effect.tryPromise({
              try: async () => {
                const prisma = db();
                const where = { tenantId: preflight.tenantId };
                const [outreach, evaluations, trees, messages, criteria] = await Promise.all([
                  prisma.outreach.deleteMany({ where }),
                  prisma.evaluation.deleteMany({ where }),
                  prisma.decisionTree.deleteMany({ where }),
                  prisma.message.deleteMany({ where }),
                  prisma.criteria.deleteMany({ where }),
                ]);
                return (
                  outreach.count + evaluations.count + trees.count + messages.count +
                  criteria.count
                );
              },
              catch: (cause): Error =>
                cause instanceof Error ? cause : new Error(String(cause)),
            }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
            if (wiped instanceof Error) {
              return json({ ok: false, phone, error: wiped.message }, 500);
            }
            const thread = threadFor(preflight.tenantId);
            yield* thread.forget();
            const resetSnapshot = yield* thread.snapshot();
            const onboarding = yield* scoutTurn(
              preflight.tenantId,
              text,
              [],
              resetSnapshot,
              () => {},
            ).pipe(
              Effect.map((outcome): HandleOutcome | undefined => outcome),
              Effect.catch(() => Effect.succeed(undefined)),
            );
            const reply =
              `Reset complete. Your prior criteria, results and conversation were ` +
              `removed; your ChatGPT connection was kept.\n\n` +
              (onboarding?.reply ?? MODEL_UNAVAILABLE_REPLY);
            yield* Effect.tryPromise({
              try: () => sender.send({ to: phone, from: ourNumber, body: reply }),
              catch: (cause): Error =>
                cause instanceof Error ? cause : new Error(String(cause)),
            }).pipe(Effect.catch(() => Effect.void));
            return json({ ok: true, phone, reply, reset: wiped });
          }

          if (wants === "delete" || wants === "delete-account") {
            if (wants === "delete" && !/account/i.test(text)) {
              return json({
                ok: true,
                phone,
                reply: "Text /delete account to erase everything, including your Codex link.",
              });
            }
            const removed = yield* Effect.tryPromise({
              // The tenant row cascades to credentials, criteria, trees,
              // evaluations, outreach and messages.
              try: () => db().tenant.delete({ where: { id: preflight.tenantId } }),
              catch: (cause): Error =>
                cause instanceof Error ? cause : new Error(String(cause)),
            }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));
            if (removed instanceof Error) {
              return json({ ok: false, phone, error: removed.message }, 500);
            }
            const reply =
              `Account deleted. Everything held for ${phone} is gone: the Codex ` +
              `connection, criteria, decision trees, evaluations, outreach and the ` +
              `whole transcript.\n\nText again to start fresh as a new user.`;
            yield* Effect.tryPromise({
              try: () => sender.send({ to: phone, from: ourNumber, body: reply }),
              catch: (cause): Error =>
                cause instanceof Error ? cause : new Error(String(cause)),
            }).pipe(Effect.catch(() => Effect.void));
            return json({ ok: true, phone, reply, deleted: true });
          }
          const thread = threadFor(preflight.tenantId);
          const snapshot = yield* thread.snapshot();
          const county = snapshot.county;
          const market = snapshot.profile?.county ?? (snapshot.configured ? county : "");
          const candidates =
            market !== "" && (yield* countyIsWarm(market))
              ? yield* countyMatches(market)
              : [];
          let input: HandleMessageInput = {
            text,
            candidates,
            codexAccount: preflight.codexAccount,
          };
          if ((lower === "connect" || lower === "connect codex") && masterKeyB64 !== "") {
            const deviceCode = yield* requestDeviceCode();
            const token = `${crypto.randomUUID()}${crypto.randomUUID()}`.replace(/-/g, "");
            yield* bucket.put(
              `connect/${token}`,
              JSON.stringify({
                phone,
                deviceAuthId: deviceCode.deviceAuthId,
                userCode: deviceCode.userCode,
                intervalSeconds: deviceCode.intervalSeconds,
                createdAt: new Date().toISOString(),
              }),
            );
            input = { ...input, connectUrl: `${origin}/connect/${token}` };
          }

          if (preflight.codexAccount === "") {
            const link = masterKeyB64 === "" ? "" : yield* mintConnectUrl;
            const reply =
              link === ""
                ? `Data Diver is not configured to sign users in on this deployment.`
                : `Hi - I am Data Diver. I find distressed properties in county ` +
                  `records and text you the ones worth a call.\n\n` +
                  `Sign in with ChatGPT to start:\n${link}\n\n` +
                  `Then just talk to me normally. Single-use link, 15 minutes.`;
            yield* Effect.tryPromise({
              try: () => sender.send({ to: phone, from: ourNumber, body: reply }),
              catch: (cause): Error =>
                cause instanceof Error ? cause : new Error(String(cause)),
            }).pipe(Effect.catch(() => Effect.void));
            return json({ ok: true, phone, reply, signedIn: false });
          }

          if (!probe) yield* showTyping;
          let scoutError = "";
          let scoutDecision = "";
          let scoutProfileUpdate: ProfileUpdate | undefined;
          const scouted = yield* scoutTurn(
            preflight.tenantId,
            text,
            input.candidates,
            snapshot,
            (decision) => {
              scoutDecision = decision.kind;
              if (decision.kind === "update_profile") {
                scoutProfileUpdate = decision.update;
              }
            },
            probe,
          ).pipe(
            Effect.map((o): HandleOutcome | undefined => o),
            Effect.catch((cause) => {
              scoutError = cause.message;
              return Effect.succeed(undefined);
            }),
          );
          if (probe) {
            return json({
              ok: scouted !== undefined,
              brain: scouted !== undefined ? "codex" : "unavailable",
              ...(scouted === undefined ? { reply: MODEL_UNAVAILABLE_REPLY } : {}),
              ...(scouted?.reply === undefined || scouted.reply === ""
                ? {}
                : { brainReply: scouted.reply }),
              ...(scoutDecision === ""
                ? {}
                : { brainDecision: scoutDecision }),
              ...(scoutProfileUpdate === undefined
                ? {}
                : { brainProfileUpdate: scoutProfileUpdate }),
              ...(scoutError === "" ? {} : { scoutError }),
            });
          }
          let outcome =
            scouted ??
            ({
              reply: MODEL_UNAVAILABLE_REPLY,
              evaluations: [],
            } satisfies HandleOutcome);

          const draftRequest = outcome.draftRequest;
          if (draftRequest !== undefined) {
            const drafted = yield* draftOutreach(
              preflight.tenantId,
              draftRequest.match,
            ).pipe(
              Effect.catch((cause) => {
                scoutError = cause.message;
                return Effect.succeed(templateDraft(draftRequest.match));
              }),
            );
            outcome = yield* thread.attachDraft({
              userText: text,
              match: draftRequest.match,
              explanation: draftRequest.explanation,
              draft: drafted,
              requiresApproval: draftRequest.requiresApproval,
            });
          }

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
              const outreach = outcome.outreach;
              if (outreach !== undefined) {
                await prisma.outreach.create({
                  data: {
                    tenantId: preflight.tenantId,
                    propertyKey: outreach.propertyKey,
                    draft: outreach.draft,
                    status: "scheduled",
                    audience: outreach.audience,
                    scheduledFor: new Date(outreach.scheduledForIso),
                    approvedAt: outreach.approved ? new Date() : null,
                    simulated: true,
                  },
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
              await sender.send({ to: phone, from: ourNumber, body: outcome.reply });
            },
            catch: (cause): Error => (cause instanceof Error ? cause : new Error(String(cause))),
          }).pipe(Effect.catch((cause: Error) => Effect.succeed(cause)));

          return json({
            ok: !(delivery instanceof Error),
            phone,
            reply: outcome.reply,
            evaluations: outcome.evaluations.length,
            treeVersion: outcome.tree?.version,
            brain: scouted !== undefined ? "codex" : "unavailable",
            ...(scoutDecision === "" ? {} : { brainDecision: scoutDecision }),
            transport: sender.name,
            ...(scoutError !== "" ? { scoutError } : {}),
            ...(delivery instanceof Error ? { error: delivery.message } : {}),
          });
        }

        const connectTokenMatch = /^\/connect\/([a-f0-9]{32,})$/.exec(url.pathname);
        if (connectTokenMatch !== null && request.method === "GET") {
          const token = connectTokenMatch[1] ?? "";
          const connectState = Option.getOrUndefined(yield* loadConnectState(token));
          if (connectState === undefined) {
            yield* bucket.delete(`connect/${token}`);
            return json(
              { ok: false, error: "connect link is invalid; text /connect for a new one" },
              410,
            );
          }
          if (Date.now() - Date.parse(connectState.createdAt) > CONNECT_TTL_MS) {
            yield* bucket.delete(`connect/${token}`);
            return json({ ok: false, error: "connect link expired; text /connect again" }, 410);
          }
          const code = escapeHtml(connectState.userCode);
          const verificationUrl = escapeHtml(DEVICE_VERIFICATION_URL);
          const pollAfterMs = Math.max(2, Math.min(connectState.intervalSeconds, 15)) * 1_000;
          return HttpServerResponse.text(
            `<!doctype html><html><head><meta charset="utf-8">` +
              `<meta name="viewport" content="width=device-width,initial-scale=1">` +
              `<title>Connect Data Diver</title>` +
              `<style>body{font:16px system-ui;color:#171717;background:#f6f5f2;margin:0}` +
              `main{max-width:34rem;margin:10vh auto;padding:2rem}` +
              `section{background:white;border:1px solid #ddd8ce;border-radius:18px;padding:2rem}` +
              `code{display:block;font-size:2rem;font-weight:700;letter-spacing:.12em;margin:1.5rem 0}` +
              `a{display:inline-block;background:#171717;color:white;text-decoration:none;padding:.9rem 1.2rem;border-radius:10px}` +
              `#status{color:#625f58;margin-top:1.5rem}</style></head>` +
              `<body><main><section><h1>Connect ChatGPT</h1>` +
              `<p>Open ChatGPT sign-in and enter this one-time code:</p>` +
              `<code>${code}</code>` +
              `<a href="${verificationUrl}" target="_blank" rel="noopener">Open ChatGPT sign-in</a>` +
              `<p>After entering the code, return to this page to finish connecting.</p>` +
              `<p id="status">Waiting for authorization…</p></section></main>` +
              `<script>const status=document.getElementById("status");` +
              `async function poll(){try{const response=await fetch(location.pathname+"/status",{method:"POST"});` +
              `const body=await response.json();if(response.status===202){setTimeout(poll,${pollAfterMs});return}` +
              `if(response.ok&&body.connected){status.textContent="Connected. Return to Messages and continue the conversation.";return}` +
              `status.textContent=body.error||"Could not finish sign-in. Text /connect for a new code."` +
              `}catch{status.textContent="Connection check failed. Retrying…";setTimeout(poll,${pollAfterMs})}}` +
              `setTimeout(poll,${pollAfterMs});</script></body></html>`,
            { headers: { "content-type": "text/html; charset=utf-8" } },
          );
        }

        const connectStatusMatch =
          /^\/connect\/([a-f0-9]{32,})\/status$/.exec(url.pathname);
        if (connectStatusMatch !== null && request.method === "POST") {
          const token = connectStatusMatch[1] ?? "";
          const connectState = Option.getOrUndefined(yield* loadConnectState(token));
          if (connectState === undefined) {
            return json({ ok: false, error: "connect link is invalid or already used" }, 410);
          }
          if (Date.now() - Date.parse(connectState.createdAt) > CONNECT_TTL_MS) {
            yield* bucket.delete(`connect/${token}`);
            return json({ ok: false, error: "connect link expired; text CONNECT again" }, 410);
          }
          const authorized = yield* pollDeviceCode({
            deviceAuthId: connectState.deviceAuthId,
            userCode: connectState.userCode,
          });
          if (authorized.status === "pending") {
            return json({ ok: true, connected: false }, 202);
          }
          const tokens = yield* exchangeDeviceCode(authorized);
          const identity = yield* storeCredential(connectState.phone, tokens);
          yield* bucket.delete(`connect/${token}`);
          return json({
            ok: true,
            connected: true,
            account: identity.email !== "" ? identity.email : identity.accountId,
          });
        }

        if (url.pathname === "/connect/callback") {
          return json(
            { ok: false, error: "this login flow was replaced; text /connect for a new code" },
            410,
          );
        }

        if (url.pathname === "/connect/status" && request.method === "GET") {
          const phone = (url.searchParams.get("phone") ?? "").replace(/[^+0-9]/g, "");
          if (phone === "") return json({ ok: false, error: "phone is required" }, 400);
          const tenant = yield* Effect.promise(() =>
            db().tenant.findUnique({ where: { phone } }),
          );
          if (tenant === null) return json({ ok: true, connected: false });
          const credential = Option.getOrUndefined(
            yield* freshCredential(tenant.id, true),
          );
          if (credential === undefined) return json({ ok: true, connected: false });
          return json({
            ok: true,
            connected: true,
            account: credential.email !== "" ? credential.email : credential.accountId,
            refreshed: true,
            tokenObtainedAt: credential.obtainedAt,
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
        // Anything still unhandled would surface to a texter as a Cloudflare
        // 1101 with no body at all. Two were seen, both moments after a
        // deployment swapped the Durable Object class, and neither reproduced
        // on demand. Whatever the cause, an answer beats a dead request.
        Effect.catchCause((cause) =>
          Effect.succeed(
            json(
              {
                ok: false,
                error: "Data Diver hit an unexpected error; text again in a moment",
                detail: Cause.pretty(cause).slice(0, 400),
              },
              500,
            ),
          ),
        ),
      ),
    };
  }).pipe(
    Effect.provide(CodexEgressHostLive),
    Effect.provide(Cloudflare.Workers.CronEventSourceLive),
    Effect.provide(Cloudflare.R2.ReadWriteBucketBinding),
  ),
) {}
