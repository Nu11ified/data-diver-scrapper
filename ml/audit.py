"""What the corpus is actually worth: labeling-function behaviour, the label
model's agreement with the lexicon labels already on disk, and cleanlab's
verdict on which of those labels are wrong."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

from labeling import (
    ABSTAIN,
    CORPUS_PATH,
    MajorityLabelModel,
    build_labeling_functions,
    lf_summary,
    load_corpus,
    load_schema,
    vote_matrix,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", type=Path, default=CORPUS_PATH)
    parser.add_argument("--llm-votes", type=Path, default=Path(__file__).parent / "llm_votes.jsonl")
    parser.add_argument("--out", type=Path, default=Path(__file__).parent / "audit.json")
    args = parser.parse_args()

    schema = load_schema()
    columns = load_corpus(args.corpus)
    classes = sorted({c.label for c in columns} | {"none"})
    index = {c: i for i, c in enumerate(classes)}
    truth = np.array([index[c.label] for c in columns])

    lfs = build_labeling_functions(schema, args.llm_votes)
    matrix = vote_matrix(columns, lfs, classes)

    print(f"corpus: {len(columns)} columns, {len(classes)} classes, "
          f"{len({c.domain for c in columns})} portals")
    print(f"existing labels: "
          f"{sum(1 for c in columns if c.label != 'none')} positive, "
          f"{sum(1 for c in columns if c.label == 'none')} none\n")

    print("labeling functions (accuracy is against the on-disk labels, which")
    print("are themselves lexicon-derived, so lexicon LFs are flattered):")
    print(f"  {'function':<18} {'coverage':>9} {'fired':>7} {'agrees':>8}")
    for row in lf_summary(matrix, lfs, truth, classes):
        print(f"  {row['name']:<18} {row['coverage']:>8.1%} {row['fired']:>7} "
              f"{row['accuracy_on_fired']:>7.1%}")

    model = MajorityLabelModel(len(classes)).fit(matrix)
    print("\nlearned label-model weights:")
    for lf, w in zip(lfs, model.weights):
        print(f"  {lf.name:<18} {w:>6.2f}")

    probs = model.predict_proba(matrix)
    covered = (matrix != ABSTAIN).any(axis=1)
    predicted = probs.argmax(axis=1)
    agree = int((predicted[covered] == truth[covered]).sum())
    print(f"\nlabel model covers {covered.sum()}/{len(columns)} columns "
          f"({covered.mean():.1%}); agrees with the on-disk label on "
          f"{agree}/{int(covered.sum())} ({agree / max(int(covered.sum()), 1):.1%})")

    # cleanlab needs per-class probabilities and the labels being audited.
    try:
        from cleanlab.filter import find_label_issues

        issues = find_label_issues(
            labels=truth,
            pred_probs=probs,
            return_indices_ranked_by="self_confidence",
        )
        print(f"\ncleanlab flags {len(issues)} of {len(columns)} labels as suspect "
              f"({len(issues) / len(columns):.1%})")
        print("worst offenders:")
        for i in issues[:12]:
            col = columns[i]
            print(f"  {col.name[:34]:<34} labeled {col.label:<16} "
                  f"model says {classes[predicted[i]]}")
    except Exception as exc:  # cleanlab is advisory, not load-bearing
        issues = []
        print(f"\ncleanlab unavailable: {exc}")

    args.out.write_text(
        json.dumps(
            {
                "columns": len(columns),
                "classes": classes,
                "lf_summary": lf_summary(matrix, lfs, truth, classes),
                "weights": {lf.name: float(w) for lf, w in zip(lfs, model.weights)},
                "coverage": float(covered.mean()),
                "suspect_indices": [int(i) for i in issues],
            },
            indent=2,
        )
    )
    print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
