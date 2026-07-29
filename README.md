# Data Diver

County records in, one canonical property schema out.

Two halves of one system. A C++ engine that turns inconsistent county
records into resolved, provenance-tracked property signals, and a Cloudflare
service that runs that same engine as WebAssembly on a schedule and answers
questions about it over SMS. The engine is the product; the CLI and the
worker are two hosts for it.

Every county publishes the same facts in a different dialect: different
markup, different APIs, different field names. Data Diver fetches a source,
classifies what kind of record it is, matches the source's own labels onto a
canonical schema you define in JSON, validates and extracts the records,
resolves them to properties, and heals the mapping when the source changes
shape. All C++20; the only dependencies are libcurl and zlib. Nothing is
mocked: every number printed is a measurement from a real run against real
bytes.

## The two halves

| Path | What it is |
| --- | --- |
| `include/dd`, `src` | the engine: parse, ml, schema matching, entity resolution, events, compile |
| `src/app` | the CLI host, for development and demos |
| `wasm/api.cpp` | the WASM seam: JSON in, JSON out, no store or sockets |
| `cloud` | the Cloudflare host: fetching, storage, cron, SMS conversation |

The engine never opens a socket or touches a database in the cloud build.
The host fetches bytes and owns storage; the engine classifies, matches,
extracts, resolves and compiles. Everything crosses one boundary as JSON,
so the WASM build links no filesystem and no libcurl.

## Build and run

```
make            # configure, build, run the tests
make run        # build and open the shell
make renderer   # install the headless browser used for JS-only portals
make run-with-renderer   # shell with rendering enabled
make wasm       # build the WebAssembly engine for the worker
```

For the cloud service:

```
cd cloud
bun install
bun alchemy dev      # local workerd on http://localhost:1337
bun alchemy deploy   # provision R2, the worker and its cron
```

## The demo, in order

Inside the shell:

```
run all              # ingest all 16 real sources concurrently (Socrata APIs
                     # and county HTML), about 1.4 s wall clock for 7 s of work.
                     # enrichment sources run in a second wave: {parcels} in
                     # their URL expands to the parcels the first wave found,
                     # so the assessor roll is fetched for exactly the
                     # properties the violations feed surfaced
counties             # the store rolled up by county
county norfolk       # properties ranked by distress, merged across id spaces,
                     # conflicting fields resolved by measured source trust

export norfolk       # the compiled county as canonical JSON with per-field
                     # source + as-of provenance: the payload an API would serve

fresh                # how current each source's records are, not just whether
                     # the fetch succeeded
bench                # score vs the hand-verified answer key:
                     # 15/16 classification, 0.95 mapping F1, ~7 ms/document
catalog              # discover county sources nationwide (140 datasets across
                     # 96 jurisdictions); 'catalog add' adds them as sources
model                # both models' status
map SOURCE_ID        # what matched what, with sample values
review SOURCE_ID     # y/n on uncertain matches; answers persist as overrides
```

Paste any URL to align it live — try a hard one:

```
https://www.hctax.net/Property/listings/taxsalelisting
```

3.8 MB of bespoke Harris County markup: 272 records, address and zip at 93%,
and it shows honestly what it could not map. `watch FILE 2` on a local copy
of a fixture, edit it in another pane, and you see drift detection and the
self-heal live. `help` lists everything; one-shot equivalents exist as
`datadiver <command>`.

Source shape is detected, not configured. JSON, CSV and static HTML are
parsed directly; a page that is only a framework shell (an app mount point,
a hydration payload, or far more script than text) is re-fetched through a
headless browser when one is available, and `align` reports which path it
took. Socrata's React grid yields no records over a static fetch and 100
records through the renderer. The browser only ever supplies bytes:
parsing, classification, matching and the models stay in C++, measured
(`align` prints parse throughput and resident memory).

## The schema is configuration

`data/schema.json` defines the labels the engine fills - not code. Each field
has a validator kind (id/name/address/money/date/status/text), an engine role
(parcel/owner/address/amount/event_date/...), an identity flag for property
resolution, and a synonym lexicon. Point `--schema` at a different file and
the same engine fills a different domain; the test suite proves it with a
business-license schema.

## The two models (both in-repo, no ML dependencies)

- **Document classifier**: multinomial naive Bayes with tunable Lidstone
  smoothing, trained on 115 real county datasets pulled from live portals
  by `harvest docs`. Leave-one-out is 83%, and the confusions are mostly
  genuine overlap (a tax sale list is both a delinquency and an auction)
  plus label noise from the search queries that gathered it, which is why
  `train` names the files that disagree with their folder.
- **Column transformer**: a byte-level transformer encoder written from
  scratch (pre-LN multi-head attention, hand-derived backprop verified
  against finite differences in the tests). It reads a column's name and
  sample values and predicts the canonical field, so headers no lexicon
  knows still map. `harvest` builds its training set from live government
  portals via the Socrata discovery API (currently 2,571 real columns from
  75 portals; exact lexicon hits as labels, near-misses masked); `train
  columns` validates on held-out portals - 87% on portals the model never
  saw. Its verdict only counts in the matcher when decisive (argmax >= 0.7)
  and never bypasses the weak-validator floor.

## Freshness and editions

A fetch that succeeds with month-old rows is the failure that matters, so
the engine measures two ages: when a source was last fetched, and how recent
the newest record it returned is. `fresh` reports both and flags sources
behind the threshold, which is how you learn that a county quietly stopped
publishing rather than discovering it in the output weeks later.

Once the mapping knows which column carries the event date, the next fetch
asks for that column in descending order, so refetches return the newest
records instead of an arbitrary page. The source teaches the engine the name
of its date column and the engine then uses it: Cincinnati went from
returning 2023 records to 50 hour old ones on the run after its mapping was
learned.

Some sources are editions rather than feeds. Every row in an assessment roll
is equally current, so no record date can tell this year's roll from last
year's. Sources therefore carry an `as_of` edition, the compile layer prefers
the newest edition of a field while keeping the earlier one, and the
difference becomes a signal: `assessed_value_change` in the export and a
year over year column in the county view.

## The cloud service

The worker fetches every configured source on an hourly cron, hands each
body to the WASM engine, and writes the raw bytes, the extracted events and
the run record to R2. `GET /county/:name` compiles the stored events into
the canonical payload: per-field values with their source, confidence and
as-of edition, the conflicts that were resolved and what lost, the distress
signals, and the full event history.

`POST /sms` is the conversational surface. Each phone number addresses its
own Durable Object, which is the tenancy boundary: one object, one thread,
its own storage, so isolation is structural rather than a filter on a
query. The thread keeps a rolling window of turns and folds older ones into
a summary, and because every decision is extracted into structured state
before compaction, the user never needs to start a new conversation.

Acquisition criteria are a versioned decision graph of condition, action
and approval nodes, not a bag of settings. Editing a criterion compiles a
new tree version; every scan evaluates each property by walking the graph
and records the per-node trace to Postgres, so "why did this match" is
answered from the trace of the exact tree version that ran, not from the
current settings.

Texting CONNECT links a ChatGPT account for Codex access: the worker
issues a single-use link that runs the PKCE flow against auth.openai.com,
and the resulting tokens are envelope-encrypted — a fresh AES-256-GCM data
key per credential, wrapped with AES-KW under a worker-held master key —
before they reach the credentials table. `GET /connect/status` proves a
stored credential is alive by refreshing it and re-sealing the result.

## How matching works

Each source label is scored against each schema field: name evidence
(synonym lexicon similarity or a decisive transformer verdict, whichever is
stronger) combined with the measured validator pass rate over the column's
actual values, then greedy one-to-one assignment. Composite values coerce
("Account No: 111-22-001" extracts "111-22-001", flagged reformatted).
Drift: an EWMA baseline over extraction rate; a collapse triggers a re-scored
repair, auto-accepted only above 75% confidence with the baseline recovered.

## Proving it

1. `ctest`: 109 behaviour tests, including the transformer gradient check
   and a concurrency test that proves parallel ingestion loses nothing.
2. `train` / `train columns`: cross-validated accuracy per class, plus the
   corpus files whose content disagrees with their folder, so weak labels
   are auditable rather than assumed.
3. `bench`: the engine vs `data/golden/golden.json`, a hand-verified answer
   key written by reading the raw columns - it records the engine's own
   mistakes. Wrong mappings on required fields count double (false positive
   and false negative); incomplete runs fail. Currently 15/16 classification,
   0.95 F1, ~7 ms per document, $0 marginal cost.

An LLM API could attempt the same mapping; this engine does it
deterministically, explainably, offline, in milliseconds, at zero marginal
cost - and the benchmark keeps that claim measured instead of asserted.
