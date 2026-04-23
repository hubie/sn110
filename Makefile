# Placeholder Makefile for SN110 Open Firmware
# Cross-compilation toolchain must be set up first (see PLAN.md Phase 1a)

# --- Toolchain Configuration ---
# For device bFLT binary (future: full uClinux toolchain)
CROSS_COMPILE ?= arm-linux-uclinux-
CC_DEVICE = $(CROSS_COMPILE)gcc

# For ARM ELF testing (Docker + QEMU)
ARM_CC = arm-linux-gnueabi-gcc
ARM_CFLAGS = -Wall -Wextra -Os -g -static \
    -march=armv4t -marm \
    -DHOST_BUILD -DMOCK_DMX
ARM_LDFLAGS = -static -lpthread
QEMU = qemu-arm-static

# For OABI device binary (ARM7TDMI + Linux 2.0)
# -fpic -msingle-pic-base: all global accesses go through GOT (in data section)
# This ensures bFLT v2 relocations only need data-section sites.
# sl (r10) is set to GOT base by crt0.S.
GCC_INCLUDE = $(shell $(ARM_CC) -print-file-name=include)
LIBGCC = $(shell $(ARM_CC) -print-libgcc-file-name)
OABI_CFLAGS = -Wall -Wextra -Os -g \
    -nostdlib -ffreestanding -nostdinc \
    -isystem $(GCC_INCLUDE) \
    -isystem src/oabi/include \
    -march=armv4t -marm \
    -fpic -msingle-pic-base -mpic-register=sl \
    -static -no-pie
OABI_SRCS = src/oabi/crt0.S src/oabi/syscalls.S \
    src/oabi/minilib.c src/oabi/minisock.c src/oabi/minithread.c \
    src/main.c src/sacn/sacn.c src/sacn/sacn_tx.c src/artnet/artnet.c src/shownet/shownet.c \
    src/dmx/dmx_real.c src/config/config.c

# For host-based testing (macOS/Linux native)
HOST_CC = gcc

# --- Flags ---
CFLAGS = -Wall -Wextra -Os -fno-builtin -nostdinc
CFLAGS += -mcpu=arm7tdmi -mno-thumb
LDFLAGS = -elf2flt

# Host test flags
HOST_CFLAGS = -Wall -Wextra -O2 -g -DHOST_BUILD -DMOCK_DMX
HOST_LDFLAGS = -lpthread

# --- Target ---
TARGET = sn110dmx
TARGET_BFLT = $(TARGET).bflt

# --- Sources ---
SRC_DIRS = src src/sacn src/artnet src/shownet src/dmx src/config src/lcd
SRCS = $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
OBJS = $(SRCS:.c=.o)

# Host test sources (everything except main.c)
LIB_SRCS = src/sacn/sacn.c src/sacn/sacn_tx.c src/artnet/artnet.c src/shownet/shownet.c \
           src/dmx/dmx_mock.c src/dmx/dmx_real.c src/config/config.c
TEST_SRCS = tests/test_basics.c $(LIB_SRCS)

# --- Device Configuration ---
IP ?= 192.168.0.71
FTP_USER ?= anonymous
FTP_PASS ?= anonymous

# --- Docker ---
DOCKER_IMAGE = sn110-toolchain
DOCKER_RUN = docker run --rm -v $(shell pwd):/project $(DOCKER_IMAGE)

# ==============================================================================
# Build Targets
# ==============================================================================

# CGI binary sources (minisock needed for MAC address ioctl)
CGI_SRCS = src/oabi/crt0.S src/oabi/syscalls.S \
    src/oabi/minilib.c src/oabi/minisock.c src/config/config.c src/cgi/cgi_config.c

# Fuzzer compiler: needs real clang with libFuzzer (Apple clang lacks it)
FUZZ_CC := $(shell \
    if command -v /opt/homebrew/opt/llvm/bin/clang >/dev/null 2>&1; then \
        echo /opt/homebrew/opt/llvm/bin/clang; \
    elif command -v /usr/local/opt/llvm/bin/clang >/dev/null 2>&1; then \
        echo /usr/local/opt/llvm/bin/clang; \
    else \
        echo clang; \
    fi)

.PHONY: all clean test test-asan arm-test oabi-daemon bflt oabi-cgi cgi-bflt oabi-probe probe-bflt oabi-lcd-probe lcd-probe-bflt oabi-lcd-partial lcd-partial-bflt docker-build docker-test docker-shell docker-bflt docker-cgi-bflt docker-probe-bflt docker-lcd-probe-bflt docker-lcd-partial-bflt deploy-web fuzz-config fuzz-cgi help

all: $(TARGET_BFLT)
	@echo "Built $(TARGET_BFLT) ($$(wc -c < $(TARGET_BFLT)) bytes)"
	@echo "Budget: 449632 bytes (size of original lxnetdmx)"

help:
	@echo "SN110 Open Firmware Build System"
	@echo ""
	@echo "  make              Build cross-compiled bFLT binary (needs uClinux toolchain)"
	@echo "  make test         Build and run host-based tests (macOS/Linux native)"
	@echo "  make arm-test     Build and run ARM tests under QEMU (use inside Docker)"
	@echo "  make docker-build Build the Docker toolchain image"
	@echo "  make docker-test  Build Docker image and run ARM tests"
	@echo "  make docker-shell Open interactive shell in Docker toolchain"
	@echo "  make deploy-ram   Upload to /tmp/ on device (safe, lost on reboot)"
	@echo "  make deploy-web   Deploy CGI binary + web UI to device"
	@echo "  make deploy-flash Flash to device (persistent, read SAFETY.md first!)"
	@echo "  make backup       Download all files from device via FTP"
	@echo "  make restore      Restore original lxnetdmx to device"
	@echo "  make clean        Remove build artifacts"

# ==============================================================================
# Cross-compilation (ARM7 → bFLT) — requires uClinux toolchain
# ==============================================================================

$(TARGET_BFLT): $(OBJS)
	$(CC_DEVICE) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC_DEVICE) $(CFLAGS) -c -o $@ $<

# ==============================================================================
# Host Testing (native macOS/Linux)
# ==============================================================================

test: build/test_runner
	./build/test_runner

build/test_runner: $(TEST_SRCS) | build
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(TEST_SRCS) $(HOST_LDFLAGS)

build:
	mkdir -p build

# ==============================================================================
# Sanitizer Testing (ASan + UBSan)
# ==============================================================================

test-asan: build/test_runner_asan
	./build/test_runner_asan

build/test_runner_asan: $(TEST_SRCS) | build
	$(HOST_CC) $(HOST_CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
		-o $@ $(TEST_SRCS) $(HOST_LDFLAGS)

# ==============================================================================
# Fuzzing (requires LLVM clang with libFuzzer)
# ==============================================================================

fuzz-config: build/fuzz_config
	@mkdir -p build/corpus_config
	@[ -d tests/corpus_config ] && cp tests/corpus_config/* build/corpus_config/ 2>/dev/null; true
	./build/fuzz_config build/corpus_config

build/fuzz_config: tests/fuzz_config.c src/config/config.c | build
	$(FUZZ_CC) -Wall -Wextra -O2 -g -DHOST_BUILD -DMOCK_DMX \
		-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		-o $@ tests/fuzz_config.c src/config/config.c

fuzz-cgi: build/fuzz_cgi
	@mkdir -p build/corpus_cgi
	@[ -d tests/corpus_cgi ] && cp tests/corpus_cgi/* build/corpus_cgi/ 2>/dev/null; true
	./build/fuzz_cgi build/corpus_cgi

build/fuzz_cgi: tests/fuzz_cgi.c src/config/config.c | build
	$(FUZZ_CC) -Wall -Wextra -O2 -g -DHOST_BUILD -DMOCK_DMX \
		-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		-o $@ tests/fuzz_cgi.c src/config/config.c

# ==============================================================================
# ARM Testing (cross-compiled, runs under QEMU)
# ==============================================================================

arm-test: build/test_runner_arm
	$(QEMU) ./build/test_runner_arm

build/test_runner_arm: $(TEST_SRCS) | build
	$(ARM_CC) $(ARM_CFLAGS) -o $@ $(TEST_SRCS) $(ARM_LDFLAGS)
	@echo "Built ARM test binary: $$(file $@)"

# Also build main daemon as ARM ELF for size/link checking
arm-daemon: build/sn110dmx_arm

build/sn110dmx_arm: src/main.c $(LIB_SRCS) | build
	$(ARM_CC) $(ARM_CFLAGS) -o $@ $^ $(ARM_LDFLAGS)
	@SIZE=$$(wc -c < $@); \
	echo "ARM ELF size: $$SIZE bytes (budget: 449632 for bFLT)"; \
	echo "(Note: static ELF is larger than bFLT; final size will be smaller)"

# ==============================================================================
# OABI Device Binary (ARM7TDMI + Linux 2.0 compatible)
# ==============================================================================

oabi-daemon: build/sn110dmx_oabi

build/sn110dmx_oabi: $(OABI_SRCS) | build
	$(ARM_CC) $(OABI_CFLAGS) -o $@ $(OABI_SRCS) $(LIBGCC)
	@SIZE=$$(wc -c < $@); \
	echo "OABI ELF size: $$SIZE bytes (budget: 449632 for bFLT)"; \
	echo "Note: bFLT conversion will reduce size further"

# OABI ELF with relocations (for bFLT conversion)
build/sn110dmx_reloc.elf: $(OABI_SRCS) src/oabi/flat.ld | build
	$(ARM_CC) $(OABI_CFLAGS) \
		-T src/oabi/flat.ld \
		-Wl,--emit-relocs,--build-id=none \
		-o $@ $(OABI_SRCS) $(LIBGCC)

# Device-ready bFLT binary
bflt: build/sn110dmx.bflt

build/sn110dmx.bflt: build/sn110dmx_reloc.elf tools/elf2bflt.py
	python3 tools/elf2bflt.py $< $@

# ==============================================================================
# CGI Binary (OABI — for web configuration interface)
# ==============================================================================

oabi-cgi: build/cgi_config_oabi

build/cgi_config_oabi: $(CGI_SRCS) | build
	$(ARM_CC) $(OABI_CFLAGS) -o $@ $(CGI_SRCS) $(LIBGCC)
	@SIZE=$$(wc -c < $@); \
	echo "CGI OABI ELF size: $$SIZE bytes"

build/cgi_config_reloc.elf: $(CGI_SRCS) src/oabi/flat.ld | build
	$(ARM_CC) $(OABI_CFLAGS) \
		-T src/oabi/flat.ld \
		-Wl,--emit-relocs,--build-id=none \
		-o $@ $(CGI_SRCS) $(LIBGCC)

cgi-bflt: build/cgi_config.bflt

build/cgi_config.bflt: build/cgi_config_reloc.elf tools/elf2bflt.py
	python3 tools/elf2bflt.py $< $@
	@SIZE=$$(wc -c < $@); \
	echo "CGI bFLT size: $$SIZE bytes"

# RCGT probe — on-device UART register experiment tool
PROBE_SRCS = src/oabi/crt0.S src/oabi/syscalls.S src/oabi/minilib.c tests/rcgt_probe.c

# LCD probe — /dev/lcd0 interface discovery tool
LCD_PROBE_SRCS = src/oabi/crt0.S src/oabi/syscalls.S src/oabi/minilib.c tests/lcd_probe.c

oabi-probe: build/rcgt_probe_oabi

build/rcgt_probe_oabi: $(PROBE_SRCS) | build
	$(ARM_CC) $(OABI_CFLAGS) -o $@ $(PROBE_SRCS) $(LIBGCC)
	@SIZE=$$(wc -c < $@); echo "RCGT probe OABI ELF: $$SIZE bytes"

build/rcgt_probe_reloc.elf: $(PROBE_SRCS) src/oabi/flat.ld | build
	$(ARM_CC) $(OABI_CFLAGS) \
		-T src/oabi/flat.ld \
		-Wl,--emit-relocs,--build-id=none \
		-o $@ $(PROBE_SRCS) $(LIBGCC)

probe-bflt: build/rcgt_probe.bflt

build/rcgt_probe.bflt: build/rcgt_probe_reloc.elf tools/elf2bflt.py
	python3 tools/elf2bflt.py $< $@
	@SIZE=$$(wc -c < $@); echo "RCGT probe bFLT: $$SIZE bytes"

docker-probe-bflt: docker-build
	$(DOCKER_RUN) make probe-bflt

oabi-lcd-probe: build/lcd_probe_oabi

build/lcd_probe_oabi: $(LCD_PROBE_SRCS) | build
	$(ARM_CC) $(OABI_CFLAGS) -o $@ $(LCD_PROBE_SRCS) $(LIBGCC)
	@SIZE=$$(wc -c < $@); echo "LCD probe OABI ELF: $$SIZE bytes"

build/lcd_probe_reloc.elf: $(LCD_PROBE_SRCS) src/oabi/flat.ld | build
	$(ARM_CC) $(OABI_CFLAGS) \
		-T src/oabi/flat.ld \
		-Wl,--emit-relocs,--build-id=none \
		-o $@ $(LCD_PROBE_SRCS) $(LIBGCC)

lcd-probe-bflt: build/lcd_probe.bflt

build/lcd_probe.bflt: build/lcd_probe_reloc.elf tools/elf2bflt.py
	python3 tools/elf2bflt.py $< $@
	@SIZE=$$(wc -c < $@); echo "LCD probe bFLT: $$SIZE bytes"

docker-lcd-probe-bflt: docker-build
	$(DOCKER_RUN) make lcd-probe-bflt

# LCD partial write test — proves cursor+NUL partial update mechanism
LCD_PARTIAL_SRCS = src/oabi/crt0.S src/oabi/syscalls.S src/oabi/minilib.c tests/lcd_partial.c

oabi-lcd-partial: build/lcd_partial_oabi

build/lcd_partial_oabi: $(LCD_PARTIAL_SRCS) | build
	$(ARM_CC) $(OABI_CFLAGS) -o $@ $(LCD_PARTIAL_SRCS) $(LIBGCC)
	@SIZE=$$(wc -c < $@); echo "LCD partial OABI ELF: $$SIZE bytes"

build/lcd_partial_reloc.elf: $(LCD_PARTIAL_SRCS) src/oabi/flat.ld | build
	$(ARM_CC) $(OABI_CFLAGS) \
		-T src/oabi/flat.ld \
		-Wl,--emit-relocs,--build-id=none \
		-o $@ $(LCD_PARTIAL_SRCS) $(LIBGCC)

lcd-partial-bflt: build/lcd_partial.bflt

build/lcd_partial.bflt: build/lcd_partial_reloc.elf tools/elf2bflt.py
	python3 tools/elf2bflt.py $< $@
	@SIZE=$$(wc -c < $@); echo "LCD partial bFLT: $$SIZE bytes"

docker-lcd-partial-bflt: docker-build
	$(DOCKER_RUN) make lcd-partial-bflt

# Minimal test bFLT (hello world — for verifying bFLT format)
build/hello_device.bflt: tests/hello_device.c src/oabi/crt0.S src/oabi/flat.ld tools/elf2bflt.py | build
	$(ARM_CC) $(OABI_CFLAGS) \
		-T src/oabi/flat.ld \
		-Wl,--emit-relocs,--build-id=none \
		-o build/hello_reloc.elf src/oabi/crt0.S tests/hello_device.c $(LIBGCC)
	python3 tools/elf2bflt.py build/hello_reloc.elf $@

# ==============================================================================
# Docker — containerized ARM toolchain + QEMU
# ==============================================================================

docker-build:
	docker build -t $(DOCKER_IMAGE) -f docker/Dockerfile .

docker-test: docker-build
	$(DOCKER_RUN) make arm-test

docker-oabi: docker-build
	$(DOCKER_RUN) make oabi-daemon

docker-bflt: docker-build
	$(DOCKER_RUN) make bflt

docker-cgi-bflt: docker-build
	$(DOCKER_RUN) make cgi-bflt

docker-shell: docker-build
	docker run --rm -it -v $(shell pwd):/project $(DOCKER_IMAGE) bash

# ==============================================================================
# Deployment
# ==============================================================================

deploy-ram: $(TARGET_BFLT)
	@echo "Uploading $(TARGET_BFLT) to $(IP):/tmp/$(TARGET)..."
	curl -T $(TARGET_BFLT) -u $(FTP_USER):$(FTP_PASS) ftp://$(IP)/tmp/$(TARGET)
	@echo ""
	@echo "Upload complete. To test, telnet to $(IP) and run:"
	@echo "  /tmp/$(TARGET)"
	@echo ""
	@echo "To stop the existing daemon first:"
	@echo "  # Find PID of lxnetdmx and kill it"
	@echo "  ps"

deploy-web: build/cgi_config.bflt
	python3 tools/deploy_web.py $(IP)

deploy-flash: $(TARGET_BFLT)
	@echo "⚠️  WARNING: This will permanently modify the device firmware!"
	@echo "Have you read SAFETY.md? Have you tested with deploy-ram first?"
	@echo "Press Ctrl-C to abort, or Enter to continue..."
	@read _confirm
	@echo "Uploading install files to $(IP)..."
	curl -T tools/install.sh -u $(FTP_USER):$(FTP_PASS) ftp://$(IP)/tmp/install.sh
	curl -T $(TARGET_BFLT) -u $(FTP_USER):$(FTP_PASS) ftp://$(IP)/tmp/$(TARGET)
	curl -T tools/install-trigger -u $(FTP_USER):$(FTP_PASS) ftp://$(IP)/tmp/install.arm
	@echo ""
	@echo "Install triggered. The device will:"
	@echo "  1. Detect /tmp/install.arm within 10 seconds"
	@echo "  2. Run /tmp/install.sh"
	@echo "  3. Replace /usr/bin/lxnetdmx with the new binary"
	@echo "  4. Reboot"

# ==============================================================================
# Backup & Recovery
# ==============================================================================

backup:
	@echo "Backing up device at $(IP) to dump/firmware/..."
	tools/backup.sh $(IP)

deploy:
	@echo "Deploying open firmware to $(IP)..."
	./tools/install.sh $(IP)

restore:
	@echo "Restoring original firmware to $(IP)..."
	./tools/restore.sh $(IP)

# ==============================================================================
# Cleanup
# ==============================================================================

clean:
	rm -f $(OBJS) $(TARGET_BFLT)
	rm -rf build/
