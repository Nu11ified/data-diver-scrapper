"""Labeling functions and the label model that combines their votes.

Each function votes for a canonical field or abstains. Abstention is the
point: a function that never abstains carries no information about when it
should be trusted. The label model estimates per-function accuracy from
agreement patterns, so a source that only fires when it is sure outranks one
that guesses everywhere.
"""

from __future__ import annotations

import json
import math
import re
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

ABSTAIN = -1

REPO = Path(__file__).resolve().parent.parent
SCHEMA_PATH = REPO / "data" / "schema.json"
CORPUS_PATH = REPO / "data" / "columns" / "corpus.jsonl"


@dataclass
class Column:
    name: str
    values: list[str]
    label: str
    domain: str
    label_source: str = ""


def load_corpus(path: Path = CORPUS_PATH) -> list[Column]:
    rows: list[Column] = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        row = json.loads(line)
        if not row.get("name") or not row.get("label"):
            continue
        rows.append(
            Column(
                name=row["name"],
                values=[str(v) for v in row.get("values", [])],
                label=row["label"],
                domain=row.get("domain", ""),
                label_source=row.get("label_source", ""),
            )
        )
    return rows


@dataclass
class Schema:
    fields: list[dict]

    @property
    def names(self) -> list[str]:
        return [f["name"] for f in self.fields]

    def kind(self, name: str) -> str:
        for f in self.fields:
            if f["name"] == name:
                return f.get("kind", "text")
        return "text"

    def synonyms(self, name: str) -> list[str]:
        for f in self.fields:
            if f["name"] == name:
                return list(f.get("synonyms", [])) + [name]
        return [name]

    def range_of(self, name: str) -> tuple[float, float] | None:
        for f in self.fields:
            if f["name"] == name and "min" in f and "max" in f:
                return float(f["min"]), float(f["max"])
        return None


def load_schema(path: Path = SCHEMA_PATH) -> Schema:
    return Schema(json.loads(path.read_text())["fields"])


def slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")


# -- validators mirrored from src/engine/schema.cpp ------------------------
# The C++ validators are the contract at extraction time; a labeling function
# that disagreed with them would teach the model to propose values the engine
# then refuses.

_DATE = re.compile(r"^\d{4}-\d{2}-\d{2}|^\d{1,2}/\d{1,2}/\d{4}")


def _digits(text: str) -> int:
    return sum(c.isdigit() for c in text)


def valid_id(v: str) -> bool:
    if not 3 <= len(v) <= 30 or "$" in v or _DATE.match(v):
        return False
    d = _digits(v)
    return d >= 2 and d * 10 >= len(v) * 4


def valid_money(v: str) -> bool:
    cleaned = v.replace("$", "").replace(",", "").strip()
    try:
        parsed = float(cleaned)
    except ValueError:
        return False
    return 0.0 <= parsed < 1e10


def valid_date(v: str) -> bool:
    return bool(_DATE.match(v.strip()))


def valid_email(v: str) -> bool:
    if not 6 <= len(v) <= 254 or v.count("@") != 1 or " " in v:
        return False
    domain = v.split("@")[1]
    return "." in domain and len(domain.split(".")[-1]) >= 2


def valid_phone(v: str) -> bool:
    if "@" in v:
        return False
    d = _digits(v)
    return 10 <= d <= 15 and all(c.isdigit() or c in "()-+ .xX" for c in v)


def valid_number(v: str, bounds: tuple[float, float] | None) -> bool:
    try:
        parsed = float(v.strip())
    except ValueError:
        return False
    if not math.isfinite(parsed):
        return False
    if bounds is None:
        return True
    return bounds[0] <= parsed <= bounds[1]


def valid_address(v: str) -> bool:
    if not 5 <= len(v) <= 120 or not any(c.isalpha() for c in v):
        return False
    if "po box" in v.lower():
        return True
    return _digits(v) >= 1 and _digits(v) * 2 <= len(v)


STRONG_KINDS = {"id", "money", "date", "email", "phone", "number"}


def validates(schema: Schema, field_name: str, value: str) -> bool:
    kind = schema.kind(field_name)
    if kind == "id":
        return valid_id(value)
    if kind == "money":
        return valid_money(value)
    if kind == "date":
        return valid_date(value)
    if kind == "email":
        return valid_email(value)
    if kind == "phone":
        return valid_phone(value)
    if kind == "number":
        return valid_number(value, schema.range_of(field_name))
    if kind == "address":
        return valid_address(value)
    if kind == "name":
        return 2 <= len(value) <= 80 and any(c.isalpha() for c in value)
    return len(value) >= 3


# -- labeling functions ----------------------------------------------------


@dataclass
class LabelingFunction:
    name: str
    fn: object
    description: str = ""


def lf_lexicon_exact(schema: Schema):
    """The column name is exactly a known synonym. High precision, low recall."""
    lookup: dict[str, set[str]] = defaultdict(set)
    for f in schema.names:
        for s in schema.synonyms(f):
            lookup[slug(s)].add(f)

    def fn(col: Column) -> str | None:
        hits = lookup.get(slug(col.name), set())
        return next(iter(hits)) if len(hits) == 1 else None

    return LabelingFunction("lexicon_exact", fn, "name is exactly one known synonym")


def lf_lexicon_token(schema: Schema):
    """Every token of a synonym appears in the name. Looser, so noisier."""
    syns = {f: [slug(s).split("_") for s in schema.synonyms(f)] for f in schema.names}

    def fn(col: Column) -> str | None:
        tokens = set(slug(col.name).split("_"))
        if not tokens:
            return None
        winners = []
        for f, variants in syns.items():
            for parts in variants:
                if len(parts) >= 2 and set(parts) <= tokens:
                    winners.append(f)
                    break
        return winners[0] if len(winners) == 1 else None

    return LabelingFunction("lexicon_token", fn, "all synonym tokens present in name")


def lf_validator_unique(schema: Schema):
    """Values pass exactly one strongly validated field. Abstains constantly."""

    def fn(col: Column) -> str | None:
        if len(col.values) < 2:
            return None
        claims = [
            f
            for f in schema.names
            if schema.kind(f) in STRONG_KINDS
            and all(validates(schema, f, v) for v in col.values)
        ]
        return claims[0] if len(claims) == 1 else None

    return LabelingFunction("validator_unique", fn, "values fit exactly one strict field")


def lf_validator_veto(schema: Schema):
    """Not a positive vote: names the 'none' class when no field can hold the
    values at all. This is the only function that can argue for a negative."""

    def fn(col: Column) -> str | None:
        if len(col.values) < 2:
            return None
        for f in schema.names:
            if any(validates(schema, f, v) for v in col.values):
                return None
        return "none"

    return LabelingFunction("validator_veto", fn, "no field can hold these values")


def lf_llm(schema: Schema, path: Path):
    """Votes read from an LLM pass. Independent of the lexicon, which is what
    lets the label model identify the other functions' accuracies."""
    votes: dict[str, str] = {}
    if path.exists():
        for line in path.read_text().splitlines():
            if not line.strip():
                continue
            row = json.loads(line)
            key = f"{row.get('domain','')}|{row.get('name','')}"
            label = row.get("label", "")
            if label:
                votes[key] = label

    def fn(col: Column) -> str | None:
        return votes.get(f"{col.domain}|{col.name}")

    return LabelingFunction("llm", fn, f"LLM votes ({len(votes)} loaded)")


def build_labeling_functions(schema: Schema, llm_votes: Path | None = None):
    lfs = [
        lf_lexicon_exact(schema),
        lf_lexicon_token(schema),
        lf_validator_unique(schema),
        lf_validator_veto(schema),
    ]
    if llm_votes is not None:
        lfs.append(lf_llm(schema, llm_votes))
    return lfs


def vote_matrix(columns: list[Column], lfs, classes: list[str]):
    """Returns an (examples x functions) matrix of class indices, ABSTAIN for
    no vote."""
    import numpy as np

    index = {c: i for i, c in enumerate(classes)}
    matrix = np.full((len(columns), len(lfs)), ABSTAIN, dtype=np.int64)
    for r, col in enumerate(columns):
        for c, lf in enumerate(lfs):
            vote = lf.fn(col)
            if vote is not None and vote in index:
                matrix[r, c] = index[vote]
    return matrix


def lf_summary(matrix, lfs, truth, classes: list[str]) -> list[dict]:
    """Coverage and, where truth exists, the accuracy of each function on the
    subset it actually voted on."""
    import numpy as np

    out = []
    for c, lf in enumerate(lfs):
        votes = matrix[:, c]
        fired = votes != ABSTAIN
        n = int(fired.sum())
        correct = int((votes[fired] == truth[fired]).sum()) if n else 0
        out.append(
            {
                "name": lf.name,
                "coverage": n / len(votes) if len(votes) else 0.0,
                "fired": n,
                "accuracy_on_fired": correct / n if n else 0.0,
                "description": lf.description,
            }
        )
    return out


class MajorityLabelModel:
    """Weighted majority vote whose weights are each function's estimated
    accuracy, learned from how often functions agree with the consensus.

    This is the identifiable core of what Snorkel's LabelModel does. It is
    written out rather than imported because the dependency has not shipped a
    release in two years and this is fifty lines.
    """

    def __init__(self, n_classes: int, iterations: int = 20):
        self.n_classes = n_classes
        self.iterations = iterations
        self.weights = None

    def fit(self, matrix):
        import numpy as np

        n_lf = matrix.shape[1]
        self.weights = np.ones(n_lf)
        for _ in range(self.iterations):
            probs = self.predict_proba(matrix)
            consensus = probs.argmax(axis=1)
            confident = probs.max(axis=1) >= 0.5
            new = np.ones(n_lf)
            for c in range(n_lf):
                fired = (matrix[:, c] != ABSTAIN) & confident
                n = int(fired.sum())
                if n == 0:
                    new[c] = 0.1
                    continue
                agree = float((matrix[fired, c] == consensus[fired]).sum()) / n
                # Log-odds of agreement, floored so a bad function is ignored
                # rather than inverted into a confident negative signal.
                new[c] = max(0.05, math.log(max(agree, 1e-3) / max(1 - agree, 1e-3)))
            if np.allclose(new, self.weights, atol=1e-4):
                self.weights = new
                break
            self.weights = new
        return self

    def predict_proba(self, matrix):
        import numpy as np

        weights = self.weights if self.weights is not None else np.ones(matrix.shape[1])
        scores = np.zeros((matrix.shape[0], self.n_classes))
        for c in range(matrix.shape[1]):
            votes = matrix[:, c]
            fired = votes != ABSTAIN
            scores[fired, votes[fired]] += weights[c]
        # Share of the weight that went to each class, not a softmax: with
        # twenty-odd classes a softmax buries a lone unopposed vote below any
        # sane confidence threshold, which silently starves the weight update.
        empty = scores.sum(axis=1) == 0
        scores[empty] = 1.0
        return scores / scores.sum(axis=1, keepdims=True)
