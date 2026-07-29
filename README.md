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
make            # configure, build, run the tests
make run        # build and open the shell
make renderer   # install the headless browser used for JS-only portals
make run-with-renderer   # shell with rendering enabled
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
