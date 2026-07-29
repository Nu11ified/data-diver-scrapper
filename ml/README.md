# The column tagger

Training lives here; inference does not. The engine ships one 130 KB WASM
module, and this directory produces the weights it loads. No runtime from this
directory reaches production — that is the point of the export.

## Why not ONNX

Measured, gzipped, which is what Cloudflare counts:

| payload | size |
|---|---|
| the whole C++ engine, parsers and all | 0.13 MB |
| ONNX Runtime's base wasm, alone | 3.3 MB |

A Worker gets 3 MB compressed on the free plan and 10 MB on paid. ONNX Runtime
does not fit on the free plan at all, and on paid it costs 25× the engine to
replace one component of it. So PyTorch trains, and `export.py` writes the
weights into the forward pass `src/ml/columns.cpp` already runs.

That only works if the two agree. `datadiver --predict-columns` feeds columns
through the engine and prints its distribution; `test_export.py` compares it
against torch on random weights, which is the harder case because a trained
model can agree by accident when one class dominates. Current parity: **1.4e-08
maximum probability difference, zero argmax disagreements**. A transposed
matrix moves probabilities by tenths, so this bound is far tighter than the
error it exists to catch.

## Why the labels are the hard part

The corpus was labeled by exact lexicon match on column names. The name is also
a model input, so the model was largely being taught to reproduce a dictionary,
and every column the lexicon could not name was labeled `none` — teaching the
model that `CPID`, `ROLL_NUMBER` and `TXBLVALYRC` are nothing.

Labeling functions vote or abstain, and a label model weights them by how often
they agree with the consensus. Cascading them, which is what the C++ did, throws
away the abstentions that make a source's reliability estimable.

Measured by `audit.py` over 7,183 columns from 111 portals:

| function | coverage | agrees | weight | note |
|---|---|---|---|---|
| `lexicon_exact` | 15.8% | 96.2% | 6.91 | name is exactly a known synonym |
| `lexicon_token` | 8.3% | 94.3% | 6.91 | every synonym token appears in the name |
| `validator_unique` | 0.3% | 94.4% | 0.45 | values fit exactly one strict field |
| `validator_veto` | 1.7% | 95.8% | 2.29 | no field can hold these values, so `none` |
| `llm` | 82.7% | 85.0% | 3.49 | an LLM pass, independent of the lexicon |

Agreement is against the on-disk labels, which are themselves lexicon-derived,
so the two lexicon functions are flattered and the LLM's 85% is understated by
exactly the amount the lexicon is wrong.

`validator_unique` covering almost nothing is a real result, not a bug:
`parcel_id` and `case_number` are both ids, `amount_due` and `sale_price` are
both money, so the values are ambiguous and it refuses rather than guessing.

The LLM voter is what makes the rest identifiable. Without it every function is
a view of the same synonym list, they agree by construction, and cleanlab finds
nothing because there is no disagreement to find. With it:

- label-model coverage is **85.2%** of the corpus (6,120 of 7,183); it agrees
  with the on-disk label on 87.6% of what it covers
- cleanlab flags **749 of 7,183 labels (10.4%)** as suspect, overwhelmingly
  false `none`s on abbreviated names — `Taxable_Value`, `Imps_Value`,
  `Agriculture Equipment AV` all read `none` on disk and `assessed_value` to the
  label model. It also catches the lexicon firing wrongly: a column named
  `State` holding US state codes was labeled `status` because "state" is a
  `status` synonym, and the label model returns it to `none`.

The flags are not uniformly right. Where a dataset has nothing to do with
property records the LLM still reaches for the nearest field — `Most Serious
Crime` in a prison-admissions table becomes `description` — so a flag is
evidence to review, not a correction to apply blindly.

## Running it

```
python3 -m venv .venv && .venv/bin/pip install torch numpy cleanlab
.venv/bin/python audit.py       # what the corpus is worth
.venv/bin/python llm_label.py   # LLM votes, cached and resumable
.venv/bin/python train.py       # train, gate on macro-F1, export, prove parity
.venv/bin/python test_export.py # parity alone
```

`train.py` refuses to export below the macro-F1 gate, and refuses again if the
engine disagrees with torch. Macro-F1 excludes `none`: it is roughly two thirds
of the holdout, and overall accuracy lets six classes score zero behind it.

## How the holdout is kept honest

Two things used to leak, and both flattered the reported score:

- **The epoch was chosen on the holdout it then reported.** Consulting the
  holdout once per epoch and publishing its maximum reports a selected best
  case, not an estimate of unseen performance. The epoch is now chosen on a
  validation slice of portals carved out of the *training* half; the holdout
  portals are scored once, after the weights are frozen.
- **The label model was fitted over the whole corpus.** The holdout's own vote
  patterns shaped the weights that then produced the holdout's targets, which
  makes the evaluation transductive. It is now fitted on the training portals
  and applied to the held-out ones without refitting.

Neither fix makes the model better. Both make the number smaller and mean what
it claims to mean.

## What the numbers currently say

The corpus is now 7,183 columns across 111 portals, up from 2,039 across 75, and
5,938 of them (82.7%) carry an LLM vote. The label model covers 6,120; the
remaining 1,063 are columns no function would vote on, and they are dropped from
training rather than taught as `none`.

The latest run, under the honest split:

```
3610 fit / 664 validation / 1846 holdout columns, 75 vs 14 vs 21 portals, 23 classes
epoch 43 chosen on validation (macro-F1 0.611); holdout macro-F1 0.494, 56.3% on the 469 field-bearing columns
macro-F1 0.494 below the 0.50 gate: not exported
```

The 0.611 and the 0.494 are the whole point. The first is a maximum over 60
epochs on the split the epoch was chosen from; the second is what those same
frozen weights score on portals nothing ever consulted. The old code reported a
number of the first kind and called it the holdout score. The 0.117 gap is a
measurement of what selecting on the reported set is worth here, and it is
wider than most of the run-to-run differences the gate is asked to adjudicate.

So the bigger corpus did not buy a shippable model, and this run exports
nothing. The classes that still fail are the ones with almost no support —
`buyer`, `defendant` and `plaintiff` have three, one and one holdout examples
between them — which is a sampling problem, not a capacity problem. More portals
in the court and deed domains is the next lever, not more epochs.

## The architecture sweep, and why nothing shipped

Nine architectures, each scored on the holdout once and then benched end to end
against `data/golden/golden.json`. Parity is `check_parity` over 40 holdout
columns; the shipped baseline is the model already in `data/model/`.

| tag | d_model/layers/ffn | holdout macro-F1 | gzipped | parity delta | bench |
|---|---|---|---|---|---|
| *shipped* | 48/2/96 | — | 445 KB | — | **15/16, P 86% R 98% F1 91%** |
| A | 48/2/96 | 0.494 | 436 KB | 2.92e-08 | 15/16, P 85% R 97% F1 90% |
| B | 32/2/64 | 0.422 | 215 KB | 2.91e-08 | 15/16, P 83% R 98% F1 90% |
| C | 64/2/128 | **0.503** | 732 KB | 2.96e-08 | 15/16, P 85% R 98% F1 91% |
| D | 48/3/96 | 0.466 | 612 KB | 2.91e-08 | 15/16, P 85% R 98% F1 91% |
| E | 48/2/192 | 0.446 | 609 KB | 2.86e-08 | 15/16, P 85% R 98% F1 91% |
| F | 64/2/128, 8 heads | 0.482 | 733 KB | 2.92e-08 | 15/16, P 85% R 98% F1 91% |
| G | 32/3/64 | 0.494 | 294 KB | 2.84e-08 | 15/16, P 85% R 98% F1 91% |
| H | 48/2/96, lr 1e-3 | 0.432 | 436 KB | 2.73e-08 | 15/16, P 84% R 98% F1 91% |
| I | 24/2/48 | 0.420 | **133 KB** | 2.96e-08 | 15/16, P 86% R 98% F1 91% |

Every parity check found zero argmax disagreements, so all nine are faithful
exports; none of them is a better model.

Nothing beat the baseline, so nothing shipped. Two things in the table are worth
more than the ranking:

**C is the inversion again.** It is the only candidate to clear the 0.50
macro-F1 gate, and it is the only candidate that loses a mapping the baseline
gets: `cook_tax_sale` goes from 3 correct / 1 spurious to 3 / 2. The gate would
have exported the one model measurably worse on the key. Macro-F1 over 1,846
holdout columns and mapping F1 over 16 hand-verified sources are not the same
quantity, and when they disagree the key wins.

**I is identical to the shipped model where it counts.** Its per-source
ok/spurious/missed column is byte-identical to the baseline on all sixteen
sources; only the timings differ. It has the worst holdout macro-F1 in the
sweep, 0.420 against the baseline arch's 0.494, and 30% of the gzipped size. So
across a 74 KB–733 KB range and a 0.42–0.50 macro-F1 range, the bench moves by
at most one mapping. The tagger is not what the remaining 9% of mapping error is
made of, and a bigger one cannot fix it.

That also bounds this table's own worth. Picking the maximum over nine
candidates on a 16-document key is the same selection effect the holdout fix was
about, one document wide — which is why a tie is reported as a tie and not
rounded up into a reason to ship.
