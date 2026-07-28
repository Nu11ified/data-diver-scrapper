# Data Diver

Data Diver is a C++ public-record ingestion engine. It classifies county source
documents, maps inconsistent schemas onto one canonical property-event schema,
resolves records to properties, and maintains an evidence-backed property
lifecycle. When a source changes shape, it detects the drift and attempts to
repair the extraction mapping automatically.

It is not a demo. Every path reachable from the operator console runs the real
pipeline against real bytes, and every number shown is a measurement taken
during a run.

## Build and run

Dependencies: a C++20 compiler, CMake 3.20+, libcurl, zlib. Nothing else.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/datadiver serve
```

The engine serves a JSON API on http://127.0.0.1:8080; `GET /` returns a
service descriptor listing every endpoint. The Next.js console under
`console/` is the UI: it calls this API and mirrors results into Postgres
(see `docs/architecture.md` for the full contract). Add a source with any
http(s) URL or a local file path, run it, review the learned mapping, pin or
unmap fields with operator overrides (`POST /api/mapping`), and approve or
reject queued drift repairs (`POST /api/repairs/resolve`). Pass
`--refresh SECONDS` to `serve` and the engine re-ingests every enabled source
on that interval.

Every successful fetch caches its body verbatim at `var/cache/<source_id>`
with a JSON sidecar `<source_id>.meta` carrying the content type, fetch time
and byte count. `POST /api/benchmark` replays those cached bytes through the
full pipeline, so scale tests never hammer public endpoints; sources without
a cache yet are counted in the response's `skipped` field.

Millbrook County demonstrates cross-referencing: three independent sources
(tax roll, assessment roll, code enforcement) resolve onto the same six
parcels, and each property page shows the corroborating events together.

CLI equivalents:

```
./build/datadiver sources          # list configured sites
./build/datadiver ingest --all     # run every site once
./build/datadiver ingest SOURCE_ID # run one site
./build/dd_train                   # retrain the source classifier from data/corpus
```

## Watching it self-heal

The Millbrook tax roll's bytes live at `var/local/millbrook_roll.html`, seeded
on first start, so they can change the way a real county site's bytes change.
From the CLI:

```
cp data/fixtures/millbrook_tax_v2.html var/local/millbrook_roll.html
./build/datadiver ingest millbrook_tax
```

The v2 page renames every label (`Owner Name` becomes a `data-field="taxpayer"`
attribute, the table becomes repeated blocks) and adds one newly delinquent
parcel. The engine detects that its stored mapping collapsed, searches the new
structure for a replacement, validates the values it extracts, auto-accepts the
repair above the confidence bar (below the bar it queues as `pending` for a
human to approve or reject), and records the whole thing with its before and
after. The run reports exactly one new event: the new parcel. The Mapping
repairs section of the console shows the field-by-field diff.

## How it works

```
fetch (libcurl / local files; bytes and wall time measured)
  -> detect format (bytes win over the Content-Type header)
  -> extract records of (label, value, provenance) cells
       html tables, repeated labelled blocks, data-* attributes,
       json record arrays, csv, pdf text, aligned text reports
  -> classify source type (naive Bayes over document features)
  -> map dialect labels onto the canonical schema
       synonym similarity x measured validator pass rate = confidence
  -> detect drift against the learned extraction-rate baseline
       and propose, validate, and score a repair when it hits
  -> resolve records to jurisdiction-scoped properties
  -> append content-hashed events (re-ingestion is idempotent)
  -> reduce events into a deterministic lifecycle state machine
       NORMAL -> TAX_DELINQUENT -> FORECLOSURE_FILED ->
       AUCTION_SCHEDULED -> SOLD_AT_AUCTION, deed transfer resets
```

Layout (`include/dd/<layer>`, `src/<layer>`):

| Layer | Modules | Job |
| --- | --- | --- |
| `core` | `core`, `json`, `metrics` | strings, time, files, hashing, JSON, OS metrics |
| `parse` | `html`, `csv`, `pdf`, `document` | in-house parsers and the unified record model with provenance |
| `net` | `fetch`, `server` | libcurl transport plus local files; the HTTP/1.1 JSON API |
| `ml` | `features`, `model`, `classify` | feature bags, multinomial naive Bayes, source classifier |
| `engine` | `schema`, `entity`, `events`, `store`, `heal`, `pipeline` | canonical mapping, property resolution, lifecycle, state, drift healing, orchestration |
| `app` | `main`, `train_main` | the `datadiver` and `dd_train` binaries |

## The model

`data/model/source_classifier.json` is trained by `dd_train` from
`data/corpus`: 40 curated county-style documents across 8 record types and 5
dialects each. Training measures leave-one-out accuracy and refuses to write a
model under 0.85 (the shipped model measures 1.000 on that corpus).
`POST /api/train` does the same in-process — per-example LOO predictions feed
the per-class and confusion sections of the returned report, the live
classifier hot-swaps, and the report persists at `var/training_report.json`
(served by `GET /api/train/report`). Classifier
confidence shown in the UI is the model's posterior for the winning class;
mapping confidence combines label similarity with measured validator pass
rates. No confidence anywhere is a constant.

## State

Everything the engine learns and records lives under `var/` (gitignored):
`sources.json`, per-source learned state (mapping, baseline, operator
overrides) in `state/`, append-only `runs.jsonl`, `events.jsonl`,
`repairs.jsonl`, the latest canonical records per source in `records/`,
cached fetch bodies in `cache/`, the last training report, and seeded working
copies in `local/`. Deleting a source removes its learned state, snapshot and
cache but keeps its historical runs, events and repairs. Delete `var/` to
start fresh.

## Working rules

See `CLAUDE.md` (symlinked as `AGENTS.md`) for the project rules and
`docs/cpp-guide.md` for the C++ guidelines the code follows. `ctest` must pass
before a commit.
