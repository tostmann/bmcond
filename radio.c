// SPDX-License-Identifier: GPL-2.0-or-later
/* radio.c — Registry der radio_ops. Linker-time-Anker für alle
 * radio-Implementierungen. Neue radios werden hier eingetragen. */

#include "radio.h"
#include <string.h>
#include <stddef.h>

extern const radio_ops_t radio_dualcopro;
/* Zukünftig:
 * extern const radio_ops_t radio_hmuartlgw;
 * extern const radio_ops_t radio_hmw;
 * extern const radio_ops_t radio_culfw;
 * extern const radio_ops_t radio_cul32;
 * extern const radio_ops_t radio_hmlgw2;
 */

static const radio_ops_t * const radio_registry[] = {
    &radio_dualcopro,
    /* &radio_hmuartlgw, ... */
};

const radio_ops_t *radio_ops_by_name(const char *name)
{
    if (!name) return NULL;
    size_t n = sizeof(radio_registry) / sizeof(radio_registry[0]);
    for (size_t i = 0; i < n; ++i) {
        if (radio_registry[i] && radio_registry[i]->name
            && !strcmp(radio_registry[i]->name, name))
            return radio_registry[i];
    }
    return NULL;
}

const radio_ops_t * const *radio_ops_all(size_t *out_count)
{
    if (out_count) *out_count = sizeof(radio_registry) / sizeof(radio_registry[0]);
    return radio_registry;
}
