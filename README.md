# Data Diver

County records in, one schema out, in your terminal.

Every county publishes the same facts in a different dialect: different
markup, different APIs, different field names. Data Diver fetches a source,
classifies what kind of record it is, matches the source's own labels onto a
canonical schema you define in JSON, validates and extracts the records, and
keeps the mapping healed when the source changes shape. All C++20; the only
dependencies are libcurl and zlib. Nothing is mocked: every number printed is
a measurement from a real run against real bytes.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## The demo

```
./build/datadiver
```

drops you into the shell. Paste any URL - a government open-data endpoint, a
county HTML page, a CSV export, a local file - and the full pipeline prints
with its evidence:

```
diver> https://data.norfolk.gov/resource/7qie-z5gv.json?$limit=25

== Fetch =====================================   real bytes, measured
== Classify ==================================   model posterior over record types
== Match =====================================   each source label scored against the
                                                 schema: similarity x validated values
== Extract ===================================   canonical records, normalized
== Result ====================================   extraction rate, confidence, timings
```

Stage compute is typically 1-3 ms; the network is the only real cost. Paste
another URL, or:

- `watch URL [seconds]` - refetch on an interval. Unchanged bytes short-circuit
  on a hash in microseconds. When the page changes shape, you watch drift
  detection and the self-heal live: the old mapping's collapse, the relearned
  field assignments, and the recovered extraction rate. Watch a local file and
  edit it in another pane to drive this by hand.
- `schema` - the canonical fields currently loaded.
- `quit`.

One-shot equivalents: `datadiver align URL`, `datadiver watch URL --interval 900`.

## The schema is yours

`data/schema.json` defines the labels the engine fills - not code:

```json
{"name": "owner", "kind": "name", "role": "owner", "identity": true,
 "synonyms": ["owner name", "taxpayer", "defendant", "grantee", ...]}
```

`kind` picks the validator (id, name, address, money, date, status, text);
`synonyms` are the dialect lexicon that label matching scores against;
`identity` fields let records resolve to properties; `role` ties fields to
event building (parcel, address, owner, status, event_date, fallback_date,
amount) without fixing their names. Pass `--schema other.json` and the same
engine fills a completely different schema - the test suite proves it with a
business-license schema defined inline.

## How matching works

- Classification: multinomial naive Bayes over prefixed document tokens
  (column names, title, headings, body), trained on a labeled corpus in
  `data/corpus`. `datadiver train` retrains and measures leave-one-out
  accuracy, refusing to save below 0.85. Confidence shown is the model's
  posterior, never a constant.
- Label assignment: each source label is scored against each schema field:
  synonym similarity (exact slug, length-normalized token overlap,
  Jaro-Winkler) combined with the measured validator pass rate of the
  column's actual values, then a greedy one-to-one assignment. Fields with
  weak validators (free text) require strong label evidence on their own.
- Healing: a structure fingerprint plus an extraction-rate baseline; when a
  refetch collapses below 60% of baseline the healer rescores the new
  document's labels and proposes a repair, auto-accepted only above 75%
  confidence with most of the baseline recovered.

## Stateful ingestion

Beyond one-shot alignment, `datadiver ingest --all` runs configured sources
(`data/sources.json` ships eight real government endpoints across six
jurisdictions - Norfolk VA taxes and assessments, Cook County IL tax sales,
Chicago violations, New Orleans violations and sheriff sales, Seattle
permits, NYC property sales) into `var/`: content-hashed events resolve onto
jurisdiction-scoped properties (re-ingestion is idempotent), a deterministic
state machine folds events into a property lifecycle (NORMAL ->
TAX_DELINQUENT -> FORECLOSURE_FILED -> AUCTION_SCHEDULED -> SOLD_AT_AUCTION;
deed transfer resets), and per-source learned state (mapping, baseline,
operator overrides) drives drift healing across runs. Fetched bodies are
cached under `var/cache` so replays never re-hit public endpoints.

## Layout

| Layer | Modules | Job |
| --- | --- | --- |
| `core` | `core`, `json`, `metrics` | strings, time, files, hashing, JSON, OS metrics |
| `parse` | `html`, `csv`, `pdf`, `document` | in-house tolerant parsers; records of (label, value, provenance) |
| `net` | `fetch` | libcurl http(s) plus local files, measured |
| `ml` | `features`, `model`, `classify` | feature bags, naive Bayes, source classifier |
| `engine` | `schema`, `entity`, `events`, `store`, `heal`, `pipeline` | JSON-defined schema, matching, resolution, lifecycle, state, healing |
| `app` | `main`, `render` | the CLI, the shell, terminal presentation |

## Working rules

See `CLAUDE.md` (symlinked as `AGENTS.md`) and `docs/cpp-guide.md`. `ctest`
must pass before a commit.
