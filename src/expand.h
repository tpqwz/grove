#ifndef GROVE_EXPAND_H
#define GROVE_EXPAND_H

#include "text.h"

#include <stddef.h>

/* One alternate spelling. kin is 1 when the word came only from the concept
 * list. The caller frees each s. */
typedef struct WordForm {
    char *s;
    size_t len;
    int kin;
} WordForm;

/* Porter stem of an [a-z] word of at least 3 letters. Writes a NUL-terminated
 * stem into out, which must hold len + 1 bytes. Returns 0 when the word is
 * not stemmed. */
size_t porter_stem(const char *word, size_t len, char *out);

/* Original word, then inflections, concept relatives, and one-edit variants.
 * *out is NULL and *n is 0 when the word is unchanged. Returns -1 on
 * allocation failure. tilde is the explicit word~ marker. modes NULL is off. */
int expand_word(const char *word, size_t len, int tilde, const SearchModes *modes, WordForm **out, int *n);
void wordforms_free(WordForm *f, int n);

#endif
