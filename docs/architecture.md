# Data Diver architecture (v2: engine + console)

Two processes, one contract:

```
C++ engine (this repo root)          Next.js console (console/)
  fetch/classify/match/extract        UI + persistence
  HTTP JSON API on 127.0.0.1:8080     Postgres via Prisma (docker, port 5455)
  owns var/ working memory            calls the engine API, mirrors results
  never touches the database          into Postgres; the UI reads Postgres
```

Rules that hold everywhere:

1. Nothing mocked. Every county, source, record, number and confidence on the
   console comes from a real engine run against real bytes. Fixture files under
   `data/fixtures/` exist for the C++ test suite only and are never seeded into
   the product. Shipped seeds are real government endpoints.
2. The C++ engine never talks to Postgres. Only Next.js API routes do, only
   through Prisma, with zero hand-written SQL (use `prisma db push`; no .sql
   files in the repo).
3. Confidence values come from the model or measured validators, never
   constants. Timings come from clocks, bytes from the fetcher, memory from
   the OS.
4. Agents working from this document do not run `git commit`; the operator
   reviews and commits.

## Employee flow (the product is this flow)

An operator onboarding "Travis County TX delinquent taxes" must be able to:

1. Add the county and paste the source URL (county-first form on the home page).
2. Run it; watch fetch/classify/match/extract with real measurements.
3. Review the learned mapping; fix any field with an override (pick a source
   label for a canonical field, or force-unmap); overrides persist and win over
   inference and healing on every later run.
4. See drift repairs; approve or reject the ones below the auto-accept bar.
5. Manage the source: edit the URL (county migrated their portal; the next run
   detects the shape change and heals), disable it, delete it.
6. Leave the engine to re-ingest on a timer and trust dedup + healing to keep
   data continuous.

## Engine HTTP API (port 8080)

All responses JSON. Existing endpoints (unchanged shapes):

- `GET /api/overview` — `{engine:{rss_bytes,peak_rss_bytes,cpu_ms,http_transport,now}, model:{trained_at,leave_one_out_accuracy,examples}, totals:{sources,runs,events,properties,repairs}, per_source:[{id,name,runs,ok_runs,bytes_total,avg_total_ms,avg_fetch_ms,last_run:RunRecord|null}]}`
- `GET /api/counties` — `[{jurisdiction,slug,sources,ok_sources,properties,corroborated,events,repairs,avg_extraction,last_run_at}]`
- `GET /api/model` — `{trained_at,leave_one_out_accuracy,examples,vocabulary,kind,classes:[{name,documents,tokens,top_tokens:[{token,count,lift}]}]}`
- `GET /api/sources` — `[{id,name,url,jurisdiction,added_at,enabled,demo,classification,has_mapping,mapping_confidence,baseline_rate,good_runs,mapping,last_run}]`
- `GET /api/schema?source=ID` — `{source_id,name,jurisdiction,baseline_rate,good_runs,snapshot}` where snapshot is `{at,run_id,format,container,fingerprint,labels:[...],raw_sample:[{label,value}],classification:{label,confidence,distribution:[{label,probability}]},mapping:{confidence,fields:[{field,source_label,label_similarity,value_pass_rate,confidence}]},field_rates:{field:rate},records:[{<field>:<value>,_completeness}]}` or null before the first run
- `GET /api/runs?limit=&source=` — `[RunRecord]` newest first. RunRecord: `{id,source_id,started_at,ok,error,stage,http_status,bytes,fetch_ms,parse_ms,classify_ms,map_ms,total_ms,format,classification,class_confidence,records_extracted,events_new,extraction_rate,mapping_confidence,baseline_rate,drift_detected,repair_attempted,repair_accepted,structure_fingerprint,rss_bytes,cpu_ms}`
- `GET /api/records?source=ID` — snapshot records array
- `GET /api/properties` — `[{key,state,events,owner,address,parcel_id,last_event_date,sources}]`
- `GET /api/property?key=K` — `{key,state,transitions:[{state,event_id,event_date}],events:[Event]}` Event: `{id,property_key,kind,event_date,recorded_at,source_id,run_id,amount,confidence,details:{...}}`
- `GET /api/repairs?source=` — `[{id,source_id,at,reason,before_mapping,after_mapping,before_rate,after_rate,confidence,accepted,resolution,changes:[...]}]`
- `POST /api/sources` body `{name,url,jurisdiction}` — creates, returns source
- `POST /api/run?source=ID`, `POST /api/run_all`
- `POST /api/benchmark?rounds=N` — replays the last fetched bytes of every
  enabled source through the full pipeline (see cache below); returns
  `{rounds,runs,ok_runs,records_processed,events_new,bytes_processed,total_ms,runs_per_sec,records_per_sec,cpu_ms,rss_before_bytes,rss_after_bytes}`

New endpoints (engine work):

- `POST /api/sources/update` body `{id, name?, url?, jurisdiction?, enabled?}` —
  partial update, persisted; returns the source. Changing the url is the real
  "portal migrated" flow: nothing else happens until the next run, where drift
  detection and healing take over.
- `POST /api/sources/delete` body `{id}` — removes the source, its learned
  state and snapshot. Historical runs/events/repairs remain.
- `POST /api/mapping` body `{source, overrides: {"owner": "taxpayer_name", "status": ""}}` —
  saves operator overrides on SourceState (field -> source label; empty string
  = force-unmap), then runs the source once and returns the RunRecord.
  Overrides are applied after inference/healing on every run: an overridden
  field maps to its label when present (similarity from the lexicon, pass rate
  measured on the document), and a force-unmapped field is removed. Overrides
  survive and constrain heal proposals.
- `POST /api/repairs/resolve` body `{id, approve}` — resolves a pending
  repair. Approve applies the repair's after_mapping to the source state
  (baseline restarts); reject keeps the old mapping. Repairs carry
  `resolution`: `auto|pending|approved|rejected` (legacy records without the
  field: accepted -> auto, else pending).
- `POST /api/train` — retrains the classifier from `data/corpus` in-process:
  leave-one-out over every document with per-example predictions, then fits on
  all and hot-swaps the live classifier (serialized with runs). Saves the model
  and `var/training_report.json`. Returns
  `{at,duration_ms,examples,classes,accuracy,per_class:[{name,examples,correct}],confusion:[{actual,predicted,count}]}`
  (confusion only lists nonzero off-diagonal pairs plus diagonal).
- `GET /api/train/report` — last saved report or 404.
- `GET /api/export` — one call for console sync:
  `{sources:[...as /api/sources], runs:[latest 500], events:[all], repairs:[all], properties:[{key,jurisdiction_slug,state,transitions,owner,address,parcel_id,last_event_date,sources:[source_id,...]}], counties:[...as /api/counties]}`

Removed: the static UI (`GET /` now returns a JSON service descriptor), the
`web/` directory, and the `/api/demo/*` endpoints (mock-flavored; the heal
path is exercised by the C++ test suite and by real drift).

Fetch cache: on every successful fetch the engine writes the body to
`var/cache/<source_id>` with its content type. The benchmark replays cached
bytes through the full pipeline so scale tests never hammer public endpoints;
`GET /api/overview` gains `engine.benchmark_replays_cache: true`.

Auto-refresh: `datadiver serve --refresh SECONDS` re-runs all enabled sources
on that interval (0 = off, default). Overview exposes
`engine.auto_refresh_seconds`.

## Console (console/)

Next.js (App Router, TypeScript), Tailwind, shadcn/ui installed via
`npx shadcn@latest init` + `add` (never hand-written component files),
TanStack Query for all client data, TanStack Virtual for long tables
(properties, records). Light mode only for now: modern, calm, slightly
rounded (radius ~0.5rem), teal primary continuing the engine's identity.
`sonner` for toasts. Engine base URL from `ENGINE_URL` env
(default `http://127.0.0.1:8080`).

Postgres 16 via root `docker-compose.yml`, host port 5455, db/user/password
`datadiver`. `console/.env`: `DATABASE_URL=postgresql://datadiver:datadiver@localhost:5455/datadiver`,
`ENGINE_URL=http://127.0.0.1:8080`. Schema applied with `prisma db push`.

### Prisma models (console/prisma/schema.prisma)

```prisma
model County {
  id         String     @id @default(cuid())
  name       String     @unique
  slug       String     @unique
  sources    Source[]
  properties Property[]
  createdAt  DateTime   @default(now())
}

model Source {
  id                 String    @id            // engine id
  name               String
  url                String
  enabled            Boolean   @default(true)
  county             County    @relation(fields: [countyId], references: [id])
  countyId           String
  classification     String?
  mappingConfidence  Float?
  baselineRate       Float?
  goodRuns           Int       @default(0)
  mapping            Json?
  labels             Json?
  rawSample          Json?
  classificationDist Json?
  fieldRates         Json?
  lastRunAt          DateTime?
  runs               Run[]
  repairs            Repair[]
  records            CanonicalRecord[]
  events             Event[]
  createdAt          DateTime  @default(now())
  updatedAt          DateTime  @updatedAt
}

model Run {
  id               String   @id              // engine run id
  source           Source   @relation(fields: [sourceId], references: [id], onDelete: Cascade)
  sourceId         String
  startedAt        DateTime
  ok               Boolean
  stage            String
  error            String?
  httpStatus       Int
  bytes            Int
  fetchMs          Float
  parseMs          Float
  classifyMs       Float
  mapMs            Float
  totalMs          Float
  format           String
  classification   String
  classConfidence  Float
  recordsExtracted Int
  eventsNew        Int
  extractionRate   Float
  mappingConfidence Float
  driftDetected    Boolean
  repairAccepted   Boolean
  rssBytes         Float
  cpuMs            Float
  @@index([sourceId, startedAt])
}

model Property {
  key           String   @id                // engine property key
  county        County   @relation(fields: [countyId], references: [id])
  countyId      String
  parcelId      String?
  owner         String?
  address       String?
  state         String
  transitions   Json?
  sourcesCount  Int      @default(1)
  lastEventDate String?
  events        Event[]
  updatedAt     DateTime @updatedAt
  @@index([countyId, state])
}

model Event {
  id          String   @id                  // engine content-hash id
  property    Property @relation(fields: [propertyKey], references: [key], onDelete: Cascade)
  propertyKey String
  source      Source?  @relation(fields: [sourceId], references: [id], onDelete: SetNull)
  sourceId    String?
  kind        String
  eventDate   String?
  recordedAt  DateTime
  runId       String
  amount      Float    @default(0)
  confidence  Float    @default(0)
  details     Json
  @@index([propertyKey])
}

model Repair {
  id            String   @id
  source        Source   @relation(fields: [sourceId], references: [id], onDelete: Cascade)
  sourceId      String
  at            DateTime
  reason        String
  beforeMapping Json?
  afterMapping  Json?
  beforeRate    Float
  afterRate     Float
  confidence    Float
  resolution    String   // auto | pending | approved | rejected
  changes       Json
}

model CanonicalRecord {
  id           String  @id @default(cuid())
  source       Source  @relation(fields: [sourceId], references: [id], onDelete: Cascade)
  sourceId     String
  runId        String
  position     Int
  values       Json
  completeness Float
  @@index([sourceId])
}

model TrainingReport {
  id         String   @id @default(cuid())
  at         DateTime
  durationMs Float
  examples   Int
  classes    Int
  accuracy   Float
  perClass   Json
  confusion  Json
}

model EngineSample {
  id          String   @id @default(cuid())
  at          DateTime @default(now())
  rssBytes    Float
  peakRss     Float
  cpuMs       Float
  totalRuns   Int
  totalEvents Int
}
```

### Sync layer

`console/src/lib/engine.ts`: typed fetch client for every engine endpoint.
`console/src/lib/sync.ts`: `syncFromEngine(prisma)` pulls `GET /api/export`
and idempotently upserts counties, sources (+ per-source `GET /api/schema`
snapshot fields and CanonicalRecord rows replaced per latest run), runs,
events, properties, repairs. Called by API routes after every mutating engine
call and by `POST /api/sync`.

### Next API routes (all engine calls live server-side)

- `GET  /api/counties`, `GET /api/counties/[slug]` — from Postgres
- `POST /api/sources` `{county,name,url}` — engine create + sync
- `POST /api/sources/[id]/run` — engine run + sync, returns run
- `PATCH /api/sources/[id]` — engine update + sync
- `DELETE /api/sources/[id]` — engine delete + prisma delete
- `POST /api/sources/[id]/mapping` `{overrides}` — engine mapping + sync
- `GET  /api/sources/[id]` — source + latest snapshot fields + runs + repairs
- `POST /api/run-all` — engine run_all + sync
- `POST /api/repairs/[id]/resolve` `{approve}` — engine resolve + sync
- `GET  /api/properties?county=`, `GET /api/properties/[key]`
- `GET  /api/model` — proxy engine (live introspection, not persisted)
- `POST /api/train` — engine train + persist TrainingReport + sync
- `GET  /api/training-reports`
- `GET  /api/overview` — engine overview (live) + Postgres totals
- `POST /api/benchmark?rounds=`
- `POST /api/sync`

### Pages

- `/` counties dashboard: county cards (sources, properties, corroborated,
  extraction, last ingest), add county+source form, ingest-all, live engine
  strip, "why trust it" links.
- `/counties/[slug]` sources side by side + county properties (virtualized) +
  county repairs.
- `/sources/[id]` the onboarding workbench: stage walk (fetch, classify with
  posterior, match, extract), mapping editor with per-field override selects
  and unmap, unmatched labels, run history, repair queue with approve/reject,
  manage (edit url/name, enable/disable, delete).
- `/properties` all + `/properties/[key]` event history + lifecycle chain.
- `/model` live model internals + training: Train button runs real LOO and
  renders per-class results and the confusion matrix from the response.
- `/performance` per-stage timings per source + cache-replay benchmark.

Every source shows a plain link to the government URL it ingests, because the
strongest "nothing is mocked" proof is opening the source in a second tab.
