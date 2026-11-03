# grove

grove 0.1.0-alpha is a local full-text search tool: one C binary and a directory you can copy. This is an early alpha. It indexes a folder, updates it when files change, and prints ranked hits with snippets.

The retrieval is in the style of Groonga's bigram tokenizer: words for alphabetic text, overlapping bigrams for CJK, with positions, phrases, prefixes, and BM25. grove is not a Groonga database. It does not read Groonga files, and it does not link libgroonga or Lucene.

Search is SQLite FTS4, compiled into the binary. The binary does not dynamically link libsqlite. FTS4 is the module with `offsets()` and `matchinfo()`, which is what ranking and the place list use. zlib is compiled into the Linux and Windows builds for archives and compressed PDF streams. The macOS build uses the zlib that ships with macOS.

## Build

```sh
make
make test
```

`make` writes `./grove`, or `grove.exe` on Windows. That one program is the commands and the window. On Linux and macOS, `make test` builds the unit tests with address and undefined-behavior sanitizers, then builds `./grove` and runs `tests/integration.sh`. On Windows, `make test` builds `grove.exe` and checks `--version`. The build refuses a wrapped allocation size, checks format strings, and links with a guarded stack and, on Linux, full RELRO. Grove's own C follows the SEI CERT C Coding Standard: a conversion reports failure, a size that would wrap is refused, and a file is measured with `fstat` on the descriptor that was opened. Running `grove` does not need Python. The first build clones [WebUI](https://github.com/webui-dev/webui) and packs the page. A later run of the program does not. A Windows copy uses that packed page.

GitHub Actions builds that same file on every push. Each artifact is one platform:

- `grove-linux-x86_64` is the Linux x86_64 executable.
- `grove-macos-arm64` or `grove-macos-x86_64` is the macOS executable for that Mac.
- `grove-windows-x86_64` is the Windows x86_64 executable, named `grove.exe`.

`PLATFORM.txt` in the artifact names that platform. A command does not need a browser. The window does.

## Index

```sh
grove index ~/notes
grove search '"full text"'
```

The index lives in `$GROVE_INDEX`, or `$XDG_DATA_HOME/grove`, or `~/.local/share/grove`. On Windows, with `GROVE_INDEX` unset, it lives in `%LOCALAPPDATA%\grove`. `--index DIR` points at another directory. That directory holds `grove.db`. Move it with the binary. An index from before this format has to be removed and indexed again.

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

`grove compact` rebuilds the full-text index and vacuums the database. `grove status` prints roots, document count, and the size of the stored text. `grove ui` opens the window in this same program. With no command, `grove` does that. A command does not open a window, and it can run while a window is already open.

`grove save` remembers a query in that index. `grove saved` lists them, newest first. `grove forget` drops one. The same list is what the window shows. An older index gains the list on the first save. It does not need to be indexed again.

`grove note` keeps one line of text on a place, the path and the byte offset of a passage. `grove notes` lists them. `grove forget-note` drops one. The same notes are what the window shows under a passage.

`grove watch` remembers a query and the files that match it then. `grove alerts` lists files that are new, or whose size or time changed, since that watch. Watching the same query again marks those files seen. `grove unwatch` stops the watch and leaves the saved query. `grove forget` drops both. An older index gains this on the first watch.

`grove fetch` saves a web page into the index and indexes its text. The same address again replaces that page. Only http and https. The copy is kept under the index. The record's identifier starts as that address, and a later edit of the identifier is kept. Fetching uses the curl program. Search does not.

## Window

```sh
./grove
grove ui
```

`grove` with no command opens the window. `grove ui` does the same. `grove status`, `grove index PATH`, and `grove search` do not open a window. They use the one index.

With no command, the window opens the installed browser on this same index and the same query language. Stemming, Fuzzy, and Concepts sit under the box. An info mark in the box shows how a query is written. Stemming starts on. A related word is marked in blue. A watch stays an exact query. Two passages are listed under each file. Show more continues through the rest of that file. A click on the file name opens it with the system. A fetched page opens at its address. The list shows a few sentences around the matched words. Snippet can stay that nearby span, show one sentence, or show the passage. Narrow refines a search by language, author, form, period, and genre. On a wide window those choices sit beside the hits and scroll with the page. The hits run beside that column and use the full width under it. On a narrow window they are under Narrow. A click on a passage opens that place in its own tab and leaves the search where it is. Choosing the tab reads the extracted text, the chapter, and the page when the file recorded one, and marks that same sentence. Selecting text there offers Copy, Copy citation, and Save excerpt. A saved passage stays underlined. Further passages open as more tabs. On a wide window those titles stack on the right, above the contents. On a narrow window they stay in the top row. Earlier and later move through that text. The original file is still opened from the reader. `ext:` and `path:` in the query show up as chips; removing a chip searches again. The file types, folders, and months in the whole match sit beside those chips, each with a file count. Choosing one adds that chip. `title:` stays in the query. Narrow opens over the results when a record has a language, creator, type, coverage, or subject. Several names in one group match any of them. The groups combine. Clear drops those five and leaves the other chips. Save keeps the query that is in the box. Remembered queries sit under the help line. Choosing one searches again, and × forgets it. Note, under a passage, keeps a line of text on that place. × removes it. The passage itself still opens in the reader. Watch, under the help line, keeps that query and the files it matches. A count on a remembered query is how many of those files are new or changed. Choosing the count marks them seen. Unwatch leaves the remembered query.

Search, Library, Personas, and Excerpts are tabs at the top of the window. The page widens with the window and stays centered. On a wide window, results and excerpts run in more than one column, and a book's contents sit beside the text. The contents and any open books scroll with the page. The text runs beside them and widens once they end. A mark at the lower right returns to the top. Excerpts lists passages saved from the reader, grouped by book, with the wording, the place, and a citation. A plus on a passage opens the tag field. A tag filters that list. Open returns to the passage and marks it. Copy all, Text, and JSON take the whole collection. The wording is kept in this window. The books stay in the library. Library does not slide the page aside. On a wide window, open books stack on the right, above that book's contents, and the page sits beside them. Library is a table of the Dublin Core record: title, creator, date, language, type, subject, coverage, and source. Title and creator keep a wide column. The rest of the row stays on one line, and the table scrolls sideways. The shelf opens grouped by creator. A file named Title — Author shows that author until the record has one. Sort any column. The title opens the volume. Identifier and the rest of the record stay in Edit. At most 300 volumes are shown, the first of that order. Add a folder or a file from the desktop chooser, or a web page by its address. The first time Library opens, grove prepares a list of titles and paths; search waits while that finishes. Edit opens the fifteen Dublin Core elements in a dialog. They describe the volume. The text stays in the file. Remove takes the volume off the shelf and leaves the file on disk. Indexing its folder puts it back. Those fields are what a search can narrow by. Author is the creator, form the type, period the coverage, and genre the subject. Personas lists the people on the shelf and the volumes they touch. Author is the creator. Contributor holds any other role as Role: Name, such as Translator, Editor, or Speaker. A source that begins Translated by, Edited by, or Read by counts too. Choosing a name in the library opens that persona. That page fetches a short public profile, including a photograph when Wikipedia has one, and lists other works attributed to the person.

The window is WebUI, linked into this same file. It needs a browser at runtime. `--index DIR` selects the index. Firefox opens with the profile Grove, stored in `~/.local/share/grove/firefox`, so another program can keep the WebUI profile. On Linux the window shows Grove's icon when X11 is present. A command does not load X11.

Saving `ui/page.html` reloads the open window when that file is beside the program. Saving `ui/main.c` rebuilds that program and swaps it in. The process and the index stay up, and the query in the box is kept. A compile error keeps the previous program and prints the compiler output. That is the same split as CToy: the host stays, and the program is replaced when a file is saved. A copy that cannot see those files uses the page packed at build time, which is already inside the executable. `$GROVE_PAGE` names a different page file. Changes under `src/` or in `ui/host.c` still need `make` and a new process.

## Samples

`samples/books` is a small shelf of public-domain EPUBs for trying search outside the synthetic tests. `samples/SOURCES.md` lists each file, its Project Gutenberg number, and where it came from. Better Gutenberg packs those Gutenberg texts. The Loebolus PDFs checked while gathering this shelf are scanned page images; grove does not OCR, so they are not included.

```sh
./grove --index samples/index index samples/books
./grove --index samples/index
```

## Query

An apostrophe inside a word is not a break. `Don't` is the token `dont`, and the passage marks the whole word, including the apostrophe. Precomposed letters in the Greek Extended block are letters, and capitals there match the small letter. A shelf indexed before those letters were words needs to be indexed again before they match.

Words in a query are combined with AND. `OR` and `NOT` are operators only in uppercase, so `or` is an ordinary word. `-term` excludes a term. Parentheses group. Quotes are a phrase: `"full text"` does not match `full-blown text`. A trailing `*` is a prefix. A trailing `~` allows one edit: a deletion, an adjacent swap, a substitution, or an insertion. `grove search` stays exact unless `--stem`, `--fuzzy`, or `--concepts` is set. Stemming also tries inflections, such as `runs` and `running`, and the Porter stem when that stem is itself a word. Fuzzy allows one edit on a word of five letters or more. Concepts adds a short classical family, such as `love` and `eros`, then the closest WordNet synonyms, twelve related words at most. The list is `share/wordnet-syn.txt`, or `$GROVE_WORDNET`. A missing file leaves the families in place. A related word is marked with `{{` and `}}`. The window shows the same three switches, and Stemming starts on. A watch stays exact. `ext:`, `path:`, and `title:` filter by extension, the path relative to the indexed root, and the extracted title. `mtime:2026-10` keeps files last touched in that month. `language:`, `author:`, `form:`, `period:`, and `genre:` narrow by the record. A creator or a language may list several names separated by semicolons. Repeating one matches any of the values. The five combine. The window lists those months with the type chips. The current month is labeled This month. Filters accept quotes (`ext:".md"`, `title:"Gamma Note"`).

```sh
grove search 'text -blown'
grove search 'hello OR gamma ext:txt'
grove search '東京駅'
grove search --limit 10 --json 'port*'
```

Hits are ordered by score, then path. The score is a percent of the best hit. Each hit carries the first eight passages, in order through the file. `more` in the JSON is true when further passages remain. A passage is the paragraph around the match. A long paragraph is cut to the sentences around the match, and the cut falls on a sentence boundary. A quoted phrase is one passage at that phrase, rather than a passage for each of its words. The passage marks an exact match with `<<` and `>>`, and a related word with `{{` and `}}`, and gives the byte offset of that match in the extracted text. For a plain text file that offset is a file offset. The passage also names the section and the page when the file has them. A heading comes from Markdown, AsciiDoc, HTML, EPUB, Word, OpenDocument, or a PDF outline. A page comes from a form feed, a Word or OpenDocument page break, or an EPUB page break. grove does not invent either one. For a PDF, `page` is 1-based. grove writes a form feed when the next text belongs to another page. Streams listed together on one page stay on that page. With no page catalog, each text stream is its own page. A format with no page marks uses `page` 0. `--json` is an object: `hits`, `nfiles`, and `facets`. `nfiles` and the facet counts cover every matching file, including files past `--limit`. A type is the extension. A folder is the parent of the path relative to the indexed root, with a trailing slash. `facets.narrow` counts language, author, form, period, and genre across those same files. A count ignores that group's own selection. The hit list itself is still capped by `--limit`. Each hit adds `nmatches` and `offsets`, the earliest 2048 FTS4 `offsets()` tuples as `{start, length, term, column}`. Those tuples are one per token, in file order. A quoted phrase is several offsets, and `term` follows the query tokens. `nmatches` counts every tuple, including ones past the 2048 that are kept. Passages still group a quoted phrase into one passage. `matchinfo` is the parsed `matchinfo('pcnalx')` blob: `p`, `c`, `n`, `avg`, `len`, and `phrases` of `{hits, total, docs}`. Ranking reads those counts, so a different rank can be coded from the same blob. The human-readable listing stays the passages. It also adds `passages`, a list of `{text, offset, page, section}`. `snippet` is the text of the first passage.

## PDF

PDF text is whatever is encoded in the file. grove reads literal strings as WinAnsi / CP1252, UTF-16BE strings, and ToUnicode `beginbfchar` / `beginbfrange` maps, and it inflates FlateDecode streams. The stream keyword may sit against `>>` with no space. A line break still has to follow it. Each font keeps its own ToUnicode map. It does not run OCR, and it does not decode every font or compression filter. A scan with no text layer indexes as empty and is skipped. A PDF with no title metadata uses the opening line of the first page. Page numbers come from the page's content streams. A PDF indexed before form feeds were stored needs to be indexed again. An outline entry becomes the section when its destination points at a page object grove already counted. grove does not infer headings from font size, and it does not read the page tree.
