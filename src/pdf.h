#ifndef GROVE_PDF_H
#define GROVE_PDF_H

#include "util.h"

#include <stddef.h>

/* Pulls digital text out of a PDF. Scanned pages yield an empty buffer. */
int pdf_extract(const unsigned char *bytes, size_t n, Buf *text, char **title, Err *e);

/* Outline entries whose destination resolves to a page object. page is
 * 1-based in file order of /Type /Page, which matches one stream per page. */
typedef struct PdfSection {
    uint32_t page;
    uint8_t level;
    char *title;
} PdfSection;

int pdf_sections(const unsigned char *bytes, size_t n, PdfSection **out, int *nout);
void pdf_sections_free(PdfSection *s, int n);

#endif
