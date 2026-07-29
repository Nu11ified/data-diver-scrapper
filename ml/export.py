"""Flatten trained weights into the parameter vector src/ml/columns.cpp reads.

The C++ keeps every parameter in one flat array whose order is fixed by
make_layout(). Linear layers are stored input-major (rows = inputs, cols =
outputs), which is the transpose of torch's nn.Linear weight, so every matrix
is transposed on the way out. Getting this wrong produces a model that loads,
runs, and is quietly wrong, which is why check_parity exists.
"""

from __future__ import annotations

import json
from pathlib import Path

import torch

from model import VOCAB, ColumnTagger, Hyper


def flatten(model: ColumnTagger) -> list[float]:
    h = model.hyper
    out: list[float] = []

    def push(tensor: torch.Tensor) -> None:
        out.extend(tensor.detach().flatten().tolist())

    def push_linear(layer: torch.nn.Linear) -> None:
        push(layer.weight.T.contiguous())  # input-major for the C++ layout
        push(layer.bias)

    push(model.tok.weight)  # VOCAB x d
    push(model.pos.weight)  # seq_len x d
    for block in model.blocks:
        push(block.ln1.weight)
        push(block.ln1.bias)
        push_linear(block.wq)
        push_linear(block.wk)
        push_linear(block.wv)
        push_linear(block.wo)
        push(block.ln2.weight)
        push(block.ln2.bias)
        push_linear(block.w1)
        push_linear(block.w2)
    push(model.lnf.weight)
    push(model.lnf.bias)
    push_linear(model.head)
    return out


def expected_size(h: Hyper, classes: int) -> int:
    d, ffn = h.d_model, h.d_ffn
    per_block = 2 * d + 4 * (d * d + d) + 2 * d + (d * ffn + ffn) + (ffn * d + d)
    return VOCAB * d + h.seq_len * d + h.layers * per_block + 2 * d + d * classes + classes


def to_json(model: ColumnTagger) -> str:
    h = model.hyper
    params = flatten(model)
    want = expected_size(h, len(model.classes))
    if len(params) != want:
        raise SystemExit(f"export: produced {len(params)} params, layout wants {want}")
    return json.dumps(
        {
            "kind": "column_transformer",
            "hyper": {
                "seq_len": h.seq_len,
                "d_model": h.d_model,
                "heads": h.heads,
                "layers": h.layers,
                "d_ffn": h.d_ffn,
            },
            "classes": model.classes,
            "params": params,
        }
    )


def write(model: ColumnTagger, path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(to_json(model))
    return path


def check_parity(model: ColumnTagger, samples, binary: Path, model_path: Path) -> dict:
    """Run the same columns through the C++ and compare argmax and probability.

    Parity is not decoration: a transposed matrix still yields a plausible
    distribution, so only agreement with the engine proves the export.
    """
    import subprocess

    from model import collate, tokenize

    model.eval()
    with torch.no_grad():
        ids, mask = collate([tokenize(n, v, model.hyper.seq_len) for n, v in samples],
                            model.hyper.seq_len)
        probs = model(ids, mask).softmax(dim=-1)

    payload = json.dumps(
        {"model": str(model_path), "columns": [{"name": n, "values": v} for n, v in samples]}
    )
    result = subprocess.run(
        [str(binary), "--predict-columns", "-"],
        input=payload,
        capture_output=True,
        text=True,
        timeout=120,
    )
    if result.returncode != 0:
        raise SystemExit(f"parity: engine failed: {result.stderr[-400:]}")
    engine = json.loads(result.stdout)

    worst = 0.0
    disagreements = []
    for i, row in enumerate(engine["predictions"]):
        mine = probs[i]
        theirs = torch.tensor([row["distribution"].get(c, 0.0) for c in model.classes])
        worst = max(worst, float((mine - theirs).abs().max()))
        if model.classes[int(mine.argmax())] != row["label"]:
            disagreements.append(
                {
                    "column": samples[i][0],
                    "torch": model.classes[int(mine.argmax())],
                    "engine": row["label"],
                }
            )
    return {"max_prob_delta": worst, "disagreements": disagreements, "checked": len(samples)}
