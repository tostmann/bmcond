// SPDX-License-Identifier: GPL-2.0-or-later
/* radio_id.h — minimaler Identifikations-State für ein Funkmodul.
 *
 * Hervorgegangen aus dem alten mux.h::bridge_state_t.  Seit der
 * Reduktion auf "pure userspace transport" (multimacd übernimmt
 * Mac-Layer + 3burst-Retry + LLMAC-Translate + AES) brauchen wir
 * vom alten bridge-Konzept nur noch die HW-Identitätsdaten — die
 * werden von --conf-emit zu rfd.conf + /var/run/bmcd-config.json
 * geschrieben und von der JSON-API ausgeliefert.
 *
 * Lizenz: GPL-2.0-or-later
 */

#ifndef CUL32HM_RADIO_ID_H
#define CUL32HM_RADIO_ID_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BRIDGE_MODE_BIDCOS_ONLY = 0,
    BRIDGE_MODE_DUAL = 1,
} bridge_mode_t;

typedef struct {
    bridge_mode_t mode;
    uint8_t       hmid[3];
    char          serial[11];       /* 10 ASCII + NUL */
    uint8_t       firmware[3];      /* major.minor.patch */
    uint8_t       sgtin[12];        /* SGTIN-96 binär */
    bool          identify_as_dual; /* set if CLI requested dual ident */
} bridge_state_t;

void bridge_init(bridge_state_t *st);

#endif
