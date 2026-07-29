"""Prove the exported weights mean the same thing to the engine as to torch.

A transposed weight matrix still loads and still yields a plausible-looking
distribution, so the only real check is running both and comparing.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import torch

from export import check_parity, expected_size, flatten, write
from model import ColumnTagger, Hyper

REPO = Path(__file__).resolve().parent.parent
BINARY = Path(os.environ.get("DATADIVER_BIN", REPO / "build" / "datadiver"))

SAMPLES = [
    ("owner_name", ["SMITH, JANE", "ACME LLC"]),
    ("tax_due", ["1200.50", "98.00"]),
    ("parcel", ["101-22-0001", "101-22-0002"]),
    ("situs_address", ["436 W 31ST ST", "9628 10TH BAY ST"]),
    ("violation_date", ["2026-01-04", "2025-11-30"]),
    ("mail_address_name", ["BIG DEAHL LTD LIAB CO", "R & D ARELLANO"]),
    ("weird_internal_code", ["ZX-4", "QQ-9"]),
]


def main() -> int:
    torch.manual_seed(3)
    classes = ["address", "amount_due", "event_date", "none", "owner", "parcel_id"]
    hyper = Hyper(seq_len=64, d_model=48, heads=4, layers=2, d_ffn=96)
    model = ColumnTagger(hyper, classes)
    # Random weights are the harder test: a trained model can agree by
    # accident because most probability sits on one class regardless.
    model.eval()

    params = flatten(model)
    want = expected_size(hyper, len(classes))
    assert len(params) == want, f"flatten produced {len(params)}, layout wants {want}"
    print(f"parameter count matches the C++ layout: {want}")

    if not BINARY.exists():
        print(f"engine binary missing at {BINARY}; build it first", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        path = write(model, Path(tmp) / "column_model.json")
        loaded = json.loads(path.read_text())
        assert loaded["kind"] == "column_transformer"
        assert loaded["classes"] == classes

        result = check_parity(model, SAMPLES, BINARY, path)

    print(f"checked {result['checked']} columns against the engine")
    print(f"max probability delta: {result['max_prob_delta']:.3e}")
    if result["disagreements"]:
        for row in result["disagreements"]:
            print(f"  DISAGREE {row['column']}: torch={row['torch']} engine={row['engine']}")
        return 1
    if result["max_prob_delta"] > 1e-6:
        print("probabilities drifted beyond 1e-6: the export is not faithful")
        return 1
    print("export is faithful: the engine reproduces torch to within 1e-6")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
