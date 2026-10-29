# Sample books

Public-domain EPUBs for trying grove outside the synthetic tests in `src/test.c`. Each file says "Public domain in the USA" and names a Project Gutenberg source. Better Gutenberg repackaged them. These are not wired into `make test`.

The LibraryThing catalog at collection 997765 did not load (Cloudflare challenge, and the catalog API returned an error), so nothing was taken from that list.

Loebolus PDFs are public-domain Loeb volumes, but the two that were opened are scans. L020 Sophocles (Google Books, 455 pages) yielded 114 bytes of extractor text. L170N, Murray's Iliad (Internet Archive, 608 pages), has a broken OCR layer (`pdftotext` returns Latin gibberish for the Greek). grove does not OCR, so those PDFs are not in this folder.

| File | Work | Gutenberg | Language |
| --- | --- | --- | --- |
| `allen-as-a-man-thinketh.epub` | James Allen, *As a Man Thinketh* | 4507 | English essay |
| `shakespeare-sonnets.epub` | William Shakespeare, *Sonnets* | 1041 | English verse |
| `austen-pride-and-prejudice.epub` | Jane Austen, *Pride and Prejudice* | 1342 | English novel |
| `plato-symposium.epub` | Plato, *Symposium* | 1600 | English prose |
| `homer-odyssey.epub` | Homer, *The Odyssey* | 1728 | English verse, a little Greek in notes |
| `dumas-trois-mousquetaires.epub` | Alexandre Dumas, *Les Trois Mousquetaires* | 13951 | French novel |
| `laozi-tao-te-ching.epub` | Laozi, *The Tao Te Ching* | 216 | English prose |
| `sunzi-art-of-war.epub` | Sun Tzu, *The Art of War* | 132 | English prose |
| `budd-chinese-poems.epub` | Charles Budd, *Chinese Poems* | 37938 | English verse |

Download pages use `https://bettergutenberg.org/...` and the zip at `https://bettergutenberg.org/wp-content/uploads/2026/10/<slug>.zip`. The zip is the EPUB (`mimetype` stored first). Loebolus files, when wanted again, are `https://ryanfb.xyz/loebolus-data/L###.pdf`.

The whole Better Gutenberg shelf is one zip, `https://bettergutenberg.org/wp-content/uploads/downloads/v2/better-gutenberg-complete-library.zip` (6,728 EPUBs, about 1.8GB, revision 2026-09-08). It is public-domain text and indexes as digital text. It is not stored in this repository.

Delphi Classics lists a free collection at `https://www.delphiclassics.com/product-category/free-downloads/` (fifteen anthologies priced at $0). Taking one still goes through the shop cart, and that checkout asks for a human check, so those files are not in this folder. Individual sample EPUBs are linked in the open from `https://www.delphiclassics.com/parts-edition/` and `https://www.delphiclassics.com/sampleebooks/`.

```sh
./grove index --index samples/index samples/books
./grove-ui --index samples/index
./samples/check.sh
```
