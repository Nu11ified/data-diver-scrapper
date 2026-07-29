"""Train the column tagger and export it into the C++ engine.

The split is grouped by portal domain, never random: columns from one county
portal share naming habits, so a random split lets the model see a portal's
dialect in training and be graded on it again. Macro-F1 over the field classes
is the gate, because the 'none' class is the majority and hides the rest.

The holdout portals are scored exactly once, after the model is frozen. The
epoch is chosen on a validation slice carved out of the training portals, and
the label model that produces the targets is fitted on the training portals
alone, so neither the epoch nor the labels are a function of the holdout.
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


def group_split(columns, fold: int, salt: str = ""):
    """Portal domains land wholly in one side or wholly in the other."""
    keep, held = [], []
    for col in columns:
        digest = hashlib.sha256(f"{salt}{col.domain}".encode()).digest()
        (held if digest[0] % fold == 0 else keep).append(col)
    return keep, held


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


def relabel(train_cols, holdout_cols, classes, llm_votes: Path, verbose: bool = True):
    """Replace the on-disk lexicon labels with the label model's consensus.

    The label model is fitted on the training portals alone. Fitting it over the
    whole corpus would let the holdout's vote patterns shape the weights that
    then produce the holdout's own targets, which makes the evaluation
    transductive rather than an estimate of unseen performance.
    """
    schema = load_schema()
    lfs = build_labeling_functions(schema, llm_votes)
    train_matrix = vote_matrix(train_cols, lfs, classes)
    model = MajorityLabelModel(len(classes)).fit(train_matrix)

    def apply(cols, matrix):
        probs = model.predict_proba(matrix)
        covered = (matrix != ABSTAIN).any(axis=1)
        kept = []
        for i, col in enumerate(cols):
            if not covered[i]:
                continue
            col.label = classes[int(probs[i].argmax())]
            kept.append(col)
        return kept, int(covered.sum())

    kept_train, covered_train = apply(train_cols, train_matrix)
    kept_holdout, covered_holdout = apply(
        holdout_cols, vote_matrix(holdout_cols, lfs, classes)
    )
    if verbose:
        print("label model weights (fitted on training portals only): "
              + ", ".join(f"{lf.name}={w:.2f}" for lf, w in zip(lfs, model.weights)))
        print(f"label model covers {covered_train}/{len(train_cols)} train and "
              f"{covered_holdout}/{len(holdout_cols)} holdout columns")
    return kept_train, kept_holdout


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
    index = {c: i for i, c in enumerate(classes)}

    train_cols, holdout_cols = group_split(columns, args.fold)
    if not holdout_cols:
        raise SystemExit("split produced an empty holdout; lower --fold")
    if not args.no_relabel:
        train_cols, holdout_cols = relabel(train_cols, holdout_cols, classes, args.llm_votes)
        if not holdout_cols:
            raise SystemExit("label model covered no holdout column")

    fit_cols, val_cols = group_split(train_cols, args.fold, salt="val|")
    if not val_cols:
        raise SystemExit("validation split produced no portals; lower --fold")
    print(f"{len(fit_cols)} fit / {len(val_cols)} validation / {len(holdout_cols)} holdout "
          f"columns, {len({c.domain for c in fit_cols})} vs "
          f"{len({c.domain for c in val_cols})} vs "
          f"{len({c.domain for c in holdout_cols})} portals, {len(classes)} classes")
    for a, b, what in (
        (fit_cols, holdout_cols, "fit/holdout"),
        (val_cols, holdout_cols, "validation/holdout"),
        (fit_cols, val_cols, "fit/validation"),
    ):
        overlap = {c.domain for c in a} & {c.domain for c in b}
        if overlap:
            raise SystemExit(f"portal leaked across {what}: {overlap}")

    hyper = Hyper(d_model=args.d_model, heads=args.heads, layers=args.layers, d_ffn=args.d_ffn)
    model = ColumnTagger(hyper, classes)
    print(f"{sum(p.numel() for p in model.parameters())} parameters")

    def encode(cols):
        ids = [tokenize(c.name, c.values, hyper.seq_len) for c in cols]
        labels = torch.tensor([index[c.label] for c in cols], dtype=torch.long)
        return ids, labels

    train_ids, train_y = encode(fit_cols)
    val_ids, val_y = encode(val_cols)
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
            ids, mask = collate(val_ids, hyper.seq_len)
            predicted = model(ids, mask).argmax(dim=-1)
        score, _ = macro_f1(val_y.numpy(), predicted.numpy(), classes)
        if score > best["macro_f1"]:
            best = {
                "macro_f1": score,
                "state": {k: v.clone() for k, v in model.state_dict().items()},
                "epoch": epoch,
            }
        if epoch % 10 == 0 or epoch == 1:
            print(f"  epoch {epoch:>3}  loss {total / len(order):.4f}  "
                  f"validation macro-F1 {score:.3f}")

    model.load_state_dict(best["state"])
    model.eval()
    with torch.no_grad():
        ids, mask = collate(hold_ids, hyper.seq_len)
        predicted = model(ids, mask).argmax(dim=-1)
    score, per_class = macro_f1(hold_y.numpy(), predicted.numpy(), classes)
    positives = hold_y != index.get("none", -1)
    positive_accuracy = float((predicted[positives] == hold_y[positives]).float().mean()) if int(positives.sum()) else 0.0

    print(f"\nepoch {best['epoch']} chosen on validation (macro-F1 {best['macro_f1']:.3f}); "
          f"holdout macro-F1 {score:.3f}, "
          f"{positive_accuracy:.1%} on the {int(positives.sum())} field-bearing columns")
    print(f"  {'class':<20}{'P':>7}{'R':>7}{'F1':>7}{'n':>5}")
    for name, row in sorted(per_class.items(), key=lambda kv: -kv[1]["f1"]):
        print(f"  {name:<20}{row['precision']:>7.2f}{row['recall']:>7.2f}"
              f"{row['f1']:>7.2f}{row['support']:>5}")

    report = {
        "macro_f1": score,
        "validation_macro_f1": best["macro_f1"],
        "positive_accuracy": positive_accuracy,
        "per_class": per_class,
        "fit": len(fit_cols),
        "validation": len(val_cols),
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
