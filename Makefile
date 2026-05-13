# SPDX-License-Identifier: GPL-2.0-or-later
# BusMatic — Build (lean shim-mode)
#
# Targets:
#   all          — build concentrator
#   concentrator — busmatic-concentrator daemon (lean userspace transport-shim)
#   tools        — copro_diag + hmip_probe + eth_soak (Diagnose-CLIs)
#   clean        — drop build artifacts
#
# Version-String wird aus version.txt gelesen (eine Zeile, z.B. "2026.5.7")
# und beim Build in version.h hineingeneriert.

PROJECT  := busmatic-concentrator
CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 -D_GNU_SOURCE
LDFLAGS  ?=

BUILD    := build
BIN      := bin

VERSION_TXT  := version.txt
VERSION_H    := version.h

VERSION  := $(shell cat $(VERSION_TXT) 2>/dev/null || echo 0.0.0)

.PHONY: all clean concentrator tools hmip_probe copro_diag eth_soak version

all: version concentrator
tools: hmip_probe copro_diag eth_soak

# ── version.h regeneration ────────────────────────────────────────────
version: $(VERSION_H)

$(VERSION_H): $(VERSION_TXT)
	@echo "Generating $(VERSION_H) — $(VERSION)"
	@printf '#ifndef CUL32HM_VERSION_H\n#define CUL32HM_VERSION_H\n' > $@
	@printf '#define FW_VERSION_STRING "%s"\n' "$(VERSION)" >> $@
	@printf '#define FW_BUILD_DATE "%s"\n' "$$(date -Iseconds)" >> $@
	@printf '#endif\n' >> $@

# ── compile rules ─────────────────────────────────────────────────────
$(BUILD)/%.o: %.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# ── busmatic-concentrator daemon (lean) ───────────────────────────────
$(BIN)/busmatic-concentrator: $(BUILD)/frame.o $(BUILD)/hardware.o $(BUILD)/transport_uart.o $(BUILD)/transport_tcp.o $(BUILD)/transport_usb.o $(BUILD)/transport_eth.o $(BUILD)/transport_rfnethm.o $(BUILD)/radio.o $(BUILD)/radio_dualcopro.o $(BUILD)/copro_query.o $(BUILD)/eq3_image.o $(BUILD)/backend.o $(BUILD)/confgen.o $(BUILD)/sources.o $(BUILD)/api.o $(BUILD)/concentrator.o
	@mkdir -p $(BIN)
	$(CC) $(LDFLAGS) -o $@ $^ -lutil -lpthread -lusb-1.0 -lcjson

concentrator: $(BIN)/busmatic-concentrator

# ── tools/hmip_probe — diag-tool für HmIP-capability-Differential ─────
$(BUILD)/tools/hmip_probe.o: tools/hmip_probe.c
	@mkdir -p $(BUILD)/tools
	$(CC) $(CFLAGS) -I. -c -o $@ $<

$(BIN)/hmip_probe: $(BUILD)/tools/hmip_probe.o $(BUILD)/frame.o $(BUILD)/transport_usb.o $(BUILD)/radio_dualcopro.o $(BUILD)/radio.o $(BUILD)/hardware.o $(BUILD)/backend.o
	@mkdir -p $(BIN)
	$(CC) $(LDFLAGS) -o $@ $^ -lpthread -lusb-1.0 -lutil

hmip_probe: $(BIN)/hmip_probe

# ── tools/copro_diag — Co-CPU-Inventory Standalone-Tool ───────────────
$(BUILD)/tools/copro_diag.o: tools/copro_diag.c
	@mkdir -p $(BUILD)/tools
	$(CC) $(CFLAGS) -I. -c -o $@ $<

$(BIN)/copro_diag: $(BUILD)/tools/copro_diag.o $(BUILD)/copro_query.o $(BUILD)/eq3_image.o $(BUILD)/frame.o $(BUILD)/transport_usb.o $(BUILD)/transport_uart.o $(BUILD)/transport_tcp.o $(BUILD)/transport_eth.o $(BUILD)/transport_rfnethm.o $(BUILD)/radio_dualcopro.o $(BUILD)/radio.o $(BUILD)/hardware.o $(BUILD)/backend.o
	@mkdir -p $(BIN)
	$(CC) $(LDFLAGS) -o $@ $^ -lpthread -lusb-1.0 -lutil

copro_diag: $(BIN)/copro_diag

# ── tools/eth_soak — Soak-Helper für transport_eth/rfnethm-Liveness ───
$(BUILD)/tools/eth_soak.o: tools/eth_soak.c
	@mkdir -p $(BUILD)/tools
	$(CC) $(CFLAGS) -I. -c -o $@ $<

$(BIN)/eth_soak: $(BUILD)/tools/eth_soak.o $(BUILD)/transport_eth.o $(BUILD)/transport_rfnethm.o
	@mkdir -p $(BIN)
	$(CC) $(LDFLAGS) -o $@ $^ -lpthread

eth_soak: $(BIN)/eth_soak

# ── housekeeping ──────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD) $(BIN) $(VERSION_H)

# Compile-deps: header changes invalidate object files
$(BUILD)/frame.o:                frame.c frame.h
$(BUILD)/hardware.o:             hardware.c hardware.h
$(BUILD)/radio.o:                radio.c radio.h
$(BUILD)/radio_dualcopro.o:      radio_dualcopro.c radio.h backend.h frame.h hardware.h
$(BUILD)/copro_query.o:          copro_query.c copro_query.h frame.h eq3_image.h
$(BUILD)/eq3_image.o:            eq3_image.c eq3_image.h
$(BUILD)/backend.o:              backend.c backend.h radio.h transport.h frame.h
$(BUILD)/confgen.o:              confgen.c confgen.h
$(BUILD)/transport_uart.o:       transport_uart.c transport_uart.h transport.h
$(BUILD)/transport_tcp.o:        transport_tcp.c  transport_tcp.h  transport.h
$(BUILD)/transport_usb.o:        transport_usb.c  transport_usb.h  transport.h
$(BUILD)/transport_eth.o:        transport_eth.c  transport_eth.h  transport.h
$(BUILD)/transport_rfnethm.o:    transport_rfnethm.c  transport_rfnethm.h  transport_eth.h  transport.h
$(BUILD)/api.o:                  api.c api.h radio_id.h sources.h transport_usb.h version.h
$(BUILD)/sources.o:              sources.c sources.h
$(BUILD)/concentrator.o:         concentrator.c api.h confgen.h copro_query.h eq3_image.h hardware.h radio_dualcopro.h radio_id.h transport.h transport_uart.h transport_tcp.h transport_usb.h transport_eth.h version.h
