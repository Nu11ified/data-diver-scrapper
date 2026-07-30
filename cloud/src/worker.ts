
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
import { legacyProfileText } from "./conversation/profile.ts";
import { SIGNAL_CATALOG, evaluate, validateGraph } from "./decision/graph.ts";
import { CodexError, complete } from "./codex/client.ts";
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
  CountyWarmStatus,
  shouldReuseWarm,
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

const CONNECT_TTL_MS = 15 * 60_000;
const USER_AGENT = "DataDiver/0.1 (public-record research; contact site operator)";
const MODEL_UNAVAILABLE_REPLY =
  `I could not reach the reasoning service, so I did not change your ` +
  `criteria or run anything. Please try again in a few minutes.`;

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
      Effect.runPromise(codexEgress.getByName("codex").request(request)),
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
      onDecision: (kind: string) => void,
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
        const context = {
          tree: snap.tree,
          configured: snap.configured,
          summary: snap.summary,
          recentTurns: snap.recentTurns,
          county: snap.county,
          candidateCount: candidates.length,
          qualifiedCount,
          extraSignals,
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
        onDecision(decision.kind);
        if (dryRun) {
          return {
            reply: "",
            evaluations: [],
          } satisfies HandleOutcome;
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
          });
          const lead = decision.text.trim();
          return {
            ...acted,
            reply: lead === "" ? acted.reply : `${lead}\n\n${acted.reply}`,
          };
        }
        if (decision.kind === "update_profile") {
          return yield* thread.handleMessage({
            text:
              snap.profile === undefined
                ? legacyProfileText(decision.update, text)
                : text,
            candidates,
            codexAccount: credential.accountId,
            onboardingUpdate: decision.update,
            onboardingLead: decision.text,
          });
        }
        if (decision.kind === "discover") {
          const jurisdiction = slugId(decision.jurisdiction);
          const runId = `run_${Date.now().toString(36)}`;
          const vetted = decision.candidates.slice(0, 4);
          const known = yield* Effect.promise(() =>
            db().source.findMany({
              where: { id: { in: vetted.map((c) => slugId(c.id)) } },
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
                    // The model often names the page a human would land on
                    // rather than the export behind it, so the site is walked
                    // and every page is put to the engine. The best page wins,
                    // and the engine still decides what counts as records.
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
                          if (outcome.ok && (outcome.records ?? 0) > (best.records ?? 0)) {
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
            (p) => p.outcome.ok && (p.outcome.records ?? 0) > 0,
          );
          // Every page the crawl probed left a Source row behind to hang events on.
          const dead = probes
            .flatMap((p) => p.attempted)
            .filter((o) => !(o.ok && (o.records ?? 0) > 0))
            .map((o) => o.sourceId)
            .filter((id) => !preexisting.has(id));
          const admission = yield* Effect.tryPromise({
            try: async () => {
              const prisma = db();
              if (admitted.length > 0) {
                await prisma.source.updateMany({
                  where: { id: { in: admitted.map((p) => p.outcome.sourceId) } },
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
          const lines = probes.map(({ outcome, crawledFrom }) =>
            outcome.ok
              ? `✓ ${outcome.sourceId}: ${outcome.records ?? 0} records, ` +
                `${Math.round((outcome.extractionRate ?? 0) * 100)}% extraction ` +
                `(classified ${outcome.classification ?? "unknown"})` +
                (crawledFrom === undefined ? "" : ` at ${crawledFrom}`) +
                (outcome.truncated === true
                  ? ` [capped at ${RECORD_CAP}; the source has more records than this run pulled]`
                  : "")
              : `✗ ${outcome.sourceId}: failed at ${outcome.stage}` +
                `${outcome.error !== undefined ? ` - ${outcome.error}` : ""}`,
          );
          const verdictText =
            admitted.length > 0
              ? `\n\nNow scanning ${jurisdiction}. Reply REVIEW to see matches.`
              : `\n\nNo candidate survived engine validation; nothing was added.`;
          const bookkeeping =
            admission instanceof Error
              ? `\n\nThe sources ran but could not be enrolled in the hourly sweep: ${admission.message}`
              : "";
          return yield* thread.applyScout({
            userText: text,
            reply:
              `${decision.text}\n\n` +
              `Validated ${probes.length} candidate source(s):\n${lines.join("\n")}` +
              verdictText +
              bookkeeping,
            ...(admitted.length > 0 ? { county: jurisdiction } : {}),
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
              ? fetchPaginated(source.url, limiter, fetchPage)
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

    const runAll = Effect.gen(function* () {
      const runId = `run_${Date.now().toString(36)}`;
      const merged = yield* allSources;
      return yield* Effect.all(
        merged.map((source) => runSource(source, runId)),
        { concurrency: 6 },
      );
    });

    const EVENT_LIMIT = 20_000;
    const COMPILED_TTL_MS = 6 * 60 * 60_000;

    const countySlug = (county: string): string =>
      county.toLowerCase().replace(/[^a-z0-9]+/g, "_").replace(/^_|_$/g, "");

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
          orderBy: { eventDate: "asc" },
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
            decoded.stamp === stamp &&
            Date.now() - Date.parse(decoded.at) < COMPILED_TTL_MS;
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
        Effect.fn(function* (input: { readonly county: string }) {
          const canonical = yield* resolveJurisdiction(input.county);
          const stamp = yield* countyStamp(input.county);
          const event = yield* Cloudflare.Workflows.WorkflowEvent;
          const requestedAt =
            (yield* readWarmStatus(canonical))?.requestedAt ??
            event.timestamp.toISOString();
          const running: CountyWarmStatusValue = {
            county: input.county,
            canonical,
            stamp,
            state: "running",
            instanceId: event.instanceId,
            requestedAt,
            updatedAt: new Date().toISOString(),
          };
          yield* writeWarmStatus(running);

          const compiled = yield* Effect.exit(
            Cloudflare.Workflows.task(
              "compile-county",
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
                timeout: "10 minutes",
              },
            ),
          );

          if (Exit.isFailure(compiled)) {
            const error = Cause.pretty(compiled.cause).split("\n")[0] ?? "county compile failed";
            yield* writeWarmStatus({
              ...running,
              state: "error",
              updatedAt: new Date().toISOString(),
              error,
            });
            return { ok: false, county: input.county, error };
          }

          yield* writeWarmStatus({
            ...running,
            state: "complete",
            updatedAt: new Date().toISOString(),
            properties: compiled.value.properties,
          });
          return {
            ok: true,
            county: input.county,
            properties: compiled.value.properties,
          };
        }),
      ),
    );
    const countyWarmWorkflow = yield* CountyWarmWorkflow;

    const workflowStatus = (instanceId: string) =>
      countyWarmWorkflow.get(instanceId).pipe(
        Effect.flatMap((instance) => instance.status()),
        Effect.map((status) => status.status),
        Effect.catchCause(() => Effect.succeed(undefined)),
      );

    const ensureCountyWarm = (county: string) =>
      Effect.gen(function* () {
        const canonical = yield* resolveJurisdiction(county);
        const stamp = yield* countyStamp(county);
        const existing = yield* readWarmStatus(canonical);
        if (
          existing !== undefined &&
          shouldReuseWarm(existing, stamp, Date.now())
        ) {
          const runtime =
            existing.instanceId === ""
              ? undefined
              : yield* workflowStatus(existing.instanceId);
          if (runtime !== "errored" && runtime !== "terminated") return existing;
        }

        const now = new Date().toISOString();
        const queued: CountyWarmStatusValue = {
          county,
          canonical,
          stamp,
          state: "queued",
          instanceId: "",
          requestedAt: now,
          updatedAt: now,
        };
        yield* writeWarmStatus(queued);
        let createError = "";
        const instance = yield* countyWarmWorkflow.create({ params: { county } }).pipe(
          Effect.map((created) => created as { readonly id: string } | undefined),
          Effect.catchCause((cause) => {
            createError = Cause.pretty(cause).split("\n")[0] ?? "could not queue county warm";
            return Effect.succeed(undefined);
          }),
        );
        if (instance === undefined) {
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
        if (current?.state !== "queued" || current.stamp !== stamp) {
          return current ?? queued;
        }
        const withInstance: CountyWarmStatusValue = {
          ...current,
          instanceId: instance.id,
          updatedAt: new Date().toISOString(),
        };
        yield* writeWarmStatus(withInstance);
        return withInstance;
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
          decoded.stamp === (yield* countyStamp(county)) &&
          Date.now() - Date.parse(decoded.at) < COMPILED_TTL_MS
        );
      }).pipe(Effect.catch(() => Effect.succeed(false)));

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
            };
          })
          .filter((match) => match.address !== "");
      });

    yield* Cloudflare.Workers.cron("0 * * * *", () =>
      Effect.gen(function* () {
        const outcomes = yield* runAll;
        const failed = outcomes.filter((o) => !o.ok);
        yield* outcomes.length === 0
          ? Effect.logError("cron sweep: no enabled sources; POST /seed to import the bundled list")
          : Effect.log(
              `cron sweep: ${outcomes.length - failed.length}/${outcomes.length} ok` +
                (failed.length > 0
                  ? `; failed: ${failed.map((o) => `${o.sourceId}@${o.stage}`).join(", ")}`
                  : ""),
            );

        // The sweep just changed every county's event stamp, so the caches it
        // invalidated are rebuilt here rather than by whoever texts first. A
        // county compiles once, on the cron's clock, instead of making a
        // person wait three minutes for their first reply.
        const busiest = yield* Effect.promise(async () => {
          const rows = await db().property.groupBy({
            by: ["jurisdiction"],
            _count: { key: true },
            orderBy: { _count: { key: "desc" } },
            take: 1,
          });
          return rows[0]?.jurisdiction ?? "";
        });
        if (busiest !== "") {
          const warming = yield* ensureCountyWarm(busiest);
          yield* Effect.log(`cron ${warming.state} county warm for ${busiest}`);
        }
      }),
    );

    const handleFetch = Effect.gen(function* () {
        const request = yield* HttpServerRequest.HttpServerRequest;
        const url = new URL(request.url, "http://worker");
        const origin = requestOrigin(request.url, request.headers, publicOrigin);

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
          const outcomes = yield* runAll;
          if (outcomes.length === 0) {
            return json(
              { ok: false, error: "no enabled sources; POST /seed to import the bundled list", outcomes },
              503,
            );
          }
          return json({ ok: outcomes.every((o) => o.ok), outcomes });
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

        if (url.pathname === "/sms" && request.method === "POST") {
          const bodyText = yield* request.text;
          const parsed = JSON.parse(bodyText) as {
            readonly from_number?: string;
            readonly to_number?: string;
            readonly number?: string;
            readonly content?: string;
            readonly message_handle?: string;
          };
          const phone = (parsed.from_number ?? "").replace(/[^+0-9]/g, "");
          const ourNumber = (parsed.to_number ?? parsed.number ?? "").replace(/[^+0-9]/g, "");
          const text = parsed.content ?? "";
          const messageHandle = parsed.message_handle?.trim() ?? "";
          const probe =
            request.headers["x-datadiver-probe"] ===
            Redacted.value(sendblueSecret);
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
            const onboarding = yield* thread.handleMessage({
              text,
              candidates: [],
              codexAccount: preflight.codexAccount || "reset",
            });
            const reply =
              `Reset complete. Your prior criteria, results and conversation were ` +
              `removed; your ChatGPT connection was kept.\n\n${onboarding.reply}`;
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
          if (snapshot.configured && !(yield* countyIsWarm(county))) {
            const warming = yield* ensureCountyWarm(county);
            const reply =
              warming.state === "error"
                ? `I could not prepare ${county.replace(/_/g, " ")} right now. ` +
                  `Nothing was guessed or partially scanned. Please try again shortly.`
                : `Collecting information on ${county.replace(/_/g, " ")} now - reading the ` +
                  `county's records and working out the signals for every property.\n\n` +
                  `The durable job is ${warming.state}; it will retry automatically if ` +
                  `anything interrupts it. Text me again in a few minutes.`;
            yield* Effect.tryPromise({
              try: () => sender.send({ to: phone, from: ourNumber, body: reply }),
              catch: (cause): Error =>
                cause instanceof Error ? cause : new Error(String(cause)),
            }).pipe(Effect.catch(() => Effect.void));
            return json({
              ok: warming.state !== "error",
              phone,
              reply,
              warming: warming.state,
            });
          }
          const candidates = snapshot.configured
            ? yield* countyMatches(county)
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
          const scouted = yield* scoutTurn(
            preflight.tenantId,
            text,
            input.candidates,
            snapshot,
            (kind) => {
              scoutDecision = kind;
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
              ...(scoutDecision === ""
                ? {}
                : { brainDecision: scoutDecision }),
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
