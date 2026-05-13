// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BMCOND_SOURCES_H
#define BMCOND_SOURCES_H

/* Persistence + validation for bmcond.sources.json — the schema documented
 * in docs/sources_schema.md.  See that file for the field semantics; this
 * header is the C-level mirror of the JSON shape. */

#include <stdbool.h>
#include <stddef.h>

#define BMCOND_SOURCES_SCHEMA_VERSION  1

#define BMCOND_SOURCE_ID_MAX           33   /* 32 chars + NUL */
#define BMCOND_SOURCE_TRANSPORT_MAX    256
#define BMCOND_SOURCE_LABEL_MAX        65   /* 64 chars + NUL */
#define BMCOND_SOURCE_NOTES_MAX        257  /* 256 chars + NUL */
#define BMCOND_SOURCE_DISCO_MAX        16
#define BMCOND_SOURCES_MAX             16

struct bmcond_source {
    char id[BMCOND_SOURCE_ID_MAX];
    char transport[BMCOND_SOURCE_TRANSPORT_MAX];
    char label[BMCOND_SOURCE_LABEL_MAX];
    bool cap_bidcos;
    bool cap_hmip;
    bool persistent;
    char discovered_via[BMCOND_SOURCE_DISCO_MAX];   /* "libusb"|"mdns"|"manual"|"" */
    char notes[BMCOND_SOURCE_NOTES_MAX];
};

struct bmcond_slots {
    char bidcos[BMCOND_SOURCE_ID_MAX];   /* "" = null */
    char hmip[BMCOND_SOURCE_ID_MAX];     /* "" = null */
};

struct bmcond_sources_state {
    int                   schema_version;
    struct bmcond_source  sources[BMCOND_SOURCES_MAX];
    int                   n_sources;
    struct bmcond_slots   slots;
};

/* Init to empty (schema_version=1, no sources, null slots). */
void bmcond_sources_init(struct bmcond_sources_state *s);

/* Load + validate from `path`.  If file doesn't exist, fill `*out` with
 * empty state and return 0.  Parse/validate failure → -1 + err message. */
int  bmcond_sources_load(const char *path, struct bmcond_sources_state *out,
                         char *err, size_t errcap);

/* Atomic write: tmp + fsync + rename to `path`.  Validates first;
 * refuses to write invalid state. */
int  bmcond_sources_save(const char *path, const struct bmcond_sources_state *s,
                         char *err, size_t errcap);

/* Validate without persisting.  0 = ok, -1 = invalid (err filled). */
int  bmcond_sources_validate(const struct bmcond_sources_state *s,
                             char *err, size_t errcap);

/* Serialize whole state to malloc'd JSON.  Caller frees.  NULL on OOM. */
char *bmcond_sources_to_json(const struct bmcond_sources_state *s);

/* Parse a whole-document JSON (schema_version+sources+slots) into out.
 * Validates after parse. */
int  bmcond_sources_from_json(const char *json, struct bmcond_sources_state *out,
                              char *err, size_t errcap);

/* Single-source ops (for the POST /<id> + DELETE /<id> path). */
char *bmcond_source_to_json(const struct bmcond_source *src);
int   bmcond_source_from_json(const char *json, struct bmcond_source *out,
                              char *err, size_t errcap);

/* Set slots; pass NULL or empty string for null.  Validates against current
 * sources[] (id must exist + must have the requested capability). */
int   bmcond_sources_set_slots(struct bmcond_sources_state *s,
                               const char *bidcos_id_or_null,
                               const char *hmip_id_or_null,
                               char *err, size_t errcap);

/* Insert or replace a source by id.  -1 if state full or id invalid. */
int   bmcond_sources_upsert(struct bmcond_sources_state *s,
                            const struct bmcond_source *src,
                            char *err, size_t errcap);

/* Remove a source by id.  Auto-clears any slot referencing it; the
 * cleared slot names (comma-separated, e.g. "bidcos,hmip" or "") are
 * written into `cleared`.  -1 if id not found. */
int   bmcond_sources_remove(struct bmcond_sources_state *s, const char *id,
                            char *cleared, size_t cleared_cap,
                            char *err, size_t errcap);

const struct bmcond_source *bmcond_sources_find(
    const struct bmcond_sources_state *s, const char *id);

#endif
