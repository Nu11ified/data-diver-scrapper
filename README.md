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
- `run all` - ingest every configured source into the store, then:
- `counties` - the store rolled up by county: sources, ingestion health,
  properties, events.
- `county NAME` - that county's properties as one table, filled from the
  schema's identity and role fields (parcel, owner, address, lifecycle state,
  amount, last event).
- `map SOURCE_ID` - the learned mapping with sample values from the cached
  bytes, the source labels nothing matched, and any operator overrides.
- `review SOURCE_ID` - the interactive pass: uncertain matches and held-back
  near-misses each show real sample values and ask y/n. Answers become
  operator overrides (pin or force-unmap), the source re-runs immediately.
- `add "County" "Name" URL` - onboard a new source from inside the shell.
- `model` - classifier status: algorithm, alpha, corpus size, leave-one-out
  accuracy, and each class's most discriminative vocabulary by lift.
- `bench [llm]` - score the engine against the hand-verified answer key
  (see "Proving it"), optionally head-to-head with an LLM baseline.
- `train [ALPHA|sweep]` - retrain at one smoothing alpha or sweep a grid,
  with per-class validation results and confusion pairs; the best model is
  saved only above 0.85 accuracy and hot-swapped into the running shell.
- `schema` - the canonical fields currently loaded.
- `quit`.

One-shot equivalents: `datadiver align URL`, `datadiver watch URL
--interval 900`, `datadiver counties`, `datadiver county NAME`,
`datadiver map ID`, `datadiver review ID`, `datadiver model`,
`datadiver train [--alpha A | --sweep]`.

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
- Coercion: when a column's raw values fail a decisive validator (id, money,
  date), the engine looks for a validating token embedded in the composite
  value ("Account No: 111-22-001" yields "111-22-001") and, if most of the
  column extracts that way, maps it with a `reformatted` flag that survives
  into the stored mapping and the `map` view.
- Review: candidates that scored well but lost the assignment (or cleared
  the floor without being accepted) are kept with their evidence; `review`
  turns them into y/n questions backed by sample values, and the answers
  are operator overrides applied before every future run.
- Healing: a structure fingerprint plus an extraction-rate baseline; when a
  refetch collapses below 60% of baseline the healer rescores the new
  document's labels and proposes a repair, auto-accepted only above 75%
  confidence with most of the baseline recovered.

## Stateful ingestion

Beyond one-shot alignment, `datadiver ingest --all` (or `run all` in the
shell) runs configured sources (`data/sources.json` ships fourteen real
government endpoints across eleven jurisdictions - Norfolk VA taxes,
assessments and code cases, Cook County IL tax sales, Chicago violations,
New Orleans violations and sheriff sales, Seattle permits, NYC property
sales, Kansas City MO and Cincinnati OH violations, Los Angeles CA permits,
Howard County MD and Prince George's County MD permits) into `var/`: content-hashed events resolve onto
jurisdiction-scoped properties (re-ingestion is idempotent), a deterministic
state machine folds events into a property lifecycle (NORMAL ->
TAX_DELINQUENT -> FORECLOSURE_FILED -> AUCTION_SCHEDULED -> SOLD_AT_AUCTION;
deed transfer resets), and per-source learned state (mapping, baseline,
operator overrides) drives drift healing across runs. Fetched bodies are
cached under `var/cache` so replays never re-hit public endpoints.

## Proving it

Claims are cheap, so the validity story is layered and every layer is
measured:

1. Behaviour tests: `ctest` covers parsing, validation, matching, coercion,
   healing, lifecycle and the store against fixtures with known answers.
2. Model validation: `train` reports leave-one-out cross-validation accuracy
   with per-class results and confusion pairs, and refuses to save a model
   below 0.85.
3. The answer key: `data/golden/golden.json` is a hand-verified mapping and
   classification key for every shipped source, written by reading the actual
   column values - it records mistakes the engine currently makes, not the
   engine's own output. `bench` scores the engine against it from cached
   bytes: classification accuracy, mapping precision/recall/F1 per source,
   and measured compute. The engine currently scores 14/14 classification
   and 0.96 mapping F1 in about 4 ms per document at zero marginal cost.
4. The baseline: `bench llm` sends the same documents, the same schema and
   the same answer key through any OpenAI-compatible endpoint
   (`DD_LLM_ENDPOINT`, `DD_LLM_KEY`, `DD_LLM_MODEL`, optional
   `DD_LLM_PRICE_IN`/`DD_LLM_PRICE_OUT` dollars per 1M tokens) and prints
   both scoreboards side by side with latency and token cost measured. The
   comparison is honest by construction: shared scorer, shared key, and the
   baseline's answers are parsed strictly - prose is a failure, not a guess.

The point is not that a frontier model cannot match the mapping accuracy; it
is that this engine does it deterministically, explainably (`map`, `review`),
offline, in milliseconds, at zero marginal cost - and the benchmark makes
that trade measurable instead of asserted.

## Layout

| Layer | Modules | Job |
| --- | --- | --- |
| `core` | `core`, `json`, `metrics` | strings, time, files, hashing, JSON, OS metrics |
| `parse` | `html`, `csv`, `pdf`, `document` | in-house tolerant parsers; records of (label, value, provenance) |
| `net` | `fetch` | libcurl http(s) plus local files, measured |
| `ml` | `features`, `model`, `classify` | feature bags, naive Bayes, source classifier |
| `engine` | `schema`, `entity`, `events`, `store`, `heal`, `pipeline` | JSON-defined schema, matching, resolution, lifecycle, state, healing |
| `app` | `main`, `commands`, `render` | the CLI, the shell, county/review/model views, terminal presentation |

## Working rules

See `CLAUDE.md` (symlinked as `AGENTS.md`) and `docs/cpp-guide.md`. `ctest`
must pass before a commit.
