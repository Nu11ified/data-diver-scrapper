# Data Diver

County records in, one schema out, in your terminal.

Every county publishes the same facts in a different dialect: different
markup, different APIs, different field names. Data Diver fetches a source,
classifies what kind of record it is, matches the source's own labels onto a
canonical schema you define in JSON, validates and extracts the records,
resolves them to properties, and heals the mapping when the source changes
shape. All C++20; the only dependencies are libcurl and zlib. Nothing is
mocked: every number printed is a measurement from a real run against real
bytes.

## Build and run

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/datadiver
```

## The demo, in order

Inside the shell:

```
run all              # ingest all 16 real sources: Socrata APIs + county HTML.
                     # the last one is the enrichment source: {parcels} in its
                     # URL expands to the parcels already tracked, so the
                     # assessor roll is fetched for the exact properties the
                     # violations feed surfaced - those properties end up with
                     # events from two sources (violation + owner/assessed value)
counties             # the store rolled up by county
county norfolk       # one county's properties, filled per the schema

export norfolk       # the compiled county as canonical JSON with per-field
                     # source + as-of provenance: the payload an API would serve

bench                # score vs the hand-verified answer key:
                     # 15/15 classification, 0.94 mapping F1, ~8 ms/document
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

## The schema is configuration

`data/schema.json` defines the labels the engine fills - not code. Each field
has a validator kind (id/name/address/money/date/status/text), an engine role
(parcel/owner/address/amount/event_date/...), an identity flag for property
resolution, and a synonym lexicon. Point `--schema` at a different file and
the same engine fills a different domain; the test suite proves it with a
business-license schema.

## The two models (both in-repo, no ML dependencies)

- **Document classifier**: multinomial naive Bayes with tunable Lidstone
  smoothing, leave-one-out validated (`train`, `train sweep`).
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

## How matching works

Each source label is scored against each schema field: name evidence
(synonym lexicon similarity or a decisive transformer verdict, whichever is
stronger) combined with the measured validator pass rate over the column's
actual values, then greedy one-to-one assignment. Composite values coerce
("Account No: 111-22-001" extracts "111-22-001", flagged reformatted).
Drift: an EWMA baseline over extraction rate; a collapse triggers a re-scored
repair, auto-accepted only above 75% confidence with the baseline recovered.

## Proving it

1. `ctest`: 104 behaviour tests, including the transformer gradient check.
2. `train` / `train columns`: cross-validated model accuracy, per-class.
3. `bench`: the engine vs `data/golden/golden.json`, a hand-verified answer
   key written by reading the raw columns - it records the engine's own
   mistakes. Wrong mappings on required fields count double (false positive
   and false negative); incomplete runs fail. Currently 15/15 classification,
   0.94 F1, ~8 ms per document, $0 marginal cost.

An LLM API could attempt the same mapping; this engine does it
deterministically, explainably, offline, in milliseconds, at zero marginal
cost - and the benchmark keeps that claim measured instead of asserted.

## Layout

| Layer | Job |
| --- | --- |
| `core` | strings, time, files, hashing, JSON, OS metrics |
| `parse` | tolerant HTML/CSV/PDF parsers to (label, value) records |
| `net` | libcurl fetch, measured |
| `ml` | features, naive Bayes, column transformer, harvest corpus |
| `engine` | schema matching, entity resolution, events, store, healing, bench, export |
| `app` | the CLI shell and terminal rendering |
