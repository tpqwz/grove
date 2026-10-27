#!/bin/sh
# Index the sample books in a temporary directory and check a few queries.
set -eu
cd "$(dirname "$0")/.."
db=$(mktemp -d)
trap 'rm -rf "$db"' EXIT
./grove --index "$db" --quiet index samples/books >/dev/null
./grove --index "$db" status | grep -q "documents: 9"
python3 - "$db" << 'PY'
import json, subprocess, sys
db = sys.argv[1]

def hits(q, limit=5):
    out = subprocess.check_output(
        ["./grove", "--index", db, "search", "--limit", str(limit), "--json", q],
        text=True,
    )
    data = json.loads(out)
    return data["hits"] if isinstance(data, dict) else data

def titles(q, limit=5):
    return [h["title"] for h in hits(q, limit)]

expect = [
    ("universally acknowledged", "Pride and Prejudice"),
    ("Telemachus", "The Odyssey of Homer"),
    ("Athos Porthos", "Les Trois Mousquetaires"),
    ("Agathon", "Symposium"),
    ("deception", "The Art of War"),
    ("serenity", "As a Man Thinketh"),
    ('"Shall I compare thee to a summer"', "Shakespeare's Sonnets"),
]
for q, title in expect:
    got = titles(q)
    if title not in got:
        raise SystemExit(f"missing {title!r} for {q!r}: {got}")
phrase = titles('"Shall I compare thee to a summer"', 5)
if phrase != ["Shakespeare's Sonnets"]:
    raise SystemExit(f"phrase hit unexpected files: {phrase}")
if len(hits("ext:epub", 20)) != 9:
    raise SystemExit("expected 9 epub documents")
limited = json.loads(subprocess.check_output(
    ["./grove", "--index", db, "search", "--limit", "1", "--json", "ext:epub"],
    text=True,
))
if limited["nfiles"] != 9 or len(limited["hits"]) != 1:
    raise SystemExit(f"facet limit: {limited['nfiles']} files, {len(limited['hits'])} hits")
types = {t["value"]: t["count"] for t in limited["facets"]["types"]}
if types != {"epub": 9} or limited["facets"]["folders"]:
    raise SystemExit(f"sample facets: {limited['facets']}")
pride = [h for h in hits("universally acknowledged", 10) if "Pride and Prejudice" in (h.get("title") or "")]
if not pride:
    raise SystemExit("no Pride hit")
secs = [p.get("section") or "" for p in pride[0]["passages"]]
if not any("Chapter I" in s for s in secs):
    raise SystemExit(f"Pride sections: {secs}")
print("samples ok")
PY
