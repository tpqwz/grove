#!/bin/bash
# End-to-end CLI check. The unit tests cover the library; this drives the binary.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
GROVE=$ROOT/grove
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
IDX=$TMP/index
DOCS=$TMP/docs
DOCS2=$TMP/docs2
mkdir -p "$DOCS/sub"

printf '%s\n' 'Hello there. This is a full text note about portable search. Do not stop.' >"$DOCS/a.txt"
printf '%s\n' 'A full-blown text about something else.' >"$DOCS/b.txt"
printf '%s\n' '# Gamma Note' 'gamma lives here with alpha' >"$DOCS/sub/c.md"
printf '%s\n' '東京駅に行く' >"$DOCS/tokyo.txt"
printf '%s\n' 'alpha beta' >"$DOCS/keep.txt"
printf '%s\n' 'the operator or stays a word' >"$DOCS/or.txt"
printf '%s\n' 'kelp in a python source file' >"$DOCS/notes.py"
printf '%s\n' '<html><head><title>Mango Page</title></head><body><h1>Citrus Box</h1><p>html body citrus</p><script>secretnoise</script></body></html>' >"$DOCS/note.html"
printf '%s\n' 'nope' >"$DOCS/skip.bin"

python3 - "$DOCS" <<'PY'
import sys, zipfile
docs = sys.argv[1]

def write_zip(path, members):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for name, data in members.items():
            z.writestr(name, data)

write_zip(f"{docs}/page.docx", {
    "word/document.xml": (
        '<?xml version="1.0"?>'
        '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">'
        '<w:p><w:r><w:t>hello</w:t></w:r><w:r><w:tab/></w:r><w:r><w:t>world</w:t></w:r></w:p>'
        '<w:p><w:r><w:t>portable docx search</w:t></w:r></w:p>'
        '</w:document>'
    ),
    "docProps/core.xml": "<cp:coreProperties><dc:title>Docx Title</dc:title></cp:coreProperties>",
})
write_zip(f"{docs}/book.epub", {
    "OEBPS/content.opf": "<package><metadata><dc:title>Epub Title</dc:title></metadata></package>",
    "OEBPS/ch1.xhtml": "<html><body><p>epub chapter zebra</p></body></html>",
})
write_zip(f"{docs}/note.odt", {
    "content.xml": "<office:document-content><text:p>odt paragraph quinoa</text:p></office:document-content>",
    "meta.xml": "<office:meta><dc:title>Odt Title</dc:title></office:meta>",
})
stream = "(Groonga portable search.) Tj\n"
pdf = (
    "%PDF-1.4\n"
    "1 0 obj\n"
    f"<< /Length {len(stream)} >>\n"
    "stream\n"
    + stream
    + "\nendstream\nendobj\n/Title (Pdf Notes)\n"
)
open(f"{docs}/page.pdf", "w", encoding="ascii").write(pdf)
PY

test "$("$GROVE" --version)" = "grove 0.1.0-alpha"
"$GROVE" --help >/dev/null

out=$("$GROVE" --index "$IDX" index "$DOCS")
printf '%s\n' "$out" | grep -q '^indexed '

json_has() {
  local query=$1
  local needle=$2
  "$GROVE" --index "$IDX" search --json -- "$query" | python3 -c '
import json, sys
needle = sys.argv[1]
data = json.load(sys.stdin)
hits = data["hits"] if isinstance(data, dict) else data
blob = "\n".join(
    " ".join([h.get("path") or "", h.get("title") or "", h.get("snippet") or "", h.get("ext") or ""])
    for h in hits
)
if needle not in blob:
    sys.stderr.write(json.dumps(hits, ensure_ascii=False, indent=2) + "\n")
    sys.stderr.write("missing %r\n" % needle)
    sys.exit(1)
' "$needle"
}

json_count() {
  local query=$1
  local want=$2
  "$GROVE" --index "$IDX" search --json -- "$query" | python3 -c '
import json, sys
data = json.load(sys.stdin)
hits = data["hits"] if isinstance(data, dict) else data
want = int(sys.argv[1])
if len(hits) != want:
    sys.stderr.write(json.dumps(hits, ensure_ascii=False, indent=2) + "\n")
    sys.stderr.write("got %d hits, want %d\n" % (len(hits), want))
    sys.exit(1)
if hits and hits[0]["score"] != 100:
    sys.stderr.write("best score %s\n" % hits[0]["score"])
    sys.exit(1)
' "$want"
}

json_count '"full text"' 1
json_has '"full text"' '/a.txt'
json_has '"full text"' '<<'
"$GROVE" --index "$IDX" search --json -- '"full text"' | python3 -c '
import json, sys
data = json.load(sys.stdin)
h = (data["hits"] if isinstance(data, dict) else data)[0]
ps = h["passages"]
assert ps and ps[0]["text"] == h["snippet"]
assert ps[0]["page"] == 0 and ps[0]["offset"] > 0
assert ps[0]["section"] == ""
assert h["nmatches"] == 2
offs = h["offsets"]
assert len(offs) == 2
assert offs[0]["start"] == ps[0]["offset"]
assert offs[0]["length"] == 4 and offs[0]["term"] == 0 and offs[0]["column"] == 0
assert offs[1]["length"] == 4 and offs[1]["term"] == 1 and offs[1]["column"] == 0
assert offs[1]["start"] == offs[0]["start"] + 5
mi = h["matchinfo"]
assert mi["p"] == 1 and mi["c"] == 1 and mi["n"] >= 1
assert mi["phrases"][0]["hits"] == 1 and mi["phrases"][0]["docs"] >= 1
'
"$GROVE" --index "$IDX" search --json -- groonga | python3 -c '
import json, sys
data = json.load(sys.stdin)
h = (data["hits"] if isinstance(data, dict) else data)[0]
assert h["passages"][0]["page"] == 1
'
if "$GROVE" --index "$IDX" search --json -- '"full text"' | python3 -c '
import json,sys
data=json.load(sys.stdin)
hits=data["hits"] if isinstance(data, dict) else data
sys.exit(0 if all("/b.txt" not in h["path"] for h in hits) else 1)
'; then
  :
else
  echo "phrase matched full-blown" >&2
  exit 1
fi

json_count '"full-blown text"' 1
json_has '"full-blown text"' '/b.txt'
json_count '東京駅' 1
json_has '東京駅' '/tokyo.txt'
json_count 'ext:md gamma' 1
json_has 'path:sub gamma' '/c.md'
json_count 'title:Mango' 1
json_count 'secretnoise' 0
json_count 'citrus' 1
"$GROVE" --index "$IDX" search --json -- 'path:sub gamma' | python3 -c '
import json, sys
data = json.load(sys.stdin)
h = (data["hits"] if isinstance(data, dict) else data)[0]
assert h["passages"][0]["section"] == "Gamma Note", h
'
"$GROVE" --index "$IDX" search --json -- citrus | python3 -c '
import json, sys
data = json.load(sys.stdin)
h = (data["hits"] if isinstance(data, dict) else data)[0]
assert h["passages"][0]["section"] == "Citrus Box", h
assert "Mango" in (h["title"] or "")
'
"$GROVE" --index "$IDX" search --json -- zebra | python3 -c '
import json, sys
data = json.load(sys.stdin)
h = (data["hits"] if isinstance(data, dict) else data)[0]
assert h["passages"][0]["section"] == "ch1", h
'
json_count 'kelp' 1
json_count '"hello world"' 1
json_has '"hello world"' 'Docx Title'
json_count 'zebra' 1
json_has 'zebra' 'Epub Title'
json_count 'quinoa' 1
json_has 'quinoa' 'Odt Title'
json_count 'ext:pdf groonga' 1
json_has 'groonga' 'Pdf Notes'
json_count 'or' 1
json_has 'port*' '/page.pdf'
json_count 'text -blown' 1
json_has 'hello OR gamma' '/a.txt'
json_has 'hello OR gamma' '/c.md'
json_has 'hello OR gamma' '/page.docx'

human=$("$GROVE" --index "$IDX" search Hello)
printf '%s\n' "$human" | grep -q '100%'
printf '%s\n' "$human" | grep -q '<<Hello>>'

"$GROVE" --index "$IDX" search --limit 1 --json -- 'hello OR gamma' | python3 -c '
import json,sys
data=json.load(sys.stdin)
assert len(data["hits"]) == 1, data
assert data["nfiles"] == 3, data["nfiles"]
types={t["value"]: t["count"] for t in data["facets"]["types"]}
assert types == {"txt": 1, "md": 1, "docx": 1}, types
folders={t["value"]: t["count"] for t in data["facets"]["folders"]}
assert folders == {"sub/": 1}, folders
'
"$GROVE" --index "$IDX" search --limit 1 --json -- 'hello OR gamma path:sub/' | python3 -c '
import json,sys
data=json.load(sys.stdin)
assert data["nfiles"] == 1, data
assert {t["value"]: t["count"] for t in data["facets"]["types"]} == {"md": 1}
assert {t["value"]: t["count"] for t in data["facets"]["folders"]} == {"sub/": 1}
'

stored=$("$GROVE" --index "$IDX" status | sed -n 's/^root: \(.*\) \[full\]$/\1/p' | head -n 1)
test -n "$stored"
"$GROVE" --index "$IDX" search --json --at "$stored=$DOCS2" -- gamma | python3 -c '
import json,sys
data=json.load(sys.stdin)
hits=data["hits"] if isinstance(data, dict) else data
assert hits and all(sys.argv[1] in h["path"] for h in hits), hits
' "$DOCS2"

# Same size and mtime must not pick up an edit unless --force is set.
python3 - "$DOCS/keep.txt" <<'PY'
import os, sys
path = sys.argv[1]
st = os.stat(path)
text = open(path, "rb").read()
assert text == b"alpha beta\n"
open(path, "wb").write(b"ZZZZZ beta\n")
os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns))
PY
"$GROVE" --index "$IDX" index "$DOCS" >/dev/null
json_count 'ZZZZZ' 0
"$GROVE" --index "$IDX" index --force "$DOCS" >/dev/null
json_count 'ZZZZZ' 1
json_has 'alpha' '/c.md'

printf '%s\n' 'gamma replaced the previous note' >"$DOCS/a.txt"
"$GROVE" --index "$IDX" index "$DOCS" >/dev/null
json_count '"full text"' 0
json_has 'gamma' '/a.txt'
json_has 'gamma' '/c.md'

rm -f "$DOCS/b.txt"
"$GROVE" --index "$IDX" purge >/dev/null
json_count '"full-blown text"' 0
json_count 'something' 0

"$GROVE" --index "$IDX" compact >/dev/null
"$GROVE" --index "$IDX" status | grep -q '^segments: 1$'
json_has 'gamma' '/a.txt'
json_has '東京駅' '/tokyo.txt'

mv "$DOCS" "$DOCS2"
if "$GROVE" --index "$IDX" index 2>"$TMP/miss.err"; then
  echo "reindex of a missing root should fail" >&2
  exit 1
fi
grep -q rebind "$TMP/miss.err"
"$GROVE" --index "$IDX" index --rebind "$stored" "$DOCS2" >/dev/null
"$GROVE" --index "$IDX" search --json -- gamma | python3 -c '
import json,sys
data=json.load(sys.stdin)
hits=data["hits"] if isinstance(data, dict) else data
assert hits and all(sys.argv[1] in h["path"] for h in hits), hits
' "$DOCS2"

quiet=$("$GROVE" --quiet --index "$IDX" index "$DOCS2")
test -z "$quiet"

if "$GROVE" --index "$IDX" search 'hello OR' 2>/dev/null; then
  echo "broken query should fail" >&2
  exit 1
fi

test -z "$("$GROVE" --index "$IDX" saved)"
"$GROVE" --index "$IDX" save 'hello OR gamma'
test "$("$GROVE" --index "$IDX" saved)" = "hello OR gamma"
if "$GROVE" --index "$IDX" save 'mtime:2026-13' 2>"$TMP/saved.err"; then
  echo "bad month should not save" >&2
  exit 1
fi
grep -q 'mtime:' "$TMP/saved.err"
json_has 'gamma' '/a.txt'
"$GROVE" --index "$IDX" forget 'hello OR gamma'
test -z "$("$GROVE" --index "$IDX" saved)"

test -z "$("$GROVE" --index "$IDX" notes)"
"$GROVE" --index "$IDX" note "$DOCS2/a.txt" 12 'a margin note'
test "$("$GROVE" --index "$IDX" notes)" = "$(printf '12\t%s\ta margin note' "$DOCS2/a.txt")"
if "$GROVE" --index "$IDX" note "$DOCS2/a.txt" 12 '   ' 2>"$TMP/note.err"; then
  echo "empty note should fail" >&2
  exit 1
fi
grep -q 'empty note' "$TMP/note.err"
json_has 'gamma' '/a.txt'
"$GROVE" --index "$IDX" forget-note "$DOCS2/a.txt" 12
test -z "$("$GROVE" --index "$IDX" notes)"

test -z "$("$GROVE" --index "$IDX" alerts)"
"$GROVE" --index "$IDX" watch hello
test "$("$GROVE" --index "$IDX" saved)" = "hello"
test -z "$("$GROVE" --index "$IDX" alerts)"
if "$GROVE" --index "$IDX" watch 'mtime:2026-13' 2>"$TMP/watch.err"; then
  echo "bad month should not watch" >&2
  exit 1
fi
grep -q 'mtime:' "$TMP/watch.err"
printf '%s\n' 'fresh alert hello' >"$DOCS2/fresh.txt"
"$GROVE" --index "$IDX" index "$DOCS2" >/dev/null
alerts=$("$GROVE" --index "$IDX" alerts)
first=$(printf '%s\n' "$alerts" | head -n 1)
if [ "$first" != "1 hello" ]; then
  printf 'alerts:\n%s\n' "$alerts" >&2
  exit 1
fi
printf '%s\n' "$alerts" | grep -q '/fresh.txt'
"$GROVE" --index "$IDX" watch hello
test -z "$("$GROVE" --index "$IDX" alerts)"
"$GROVE" --index "$IDX" unwatch hello
test "$("$GROVE" --index "$IDX" saved)" = "hello"
test -z "$("$GROVE" --index "$IDX" alerts)"
"$GROVE" --index "$IDX" forget hello
test -z "$("$GROVE" --index "$IDX" saved)"
test -z "$("$GROVE" --index "$IDX" alerts)"

echo "integration ok"
