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

| function | coverage | note |
|---|---|---|
| `lexicon_exact` | 32% | name is exactly a known synonym |
| `lexicon_token` | 15% | every synonym token appears in the name |
| `validator_unique` | 0% | values fit exactly one strict field |
| `validator_veto` | 2% | no field can hold these values, so `none` |
| `llm` | 82% | an LLM pass, independent of the lexicon |

`validator_unique` covering nothing is a real result, not a bug: `parcel_id` and
`case_number` are both ids, `amount_due` and `sale_price` are both money, so the
values are ambiguous and it refuses rather than guessing.

The LLM voter is what makes the rest identifiable. Without it every function is
a view of the same synonym list, they agree by construction, and cleanlab finds
nothing because there is no disagreement to find. With it:

- label-model coverage rose from **34% to 100%** of the corpus
- cleanlab flagged **40 of 403 labels (9.9%)** as wrong, nearly all false
  `none`s on abbreviated names

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

## What the numbers currently say

The honest reading is that the corpus is too small, not that the model is good.
2,039 columns across 75 portals, held out by portal, leaves most classes with
one to six examples in the holdout — macro-F1 on that has error bars wide enough
to swallow the difference between runs. Growing the corpus is the next lever,
and `harvest` now merges rather than replacing so it accumulates.
