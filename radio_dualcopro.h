// SPDX-License-Identifier: GPL-2.0-or-later
/* radio_dualcopro.h — Public API der DualCoPro-radio-Implementierung
 *
 * Boot/Identify-Logik die früher in concentrator.c module_boot_to_app() lebte.
 * Wird von concentrator.c aufgerufen (Phase 2-Migration v0.3.0-alpha)
 * und zugleich vom radio_dualcopro_ops.boot()-vtable-Hook (Phase 3).
 *
 * Lizenz: GPL-2.0-or-later
 */

#ifndef CUL32HM_RADIO_DUALCOPRO_H
#define CUL32HM_RADIO_DUALCOPRO_H

#include <stddef.h>

/* Bringt das Modul am gegebenen fd in DualCoPro_App / Co_CPU_App Mode.
 *
 * Sequenz:
 *   1. Drain Boot-Banner (~600ms)
 *   2. COMMON_IDENTIFY-Probe (mit Fallback auf OS_GET_APP)
 *   3. Klassifiziere App-Tag: BOOT_APP / BOOT_BL / BOOT_NONE
 *   4. Falls BL: CHANGE_APP via dst=COMMON, warte auf COMMON-Push
 *   5. Drain Rest
 *
 * Falls app_tag_out != NULL: schreibt den erkannten App-Tag (z.B.
 * "DualCoPro_App", "HMIP_TRX_App") nach dort, max app_tag_cap-1 Bytes
 * + NUL.
 *
 * Returns: 0 = Modul ist im App-Mode, -1 = Boot fehlgeschlagen. */
int radio_dualcopro_boot_to_app(int fd, char *app_tag_out, size_t app_tag_cap);

#endif
