"""Label columns with an LLM, in batches, through the local codex CLI.

This is the vote that owes nothing to the lexicon. Without it every labeling
function is a different view of the same synonym list, the label model cannot
tell them apart, and cleanlab has no disagreement to learn from.

Votes are cached by (domain, name) so re-running only pays for new columns.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

from labeling import CORPUS_PATH, load_corpus, load_schema

HERE = Path(__file__).resolve().parent


def build_prompt(schema, batch) -> str:
    catalog = "\n".join(
        f"- {f['name']} ({f.get('kind', 'text')}): "
        f"e.g. {', '.join(f.get('synonyms', [])[:5])}"
        for f in schema.fields
    )
    columns = "\n".join(
        f"{i}. name={c.name!r} sample_values={c.values[:3]!r}" for i, c in enumerate(batch)
    )
    return f"""You are labelling columns from public-record datasets (county tax, \
assessment, permit, violation and court data) against a fixed schema.

CANONICAL FIELDS:
{catalog}

For each numbered column below, decide which canonical field it holds, using \
the column name AND its sample values. Answer "none" when the column is not \
one of these fields (ids of other entities, geometry, internal codes, \
administrative flags, anything unrelated).

Judge by meaning, not by string similarity: "mail_address_name" holds an owner \
name, not an address; "grantee" is a buyer; "situs" is a property address. If \
the values contradict the name, trust the values.

COLUMNS:
{columns}

Output ONLY a JSON array, one object per column, no prose and no code fence:
[{{"i": 0, "label": "<field name or none>", "confidence": <0.0-1.0>}}]
"""


def parse_reply(text: str, size: int):
    start = text.find("[")
    end = text.rfind("]")
    if start == -1 or end <= start:
        return []
    blob = text[start : end + 1]
    try:
        rows = json.loads(blob)
    except json.JSONDecodeError:
        return []
    out = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        i = row.get("i")
        label = row.get("label")
        if not isinstance(i, int) or not isinstance(label, str) or not 0 <= i < size:
            continue
        try:
            confidence = float(row.get("confidence", 0.0))
        except (TypeError, ValueError):
            confidence = 0.0
        out.append((i, label.strip(), confidence))
    return out


def run_codex(prompt: str, model: str, timeout: int) -> str:
    result = subprocess.run(
        ["codex", "exec", "--skip-git-repo-check", "-m", model, "-"],
        input=prompt,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if result.returncode != 0:
        raise RuntimeError(f"codex exec failed: {result.stderr[-400:]}")
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", type=Path, default=CORPUS_PATH)
    parser.add_argument("--out", type=Path, default=HERE / "llm_votes.jsonl")
    parser.add_argument("--batch", type=int, default=25)
    parser.add_argument("--limit", type=int, default=0, help="0 labels everything unlabelled")
    parser.add_argument("--model", default="gpt-5.6-sol")
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()

    schema = load_schema()
    valid = set(schema.names) | {"none"}
    columns = load_corpus(args.corpus)

    done: set[str] = set()
    if args.out.exists():
        for line in args.out.read_text().splitlines():
            if line.strip():
                row = json.loads(line)
                done.add(f"{row.get('domain','')}|{row.get('name','')}")

    todo = [c for c in columns if f"{c.domain}|{c.name}" not in done]
    if args.limit:
        todo = todo[: args.limit]
    if not todo:
        print("nothing to label")
        return 0

    print(f"labelling {len(todo)} columns in batches of {args.batch} via {args.model}")
    written = 0
    rejected = 0
    with args.out.open("a") as sink:
        for start in range(0, len(todo), args.batch):
            batch = todo[start : start + args.batch]
            try:
                reply = run_codex(build_prompt(schema, batch), args.model, args.timeout)
            except Exception as exc:
                print(f"  batch {start // args.batch}: {exc}", file=sys.stderr)
                continue
            rows = parse_reply(reply, len(batch))
            if not rows:
                print(f"  batch {start // args.batch}: unparseable reply", file=sys.stderr)
                continue
            for i, label, confidence in rows:
                if label not in valid:
                    rejected += 1
                    continue
                col = batch[i]
                sink.write(
                    json.dumps(
                        {
                            "domain": col.domain,
                            "name": col.name,
                            "label": label,
                            "confidence": confidence,
                        }
                    )
                    + "\n"
                )
                written += 1
            sink.flush()
            print(f"  batch {start // args.batch + 1}/"
                  f"{(len(todo) + args.batch - 1) // args.batch}: {len(rows)} votes")

    print(f"wrote {written} votes to {args.out}"
          + (f"; rejected {rejected} off-schema labels" if rejected else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
