import collections
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "data/columns/corpus.jsonl")
rows = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]

labels = collections.Counter(r.get("label", "") for r in rows)
portals = collections.Counter(r.get("domain", "") for r in rows)
sources = collections.Counter(r.get("label_source", "") for r in rows)

print(f"columns          {len(rows)}")
print(f"distinct portals {len(portals)}")
print(f"distinct labels  {len(labels)}")
print(f"unique (portal, name) {len({(r.get('domain'), r.get('name')) for r in rows})}")
print()
print("per label:")
for label, count in labels.most_common():
    print(f"  {label or '(empty)':<28} {count:>6}  {count / len(rows) * 100:5.1f}%")
print()
print("label source:")
for source, count in sources.most_common():
    print(f"  {source or '(empty)':<28} {count:>6}")
print()
print("top 15 portals:")
for domain, count in portals.most_common(15):
    print(f"  {domain:<40} {count:>5}")
