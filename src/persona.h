#ifndef GROVE_PERSONA_H
#define GROVE_PERSONA_H

#include "util.h"

/* A public profile for one shelf name. Augustine and Augustine of Hippo stay
 * two people. Aliases are other names from the public record, not a merge. */
enum { PERSONA_NONE = 0, PERSONA_READY = 1, PERSONA_UNAVAILABLE = 2 };
enum { PERSONA_FIND = 1, PERSONA_ENTITY = 2, PERSONA_SUMMARY = 3, PERSONA_SPARQL = 4 };

typedef struct PersonaPubWork {
    char *title;
    char *year;
    char *url;
} PersonaPubWork;

typedef struct PersonaProfile {
    int state;
    char *qid;
    char *description;
    char *summary;
    char *born;
    char *died;
    char *occupation;
    char *wikipedia;
    char *photo_url;
    char **aliases;
    int alias_n;
    PersonaPubWork *works;
    int work_n;
    unsigned char *photo;
    size_t photo_n;
    char *photo_type;
} PersonaProfile;

void persona_profile_free(PersonaProfile *p);

/* Fixed-host https address. arg is the name, Q-id, or Wikipedia title.
 * The caller frees *url. A name cannot change the host. */
int persona_lookup_url(int kind, const char *arg, char **url, Err *e);

/* Builds a profile from response bodies. Any body may be NULL. Does not use
 * the network. A thumbnail stays only as a file on upload.wikimedia.org.
 * The summary host thumb.wikimedia.org is rewritten onto that path. */
int persona_profile_parse(const char *name, const char *search, size_t search_n, const char *entity,
                          size_t entity_n, const char *summary, size_t summary_n, const char *sparql,
                          size_t sparql_n, PersonaProfile *out, Err *e);

/* Cache under index_dir/personas, named by the folded name, not by the name
 * as a path. Missing is PERSONA_NONE and a zero return. */
int persona_profile_store(const char *index_dir, const char *name, const PersonaProfile *in, Err *e);
int persona_profile_load(const char *index_dir, const char *name, PersonaProfile *out, Err *e);

/* Cache, or a public lookup. Does not write the library database. */
int persona_profile(const char *index_dir, const char *name, PersonaProfile *out, Err *e);

#endif
