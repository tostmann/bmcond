// SPDX-License-Identifier: GPL-2.0-or-later
/* copro_diag — Standalone Co-CPU-Inventarisierungs-Tool.
 *
 * Stage 2 Iter 2 Live-Verifikation.  Spricht direkt mit einem Modul
 * (USB, UART oder TCP), führt full-inventory durch:
 *
 *   1. open transport
 *   2. radio_dualcopro_boot_to_app — bringt Modul in App-Mode
 *   3. copro_query_identify        → ASCII-Tag
 *   4. copro_query_sgtin           → 12 Bytes
 *   5. copro_query_app_version     → 3 Bytes (App-Mode)
 *
 * Optional --switch-bl bonus-step:
 *   6. copro_start_bootloader      → wechselt auf BL
 *   7. copro_query_bl_version      → 3 Bytes (BL-Mode)
 *   8. copro_start_application     → zurück nach App
 *
 * ⚠ Tool ist OFFLINE: braucht exklusiven Zugriff aufs Modul.  Wenn
 * bmcond/rfd den Stick bereits halten, schlägt transport_open fehl
 * (USB-libusb-claim) bzw. UART-O_EXCL-busy.
 *
 * Bauen:  make tools/copro_diag  → bin/copro_diag
 *
 * Beispiele:
 *   bin/copro_diag usb=1b1f:c020              # HmIP-RFUSB
 *   bin/copro_diag usb=0403:6f70 --switch-bl  # HB-RF-USB v1, full sweep
 *   bin/copro_diag uart=/dev/ttyUSB0
 *   bin/copro_diag tcp=10.10.11.28:2327
 *
 * Lizenz: GPL-2.0-or-later
 */

#include "copro_query.h"
#include "eq3_image.h"
#include "frame.h"
#include "radio_dualcopro.h"
#include "transport.h"
#include "transport_usb.h"
#include "transport_uart.h"
#include "transport_tcp.h"
#include "transport_eth.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_hex(const char *label, const uint8_t *b, size_t n)
{
    printf("  %s = ", label);
    for (size_t i = 0; i < n; ++i) printf("%02X", b[i]);
    printf("\n");
}

static void print_version(const char *label, const uint8_t v[3])
{
    printf("  %s = %u.%u.%u\n", label, v[0], v[1], v[2]);
}

static struct transport *open_spec(const char *spec)
{
    struct transport *t = NULL;
    if (strncmp(spec, "usb=", 4) == 0) {
        t = transport_usb_new_str(spec + 4, 115200);
    } else if (strncmp(spec, "uart=", 5) == 0) {
        t = transport_uart_new(spec + 5, 115200);
    } else if (strncmp(spec, "tcp=", 4) == 0) {
        t = transport_tcp_new(spec + 4);
    } else if (strncmp(spec, "eth=", 4) == 0) {
        t = transport_eth_new(spec + 4);
    } else if (strncmp(spec, "rfnethm=", 8) == 0) {
        t = transport_rfnethm_new(spec + 8);
    } else {
        fprintf(stderr,
                "transport-spec: usb=VID:PID, uart=/path, tcp=host:port, "
                "eth=host:port, rfnethm=host\n");
        return NULL;
    }
    if (!t) { fprintf(stderr, "transport-construct FAIL: %s\n", strerror(errno)); return NULL; }
    if (transport_open(t) < 0) {
        fprintf(stderr, "transport_open FAIL: %s\n", strerror(errno));
        transport_free(t);
        return NULL;
    }
    return t;
}

/* --inspect: nur das .eq3-File parsen + Stats drucken, kein USB. */
static int cmd_inspect(const char *path)
{
    eq3_image_t img;
    if (eq3_image_load(path, &img) < 0) {
        fprintf(stderr, "load FAIL: %s (%s)\n", path, strerror(errno));
        return 1;
    }
    printf("══ inspect %s ══\n", path);
    printf("  expected_version = ");
    if (img.version_known)
        printf("%u.%u.%u\n",
               img.expected_version[0],
               img.expected_version[1],
               img.expected_version[2]);
    else
        printf("(unknown — filename pattern fehlt)\n");
    printf("  chunk_size       = %u bytes (= LEN_BE der ersten 2 file-bytes)\n",
           img.chunk_size);
    printf("  n_chunks         = %zu\n", img.n_frames);
    if (img.n_frames > 0) {
        const eq3_frame_t *f0 = &img.frames[0];
        printf("  first-chunk: len=%zu  data[0..8]=", f0->len);
        for (size_t i = 0; i < 8 && i < f0->len; ++i) printf("%02x", f0->bytes[i]);
        printf("\n");
    }
    if (img.n_frames > 1) {
        const eq3_frame_t *fn = &img.frames[img.n_frames - 1];
        printf("  last-chunk:  len=%zu  data[0..8]=", fn->len);
        for (size_t i = 0; i < 8 && i < fn->len; ++i) printf("%02x", fn->bytes[i]);
        if (fn->len != (size_t)(img.chunk_size - 2))
            printf("  (kürzer als chunk_size — last-chunk-tail)");
        printf("\n");
    }
    printf("══ done ══\n");
    eq3_image_free(&img);
    return 0;
}

static void flash_progress(void *ctx, size_t idx, size_t n)
{
    (void)ctx;
    if (idx == n) {
        fprintf(stderr, "  flash: all %zu frames sent\n", n);
    } else if (n > 0 && idx % 32 == 0) {
        fprintf(stderr, "  flash: %zu/%zu (%.0f%%)\n",
                idx, n, 100.0 * (double)idx / (double)n);
    }
}

int main(int argc, char **argv)
{
    int do_switch_bl = 0;
    int do_inspect   = 0;
    int do_flash     = 0;
    int force_flash  = 0;
    const char *flash_path = NULL;
    const char *spec = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--switch-bl") == 0) {
            do_switch_bl = 1;
        } else if (strcmp(argv[i], "--inspect") == 0 && i + 1 < argc) {
            do_inspect = 1; flash_path = argv[++i];
        } else if (strcmp(argv[i], "--flash") == 0 && i + 1 < argc) {
            do_flash = 1; flash_path = argv[++i];
        } else if (strcmp(argv[i], "--force") == 0) {
            force_flash = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown opt %s\n", argv[i]);
            return 2;
        } else {
            spec = argv[i];
        }
    }

    /* --inspect ist offline-only, kein transport. */
    if (do_inspect && !do_flash) {
        return cmd_inspect(flash_path);
    }

    if (!spec) {
        fprintf(stderr,
            "Usage:\n"
            "  copro_diag <spec>                          # read-only inventory\n"
            "  copro_diag <spec> --switch-bl              # + App→BL→App roundtrip\n"
            "  copro_diag <spec> --flash <file.eq3>       # flash firmware (skip if equal)\n"
            "  copro_diag <spec> --flash <file.eq3> --force  # flash even if equal\n"
            "  copro_diag --inspect <file.eq3>            # offline file inspection\n"
            "\n  spec = usb=VID:PID | uart=/path | tcp=host:port\n");
        return 2;
    }

    struct transport *t = open_spec(spec);
    if (!t) return 1;

    printf("══ copro_diag  spec=%s  fd=%d ══\n", spec, t->fd);

    char app_tag[32] = {0};
    char tag[32] = {0};
    uint8_t sgtin[12] = {0};
    uint8_t app_v[3]  = {0};
    uint8_t bl_v[3]   = {0};

    /* Iter-1+2-Inventory braucht App-Mode.  Iter-3-Flash braucht BL —
     * nach transport_open's reset_pulse ist Modul im BL.  Boot_to_app
     * würde es zu App switchen → CHANGE_APP nötig → für legacy Co_CPU
     * via OS-Layer-Fallback.  Bei `--flash` lassen wir Modul im BL
     * (skip boot_to_app) und bringen's nach Flash-Loop via
     * boot_to_app zurück. */
    if (!do_flash) {
        if (radio_dualcopro_boot_to_app(t->fd, app_tag, sizeof(app_tag)) < 0) {
            fprintf(stderr, "boot_to_app FAIL — Modul stumm oder Mode-Switch failed\n");
            transport_close(t); transport_free(t);
            return 1;
        }
        printf("  boot_to_app ok, app_tag='%s'\n", app_tag);

        /* ─── Read-only Iter-1-Probes ─────────────────────────────── */
        if (copro_query_identify(t->fd, tag, sizeof(tag), 0) == 0)
            printf("  identify     = '%s'  mode=%s\n", tag,
                   copro_classify_tag(tag) == COPRO_MODE_APPLICATION ? "App" :
                   copro_classify_tag(tag) == COPRO_MODE_BOOTLOADER  ? "BL"  : "?");

        if (copro_query_sgtin(t->fd, sgtin, 0) == 0)
            print_hex("sgtin       ", sgtin, 12);

        if (copro_query_app_version(t->fd, app_v, 0) == 0)
            print_version("app_version ", app_v);

        /* Hardware-Type via TRXAdapter cmd=0x09 (getMCUType).
         * Co_CPU-Familie supportet das nicht — fallback: Tag-Klassifikation. */
        copro_hw_type_t hw = COPRO_HW_UNKNOWN;
        if (strstr(app_tag, "Co_CPU_App")) {
            hw = COPRO_HW_HM_MOD_RPI_PCB;
        } else if (copro_query_mcu_type(t->fd, &hw, 0) < 0) {
            hw = COPRO_HW_UNKNOWN;
        }
        char sgtin_hex[28] = {0};
        for (int i = 0; i < 12; ++i) sprintf(sgtin_hex + i*2, "%02X", sgtin[i]);
        printf("  hw_type      = %s  (enum=%d)\n",
               copro_hw_type_name(hw, sgtin_hex), (int)hw);
    }

    /* ─── Optional Iter-2-Probes — App→BL→read BL-Version→App ─────── */
    if (do_switch_bl) {
        printf("\n── --switch-bl: temporärer App→BL→App roundtrip ──\n");
        char new_tag[32] = {0};
        if (copro_start_bootloader(t->fd, new_tag, sizeof(new_tag), 0) < 0) {
            fprintf(stderr, "  start_bootloader FAIL\n");
        } else {
            printf("  → in BL: '%s'\n", new_tag);
            if (copro_query_bl_version(t->fd, bl_v, 0) == 0)
                print_version("bl_version  ", bl_v);
        }
        if (copro_start_application(t->fd, new_tag, sizeof(new_tag), 0) < 0) {
            fprintf(stderr, "  start_application FAIL — Modul ggf. im BL stuck!\n");
            fprintf(stderr, "  Recovery: HW-Reset (USB-replug oder hb_rf_eth-cmd-4)\n");
        } else {
            printf("  → in App: '%s'\n", new_tag);
        }
    }

    /* ─── Optional Iter-3 Flash ─────────────────────────────────── */
    if (do_flash) {
        printf("\n── --flash %s%s ──\n", flash_path,
               force_flash ? "  (--force)" : "");

        /* Settle nach reset_pulse — Modul braucht ~500ms bis BL ready ist
         * (verifiziert empirisch: BL_VERSION-probe direkt nach reset
         * liefert status=BUSY, post-flash-ACKs sind dann auch ERROR).
         * eq3configcmd hat vermutlich eigenes settle drinn. */
        usleep(500 * 1000);

        eq3_image_t img;
        if (eq3_image_load(flash_path, &img) < 0) {
            fprintf(stderr, "load FAIL: %s\n", strerror(errno));
            transport_close(t); transport_free(t);
            return 1;
        }
        printf("  loaded: %zu chunks (chunk_size=%u)", img.n_frames, img.chunk_size);
        if (img.version_known)
            printf(", expected v%u.%u.%u",
                   img.expected_version[0],
                   img.expected_version[1],
                   img.expected_version[2]);
        printf("\n");

        /* Identify aktuellen Mode.  Wenn Modul schon im BL (nach reset_pulse
         * von transport_open zb. bei USB- oder hb_rf_eth-Box mit echter
         * RST-Pin-Beschaltung): assume_in_bl=1 → flash direkt.
         * Sonst (z.B. RFNETHM ohne RST-Pin-Mapping): start_bootloader via
         * COMMON cmd=0x02 (DualCoPro-Familie) lassen wir copro_flash_image
         * intern machen — assume_in_bl=0. */
        char ident_tag[32] = {0};
        copro_query_identify(t->fd, ident_tag, sizeof(ident_tag), 0);
        copro_mode_t cur_mode = copro_classify_tag(ident_tag);
        int already_bl = (cur_mode == COPRO_MODE_BOOTLOADER);
        printf("  pre-flash mode = %s ('%s') → %s\n",
               cur_mode == COPRO_MODE_APPLICATION ? "App" :
               cur_mode == COPRO_MODE_BOOTLOADER  ? "BL"  : "?",
               ident_tag,
               already_bl ? "assume_in_bl=1 (skip start_bootloader)" :
                            "assume_in_bl=0 (will COMMON cmd=0x02 to enter BL)");
        copro_flash_opts_t opts = {
            .force_overwrite     = force_flash,
            .per_frame_timeout_ms = 4000,
            .on_progress         = flash_progress,
            .assume_in_bl        = already_bl,
            .skip_app_after      = 1,
        };
        char err[128] = {0};
        copro_flash_result_t r = copro_flash_image(t->fd, &img, &opts,
                                                   err, sizeof(err));
        printf("  result: %s%s%s\n",
               copro_flash_result_name(r),
               err[0] ? " — " : "",
               err[0] ? err : "");
        eq3_image_free(&img);
        if (r != COPRO_FLASH_OK && r != COPRO_FLASH_SKIPPED_VERSION_MATCH) {
            transport_close(t); transport_free(t);
            return 1;
        }

        /* Re-boot to App via multi-protocol fallback. */
        printf("\n── Post-flash: bring module back to App ──\n");
        if (radio_dualcopro_boot_to_app(t->fd, app_tag, sizeof(app_tag)) < 0) {
            fprintf(stderr, "  WARN: boot_to_app post-flash failed\n");
        } else {
            printf("  app_tag='%s'\n", app_tag);
            if (copro_query_app_version(t->fd, app_v, 0) == 0)
                print_version("  new_app_version", app_v);
        }
    }

    transport_close(t);
    transport_free(t);
    printf("══ done ══\n");
    return 0;
}
