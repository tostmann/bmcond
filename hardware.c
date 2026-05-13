// SPDX-License-Identifier: GPL-2.0-or-later
/* Hardware-Identifikation: Wrapper um IOCGDEVINFO/IOCRESET-Ioctls von
 * piVCCU's generic_raw_uart. Mit sysfs-Fallback für /dev/ttyUSB*-Pfade
 * die kein generic_raw_uart-Char-Device sind. */

#include "hardware.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <libgen.h>

const char *hw_kind_name(hw_radio_kind_t kind)
{
    switch (kind) {
    case HW_KIND_HMIP_RFUSB:    return "eQ-3 HmIP-RFUSB";
    case HW_KIND_HB_RF_USB:     return "HB-RF-USB v1 + HM-MOD-RPI-PCB";
    case HW_KIND_HB_RF_USB_2:   return "HB-RF-USB v2 + HM-MOD-RPI-PCB/RPI-RF-MOD";
    case HW_KIND_HM_MOD_RPI_PCB:return "HM-MOD-RPI-PCB (UART)";
    case HW_KIND_RPI_RF_MOD:    return "RPI-RF-MOD (UART)";
    case HW_KIND_UNKNOWN:
    default:                    return "unknown";
    }
}

/* Substring-Match auf den vom Treiber gelieferten Identifikations-String. */
static hw_radio_kind_t classify(const char *devinfo)
{
    if (strstr(devinfo, "HmIP-RFUSB"))     return HW_KIND_HMIP_RFUSB;
    if (strstr(devinfo, "HB-RF-USB-2"))    return HW_KIND_HB_RF_USB_2;
    if (strstr(devinfo, "HB-RF-USB"))      return HW_KIND_HB_RF_USB;
    if (strstr(devinfo, "RPI-RF-MOD"))     return HW_KIND_RPI_RF_MOD;
    if (strstr(devinfo, "HM-MOD-RPI-PCB")) return HW_KIND_HM_MOD_RPI_PCB;
    return HW_KIND_UNKNOWN;
}

/* sysfs-Fallback für /dev/tty<X>-Pfade die kein generic_raw_uart sind.
 * Liest /sys/class/tty/<base>/device/../{idVendor,idProduct,product} und
 * leitet kind ab. Schreibt einen Devinfo-String "<product>@<vid:pid>".
 * Returns: 0 wenn was Sinnvolles gefunden wurde, -1 sonst. */
static int identify_via_sysfs(const char *path, hw_radio_info_t *out)
{
    if (!path || !*path) return -1;
    char path_copy[256];
    snprintf(path_copy, sizeof(path_copy), "%s", path);
    const char *base = basename(path_copy);  /* "ttyUSB0" */
    if (!base || !*base) return -1;

    /* /sys/class/tty/<base>/device → symlink zum tty-subnode des USB-Interfaces
     * (z.B. /sys/.../1-2:1.0/ttyUSB0). Zwei Ebenen drüber liegt das USB-Device
     * selbst (/sys/.../1-2/) mit idVendor/idProduct/product. */
    char vid[16] = {0}, pid[16] = {0}, product[64] = {0};
    char fn[256];

    snprintf(fn, sizeof(fn), "/sys/class/tty/%s/device/../../idVendor", base);
    FILE *f = fopen(fn, "r");
    if (!f) return -1;
    if (!fgets(vid, sizeof(vid), f)) { fclose(f); return -1; }
    fclose(f);
    vid[strcspn(vid, "\r\n")] = 0;

    snprintf(fn, sizeof(fn), "/sys/class/tty/%s/device/../../idProduct", base);
    f = fopen(fn, "r");
    if (!f) return -1;
    if (!fgets(pid, sizeof(pid), f)) { fclose(f); return -1; }
    fclose(f);
    pid[strcspn(pid, "\r\n")] = 0;

    snprintf(fn, sizeof(fn), "/sys/class/tty/%s/device/../../product", base);
    f = fopen(fn, "r");
    if (f) {
        if (fgets(product, sizeof(product), f)) {
            product[strcspn(product, "\r\n")] = 0;
        }
        fclose(f);
    }

    /* Numerische Hex-Parse */
    unsigned long vid_n = strtoul(vid, NULL, 16);
    unsigned long pid_n = strtoul(pid, NULL, 16);

    if (vid_n == HW_USB_VID_FTDI && pid_n == HW_USB_PID_HB_RF_USB) {
        out->kind = HW_KIND_HB_RF_USB;
        out->supports_radio_reset = true;  /* CBUS-Bitmode */
    } else if (vid_n == HW_USB_VID_SILABS) {
        if (pid_n == HW_USB_PID_HB_RF_USB_2_A ||
            pid_n == HW_USB_PID_HB_RF_USB_2_B ||
            pid_n == HW_USB_PID_HB_RF_USB_2_C ||
            pid_n == HW_USB_PID_HB_RF_USB_2_D) {
            out->kind = HW_KIND_HB_RF_USB_2;
            out->supports_radio_reset = true;
        }
    } else if (vid_n == HW_USB_VID_EQ3 && pid_n == HW_USB_PID_HMIP_RFUSB) {
        out->kind = HW_KIND_HMIP_RFUSB;
    }

    if (out->kind == HW_KIND_UNKNOWN) return -1;

    /* Fake einen devinfo-String der dem ioctl-Format ähnelt: "<product>@<vid:pid>" */
    snprintf(out->devinfo, sizeof(out->devinfo), "%s@%s:%s",
             product[0] ? product : "USB", vid, pid);
    return 0;
}

int hw_identify(int fd, const char *path, hw_radio_info_t *out)
{
    if (!out) { errno = EINVAL; return -1; }
    memset(out, 0, sizeof(*out));

    char buf[HW_DEVINFO_LEN] = {0};
    if (ioctl(fd, HW_IOC_GET_DEVINFO, buf) < 0) {
        if (errno == ENOTTY || errno == EINVAL) {
            /* Kein generic_raw_uart-Char-Dev (z.B. ein normales /dev/ttyUSB0
             * via ftdi_sio). Sysfs-Fallback versuchen. */
            if (identify_via_sysfs(path, out) == 0) {
                /* HmIP-Stick auch via Fallback erkennen — Dual-Stack-Flag
                 * gleich wie im normalen Pfad. */
                out->dual_stack = (out->kind == HW_KIND_HMIP_RFUSB ||
                                   out->kind == HW_KIND_RPI_RF_MOD);
                return 0;
            }
            return 0;  /* nichts gefunden, kind bleibt UNKNOWN */
        }
        return -1;
    }
    buf[HW_DEVINFO_LEN - 1] = 0;
    memcpy(out->devinfo, buf, HW_DEVINFO_LEN);

    out->kind = classify(out->devinfo);

    /* Reset-Capability hängt vom Adapter ab:
     *   - HmIP-RFUSB: CP2102 ohne GPIO-Latch → kein HW-Reset.
     *   - HB-RF-USB v1 (FTDI CBUS): Reset via CBUS-Bitmode.
     *   - HB-RF-USB v2 (CP2102N): Reset via Latch-Vendor-Cmd.
     *   - HM-MOD-RPI-PCB / RPI-RF-MOD am UART: Reset via GPIO am Pi (anderer
     *     Pfad — durch transport_uart_reset_via_gpio). */
    switch (out->kind) {
    case HW_KIND_HB_RF_USB:
    case HW_KIND_HB_RF_USB_2:
        out->supports_radio_reset = true;
        break;
    case HW_KIND_HMIP_RFUSB:
    case HW_KIND_HM_MOD_RPI_PCB:
    case HW_KIND_RPI_RF_MOD:
    case HW_KIND_UNKNOWN:
    default:
        out->supports_radio_reset = false;
        break;
    }

    /* Aktuell verifiziert: HmIP-RFUSB liefert sowohl BidCoS- als auch
     * HmIP-Frames über DualCoPro (dst=HMIP wird vom Modul abgesetzt).
     * Die HB-RF-USB-Adapter sind transparent — was sie liefern hängt vom
     * draufgesteckten Modul ab; HM-MOD-RPI-PCB ist BidCoS-only, RPI-RF-MOD
     * kann beides. Konservativ markieren wir nur die zweifellos-dual Sticks. */
    out->dual_stack = (out->kind == HW_KIND_HMIP_RFUSB ||
                       out->kind == HW_KIND_RPI_RF_MOD);

    return 0;
}

int hw_reset_radio(int fd)
{
    if (ioctl(fd, HW_IOC_RESET_RADIO) < 0) return -1;
    return 0;
}
