# grove

grove 0.1.0-alpha is a local full-text search tool: one C binary and a directory you can copy. This is an early alpha. It indexes a folder, updates it when files change, and prints ranked hits with snippets.

The retrieval is in the style of Groonga's bigram tokenizer: words for alphabetic text, overlapping bigrams for CJK, with positions, phrases, prefixes, and BM25. grove is not a Groonga database. It does not read Groonga files, and it does not link libgroonga or Lucene.

Search is SQLite FTS4, compiled into the binary. The binary does not dynamically link libsqlite. FTS4 is the module with `offsets()` and `matchinfo()`, which is what ranking and the place list use. zlib is still linked for archives and compressed PDF streams.

## Build

```sh
make
make test
```

`make` writes `./grove`. `make test` builds the unit tests with address and undefined-behavior sanitizers, then builds `./grove` and runs `tests/integration.sh`. The build refuses a wrapped allocation size, checks format strings, and links with a guarded stack and full RELRO. Grove's own C follows the SEI CERT C Coding Standard: a conversion reports failure, a size that would wrap is refused, and a file is measured with `fstat` on the descriptor that was opened. Running `grove` or `grove-ui` does not need Python.

## Index

```sh
grove index ~/notes
grove search '"full text"'
```

The index lives in `$GROVE_INDEX`, or `$XDG_DATA_HOME/grove`, or `~/.local/share/grove`. `--index DIR` points at another directory. That directory holds `grove.db`. Move it with the binary. An index from before this format has to be removed and indexed again.

```sh
grove --index /media/usb/grove index ~/notes
grove --index /media/usb/grove search portable
```

`grove index` with no path refreshes every root that was indexed as a directory. A single file is stored under its parent and does not cause siblings to be removed. Index the top directory once; a later path uses the longest root that already contains it.

Files and directories whose names start with `.` are skipped, and so are symlinks. Text, Markdown, HTML, and common source extensions are indexed as text. `.docx`, `.odt`, `.epub`, and digital `.pdf` text are extracted in process. Other types are skipped. There is no OCR. A file larger than 32MB is skipped. Extracted text is kept up to that same size.

Size and modification time decide whether a file is read again. `--force` reads it anyway. `grove purge` drops entries whose files are gone. A root that is not present (the drive is unplugged, or the tree was moved) is left alone.

```sh
grove index --rebind /old/notes /new/notes
grove search --at /old/notes=/new/notes portable
```

`--rebind` rewrites a stored root. `--at` only changes paths printed by that search. With one root, `--at /new/notes` is enough. An `=` inside a path cannot be used as the separator.

`grove compact` rebuilds the full-text index and vacuums the database. `grove status` prints roots, document count, and the size of the stored text.

`grove save` remembers a query in that index. `grove saved` lists them, newest first. `grove forget` drops one. The same list is what the window shows. An older index gains the list on the first save. It does not need to be indexed again.

`grove note` keeps one line of text on a place, the path and the byte offset of a passage. `grove notes` lists them. `grove forget-note` drops one. The same notes are what the window shows under a passage.

`grove watch` remembers a query and the files that match it then. `grove alerts` lists files that are new, or whose size or time changed, since that watch. Watching the same query again marks those files seen. `grove unwatch` stops the watch and leaves the saved query. `grove forget` drops both. An older index gains this on the first watch.

## Window

```sh
make ui
./grove-ui
```

`grove-ui` is a second program. It opens the installed browser on this same index and the same query language. A second line ANDs another requirement onto the first. Two passages are listed under each file. Show more continues through the rest of that file. A click on the file name opens it with the system. A click on a passage opens that place beside the results, in its own tab: the extracted text of the file, the chapter, and the page when the file recorded one. The matched words are marked. Further passages open as more tabs, and the search stays put. Earlier and later move through that text. The original file is still opened from the reader. `ext:` and `path:` in the query show up as chips; removing a chip searches again. The file types, folders, and months in the whole match sit beside those chips, each with a file count. Choosing one adds that chip. `title:` stays in the query. Save keeps the query that is in the box. Remembered queries sit under the help line. Choosing one searches again, and × forgets it. Note, under a passage, keeps a line of text on that place. × removes it. The passage itself still opens in the reader. Watch, under the help line, keeps that query and the files it matches. A count on a remembered query is how many of those files are new or changed. Choosing the count marks them seen. Unwatch leaves the remembered query.

`grove-ui` uses the same compiled-in SQLite and also links WebUI, which needs a browser at runtime. `--index DIR` selects the index, as it does for `grove`. Firefox opens with the profile Grove, stored in `~/.local/share/grove/firefox`, so another program can keep the WebUI profile.

Saving `ui/page.html` reloads the open window. Saving `ui/main.c` rebuilds that program and swaps it in. The process and the index stay up, and the query in the box is kept. A compile error keeps the previous program and prints the compiler output. That is the same split as CToy: the host stays, and the program is replaced when a file is saved. A copy that cannot see those files uses the page packed at build time. `$GROVE_PAGE` names a different page file. Changes under `src/` or in `ui/host.c` still need `make ui` and a new process.

## Samples

`samples/books` is a small shelf of public-domain EPUBs for trying search outside the synthetic tests. `samples/SOURCES.md` lists each file, its Project Gutenberg number, and where it came from. Better Gutenberg packs those Gutenberg texts. The Loebolus PDFs checked while gathering this shelf are scanned page images; grove does not OCR, so they are not included.

```sh
./grove --index samples/index index samples/books
./grove-ui --index samples/index
```

## Query

An apostrophe inside a word is not a break. `Don't` is the token `dont`, and the passage marks the whole word, including the apostrophe. Precomposed letters in the Greek Extended block are letters, and capitals there match the small letter. A shelf indexed before those letters were words needs to be indexed again before they match.

Words in a query are combined with AND. `OR` and `NOT` are operators only in uppercase, so `or` is an ordinary word. `-term` excludes a term. Parentheses group. Quotes are a phrase: `"full text"` does not match `full-blown text`. A trailing `*` is a prefix. `ext:`, `path:`, and `title:` filter by extension, the path relative to the indexed root, and the extracted title. `mtime:2026-10` keeps files last touched in that month. The window lists those months with the type chips. The current month is labeled This month. Filters accept quotes (`ext:".md"`, `title:"Gamma Note"`).

```sh
grove search 'text -blown'
grove search 'hello OR gamma ext:txt'
grove search '東京駅'
grove search --limit 10 --json 'port*'
```

Hits are ordered by score, then path. The score is a percent of the best hit. Each hit carries the first eight passages, in order through the file. `more` in the JSON is true when further passages remain. A passage is the paragraph around the match. A long paragraph is cut to the sentences around the match, and the cut falls on a sentence boundary. A quoted phrase is one passage at that phrase, rather than a passage for each of its words. The passage marks the match with `<<` and `>>`, and gives the byte offset of that match in the extracted text. For a plain text file that offset is a file offset. The passage also names the section and the page when the file has them. A heading comes from Markdown, AsciiDoc, HTML, EPUB, Word, OpenDocument, or a PDF outline. A page comes from a form feed, a Word or OpenDocument page break, or an EPUB page break. grove does not invent either one. For a PDF, `page` is 1-based. grove writes a form feed before each text stream after the first, so a file with one stream per page gets the right page. A page stored as several streams counts once per stream. A format with no page marks uses `page` 0. `--json` is an object: `hits`, `nfiles`, and `facets`. `nfiles` and the facet counts cover every matching file, including files past `--limit`. A type is the extension. A folder is the parent of the path relative to the indexed root, with a trailing slash. The hit list itself is still capped by `--limit`. Each hit adds `nmatches` and `offsets`, the earliest 2048 FTS4 `offsets()` tuples as `{start, length, term, column}`. Those tuples are one per token, in file order. A quoted phrase is several offsets, and `term` follows the query tokens. `nmatches` counts every tuple, including ones past the 2048 that are kept. Passages still group a quoted phrase into one passage. `matchinfo` is the parsed `matchinfo('pcnalx')` blob: `p`, `c`, `n`, `avg`, `len`, and `phrases` of `{hits, total, docs}`. Ranking reads those counts, so a different rank can be coded from the same blob. The human-readable listing stays the passages. It also adds `passages`, a list of `{text, offset, page, section}`. `snippet` is the text of the first passage.

## PDF

PDF text is whatever is encoded in the file. grove reads literal strings as WinAnsi / CP1252, UTF-16BE strings, and ToUnicode `beginbfchar` / `beginbfrange` maps, and it inflates FlateDecode streams. It does not run OCR, and it does not decode every font or compression filter. A scan with no text layer indexes as empty and is skipped. A PDF with no title metadata uses the opening line of the first page. Page numbers come from text streams. A PDF indexed before form feeds were stored needs to be indexed again. An outline entry becomes the section when its destination points at a page object grove already counted. grove does not infer headings from font size, and it does not read the page tree.
