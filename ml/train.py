"""Train the column tagger and export it into the C++ engine.

The split is grouped by portal domain, never random: columns from one county
portal share naming habits, so a random split lets the model see a portal's
dialect in training and be graded on it again. Macro-F1 over the field classes
is the gate, because the 'none' class is the majority and hides the rest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import random
from collections import Counter
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F

from export import check_parity, write
from labeling import (
    ABSTAIN,
    CORPUS_PATH,
    MajorityLabelModel,
    build_labeling_functions,
    load_corpus,
    load_schema,
    vote_matrix,
)
from model import ColumnTagger, Hyper, collate, tokenize

HERE = Path(__file__).resolve().parent
REPO = HERE.parent


def group_split(columns, fold: int):
    """Portal domains land wholly in train or wholly in holdout."""
    train, holdout = [], []
    for col in columns:
        digest = hashlib.sha256(col.domain.encode()).digest()
        (holdout if digest[0] % fold == 0 else train).append(col)
    return train, holdout


def macro_f1(truth, predicted, classes, skip="none"):
    per_class = {}
    for i, name in enumerate(classes):
        if name == skip:
            continue
        tp = int(((predicted == i) & (truth == i)).sum())
        fp = int(((predicted == i) & (truth != i)).sum())
        fn = int(((predicted != i) & (truth == i)).sum())
        if tp + fn == 0:
            continue  # class absent from this holdout: not evidence either way
        precision = tp / (tp + fp) if tp + fp else 0.0
        recall = tp / (tp + fn) if tp + fn else 0.0
        f1 = 2 * precision * recall / (precision + recall) if precision + recall else 0.0
        per_class[name] = {"precision": precision, "recall": recall, "f1": f1,
                           "support": tp + fn}
    mean = sum(v["f1"] for v in per_class.values()) / len(per_class) if per_class else 0.0
    return mean, per_class


def relabel(columns, classes, llm_votes: Path, verbose: bool = True):
    """Replace the on-disk lexicon labels with the label model's consensus."""
    schema = load_schema()
    lfs = build_labeling_functions(schema, llm_votes)
    matrix = vote_matrix(columns, lfs, classes)
    model = MajorityLabelModel(len(classes)).fit(matrix)
    probs = model.predict_proba(matrix)
    covered = (matrix != ABSTAIN).any(axis=1)
    if verbose:
        print("label model weights: "
              + ", ".join(f"{lf.name}={w:.2f}" for lf, w in zip(lfs, model.weights)))
        print(f"label model covers {int(covered.sum())}/{len(columns)} columns")
    kept = []
    for i, col in enumerate(columns):
        if not covered[i]:
            continue
        col.label = classes[int(probs[i].argmax())]
        kept.append(col)
    return kept


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", type=Path, default=CORPUS_PATH)
    parser.add_argument("--llm-votes", type=Path, default=HERE / "llm_votes.jsonl")
    parser.add_argument("--out", type=Path, default=REPO / "data" / "model" / "column_model.json")
    parser.add_argument("--report", type=Path, default=HERE / "train_report.json")
    parser.add_argument("--epochs", type=int, default=60)
    parser.add_argument("--batch", type=int, default=32)
    parser.add_argument("--lr", type=float, default=3e-3)
    parser.add_argument("--fold", type=int, default=5)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--gate", type=float, default=0.50, help="macro-F1 needed to export")
    parser.add_argument("--no-relabel", action="store_true", help="keep on-disk labels")
    parser.add_argument("--binary", type=Path, default=REPO / "build" / "datadiver")
    parser.add_argument("--d-model", type=int, default=48)
    parser.add_argument("--layers", type=int, default=2)
    parser.add_argument("--heads", type=int, default=4)
    parser.add_argument("--d-ffn", type=int, default=96)
    args = parser.parse_args()

    torch.manual_seed(args.seed)
    random.seed(args.seed)
    np.random.seed(args.seed)

    columns = load_corpus(args.corpus)
    classes = sorted({c.label for c in columns} | {"none"})
    if not args.no_relabel:
        columns = relabel(columns, classes, args.llm_votes)
        classes = sorted({c.label for c in columns} | {"none"})
    index = {c: i for i, c in enumerate(classes)}

    train_cols, holdout_cols = group_split(columns, args.fold)
    if not holdout_cols:
        raise SystemExit("split produced an empty holdout; lower --fold")
    print(f"{len(train_cols)} train / {len(holdout_cols)} holdout columns, "
          f"{len({c.domain for c in train_cols})} vs "
          f"{len({c.domain for c in holdout_cols})} portals, {len(classes)} classes")
    overlap = {c.domain for c in train_cols} & {c.domain for c in holdout_cols}
    if overlap:
        raise SystemExit(f"portal leaked across the split: {overlap}")

    hyper = Hyper(d_model=args.d_model, heads=args.heads, layers=args.layers, d_ffn=args.d_ffn)
    model = ColumnTagger(hyper, classes)
    print(f"{sum(p.numel() for p in model.parameters())} parameters")

    def encode(cols):
        ids = [tokenize(c.name, c.values, hyper.seq_len) for c in cols]
        labels = torch.tensor([index[c.label] for c in cols], dtype=torch.long)
        return ids, labels

    train_ids, train_y = encode(train_cols)
    hold_ids, hold_y = encode(holdout_cols)

    # The 'none' class dominates; without reweighting the model learns to say
    # nothing and scores well on accuracy while macro-F1 stays at zero.
    counts = Counter(int(y) for y in train_y)
    weights = torch.tensor(
        [1.0 / max(counts.get(i, 0), 1) ** 0.5 for i in range(len(classes))], dtype=torch.float
    )
    weights = weights / weights.mean()

    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=0.01)
    order = list(range(len(train_ids)))
    best = {"macro_f1": -1.0, "state": None, "epoch": 0}

    for epoch in range(1, args.epochs + 1):
        model.train()
        random.shuffle(order)
        total = 0.0
        for start in range(0, len(order), args.batch):
            chunk = order[start : start + args.batch]
            ids, mask = collate([train_ids[i] for i in chunk], hyper.seq_len)
            logits = model(ids, mask)
            loss = F.cross_entropy(logits, train_y[chunk], weight=weights)
            optimizer.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            total += float(loss) * len(chunk)

        model.eval()
        with torch.no_grad():
            ids, mask = collate(hold_ids, hyper.seq_len)
            predicted = model(ids, mask).argmax(dim=-1)
        score, _ = macro_f1(hold_y.numpy(), predicted.numpy(), classes)
        if score > best["macro_f1"]:
            best = {
                "macro_f1": score,
                "state": {k: v.clone() for k, v in model.state_dict().items()},
                "epoch": epoch,
            }
        if epoch % 10 == 0 or epoch == 1:
            print(f"  epoch {epoch:>3}  loss {total / len(order):.4f}  macro-F1 {score:.3f}")

    model.load_state_dict(best["state"])
    model.eval()
    with torch.no_grad():
        ids, mask = collate(hold_ids, hyper.seq_len)
        predicted = model(ids, mask).argmax(dim=-1)
    score, per_class = macro_f1(hold_y.numpy(), predicted.numpy(), classes)
    positives = hold_y != index.get("none", -1)
    positive_accuracy = float((predicted[positives] == hold_y[positives]).float().mean()) if int(positives.sum()) else 0.0

    print(f"\nbest epoch {best['epoch']}: macro-F1 {score:.3f}, "
          f"{positive_accuracy:.1%} on the {int(positives.sum())} field-bearing columns")
    print(f"  {'class':<20}{'P':>7}{'R':>7}{'F1':>7}{'n':>5}")
    for name, row in sorted(per_class.items(), key=lambda kv: -kv[1]["f1"]):
        print(f"  {name:<20}{row['precision']:>7.2f}{row['recall']:>7.2f}"
              f"{row['f1']:>7.2f}{row['support']:>5}")

    report = {
        "macro_f1": score,
        "positive_accuracy": positive_accuracy,
        "per_class": per_class,
        "train": len(train_cols),
        "holdout": len(holdout_cols),
        "classes": classes,
        "best_epoch": best["epoch"],
    }
    args.report.write_text(json.dumps(report, indent=2))

    if score < args.gate:
        print(f"\nmacro-F1 {score:.3f} below the {args.gate:.2f} gate: not exported")
        return 1

    write(model, args.out)
    print(f"\nexported to {args.out}")
    if args.binary.exists():
        samples = [(c.name, c.values[:3]) for c in holdout_cols[:40]]
        parity = check_parity(model, samples, args.binary, args.out)
        print(f"parity vs engine: max probability delta {parity['max_prob_delta']:.2e} "
              f"over {parity['checked']} columns, "
              f"{len(parity['disagreements'])} argmax disagreements")
        if parity["max_prob_delta"] > 1e-6 or parity["disagreements"]:
            print("EXPORT IS NOT FAITHFUL - the engine disagrees with the trainer")
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
