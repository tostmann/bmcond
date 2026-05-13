// SPDX-License-Identifier: GPL-2.0-or-later
/* Hardware-Identifikation der angeschlossenen HM-Funk-Module
 *
 * piVCCU's generic_raw_uart-Kernelmodul exportiert Char-Devices wie
 * /dev/raw-uart und beantwortet zwei Ioctls die wir nutzen:
 *
 *   IOCTL_IOCGDEVINFO   — liefert "<product>@usb-<bus>-<devpath>" aus
 *                         dem USB-Descriptor (oder einen Plattform-String
 *                         wenn der Treiber bcm283x_raw_uart / pl011_raw_uart
 *                         ist und der Stick direkt am Pi-UART hängt).
 *   IOCTL_IOCRESET_RADIO_MODULE — kickt das Funkmodul (USB-bus DTR/RST oder
 *                         GPIO am Adapter), wir brauchen es als Replacement
 *                         für den GPIO-Reset-Pfad bei UART-direkt-Sticks.
 *
 * Die USB-VID/PID-Tabelle hier ist redundant zu dem was die piVCCU-Treiber
 * tun (sie matchen selbst auf VID/PID), aber sie ist dokumentarischer
 * Ankerpunkt: damit wir wissen welches Hardware unsere Codepfade adressieren
 * sollen, ohne in die Kernelmodul-Sourcen springen zu müssen.
 *
 * Lizenz: GPL-2.0-or-later
 */

#ifndef CUL32HM_HARDWARE_H
#define CUL32HM_HARDWARE_H

#include <stdint.h>
#include <stdbool.h>

/* USB-Identifikatoren aller HM-Funk-Sticks die wir am userspace-Ende
 * gegen unseren Concentrator betreiben können. Quelle: piVCCU-Sourcen
 * (refs/piVCCU/hb_rf_usb*.c) plus Live-Inspektion am Pi5 (lsusb 2026-05-03). */

#define HW_USB_VID_FTDI            0x0403
#define HW_USB_VID_SILABS          0x10c4
#define HW_USB_VID_EQ3             0x1b1f

/* HB-RF-USB v1 — FTDI-FT231X-Adapter (Reinert), nimmt HM-MOD-RPI-PCB auf.
 *   Treiber: hb_rf_usb (piVCCU). Achtung: ftdi_sio greift häufig zuerst zu;
 *   dann landet er als /dev/ttyUSB0, NICHT als /dev/raw-uart. */
#define HW_USB_PID_HB_RF_USB       0x6f70

/* HB-RF-USB v2 — CP2102N-basierter Adapter (Reinert), nimmt HM-MOD-RPI-PCB
 * oder RPI-RF-MOD auf. Mehrere PIDs (Charge-abhängig); ECDSA-locked außer
 * der ELV-Variante. Treiber: hb_rf_usb_2 (piVCCU). */
#define HW_USB_PID_HB_RF_USB_2_A   0x8c07
#define HW_USB_PID_HB_RF_USB_2_B   0x8d81
#define HW_USB_PID_HB_RF_USB_2_C   0x8d91
#define HW_USB_PID_HB_RF_USB_2_D   0x8e4a

/* eQ-3 HmIP-RFUSB — offizieller eQ-3-Stick mit integriertem Funkmodul.
 *   USB-Class 0xff (vendor-specific), bringt sowohl BidCoS-RF als auch
 *   HmIP-Frames über DualCoPro. Treiber: hb_rf_usb_2 (CP210x-Init in
 *   userspace-mode, kein crypto-lock). */
#define HW_USB_PID_HMIP_RFUSB      0xc020

/* Ioctls auf /dev/raw-uart (von refs/piVCCU/generic_raw_uart.h und ../hm.h).
 * Linux _IO/_IOW-Encoding ist Standard; Werte numerisch geprüft per Live-
 * Test (IOCGDEVINFO = 0x40407582). MAX_DEVICE_TYPE_LEN = 64. */
#define HW_IOCTL_MAGIC             'u'
#define HW_DEVINFO_LEN             64
/* _IOW(magic, 0x82, char[64])  →  size 64 << 16 | dir 1 << 30 | 'u'<<8 | 0x82 */
#define HW_IOC_GET_DEVINFO         (((1u)<<30) | (HW_DEVINFO_LEN<<16) | ((unsigned)HW_IOCTL_MAGIC<<8) | 0x82u)
/* _IO(magic, 0x81)  →  dir 0 | size 0 | 'u'<<8 | 0x81 */
#define HW_IOC_RESET_RADIO         (((unsigned)HW_IOCTL_MAGIC<<8) | 0x81u)

typedef enum {
    HW_KIND_UNKNOWN = 0,
    HW_KIND_HMIP_RFUSB,         /* eQ-3 HmIP-RFUSB (1b1f:c020), Dual-Stack */
    HW_KIND_HB_RF_USB,          /* HB-RF-USB v1, hat HM-MOD-RPI-PCB on top  */
    HW_KIND_HB_RF_USB_2,        /* HB-RF-USB v2, hat HM-MOD-RPI-PCB / RPI-RF-MOD on top */
    HW_KIND_HM_MOD_RPI_PCB,     /* HM-MOD-RPI-PCB direkt am Pi-UART (HAT) */
    HW_KIND_RPI_RF_MOD,         /* RPI-RF-MOD direkt am Pi-UART (HAT) */
} hw_radio_kind_t;

typedef struct {
    hw_radio_kind_t kind;
    char            devinfo[HW_DEVINFO_LEN];   /* product@usb-bus-devpath, leer wenn ioctl unsupported */
    bool            supports_radio_reset;       /* IOCRESET_RADIO_MODULE wäre sinnvoll */
    bool            dual_stack;                 /* sendet auch HmIP-Frames (nicht nur BidCoS) */
} hw_radio_info_t;

/* Identifizierung anhand des bereits geöffneten fd (Kernel-Char-Device).
 * Wenn der IOCGDEVINFO-ioctl nicht supported ist (z.B. /dev/ttyUSB* via
 * ftdi_sio statt generic_raw_uart), wird `path` für einen sysfs-Fallback
 * benutzt: /sys/class/tty/<base>/device/.. → idVendor/idProduct/product.
 * `path` darf NULL sein — dann ist der Fallback inaktiv.
 *
 * Schreibt Treiber-Devinfo + abgeleiteten kind nach *out.
 * Returns: 0 auch dann, wenn weder ioctl noch sysfs etwas liefern
 *          (kind=UNKNOWN, devinfo leer); -1 nur bei syscall-Fehler. */
int hw_identify(int fd, const char *path, hw_radio_info_t *out);

/* Hardware-Reset des Funkmoduls über IOCTL_IOCRESET_RADIO_MODULE.
 * Nicht alle Treiber/Module supporten das (CP2102 ohne GPIO-Latch nicht).
 * Returns: 0 ok, -1 errno. */
int hw_reset_radio(int fd);

const char *hw_kind_name(hw_radio_kind_t kind);

#endif
