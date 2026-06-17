# nrf_ocd - portable pyOCD clone focused on nRF54L15 / nRF54LM20A
#
# Build:
#   make                  -> release build (default)
#   make DEBUG=1          -> debug build with -O0 -g
#   make clean            -> remove build artifacts
#   make test             -> build and run self-tests
#
# Cross-compile examples:
#   make OS=windows CROSS=i686-w64-mingw32-   -> Windows .exe
#   make OS=darwin                            -> macOS (run on macOS host)
#
# Strictly no external dependencies. The HID layer uses only OS-native APIs
# (hidraw on Linux, IOKit HID on macOS, hid.dll + setupapi.dll on Windows).
#
# Optionally, libusb-1.0 can be enabled for CMSIS-DAP v2 bulk endpoints
# (which some boards - notably the Seeed XIAO nRF54 - require for
# reliable DAP_Transfer responses). Set USE_LIBUSB=1 to build with
# libusb support; the resulting binary will prefer the bulk backend
# when both are available.

# ----- Host / target OS detection --------------------------------------------
HOST_OS  := $(shell uname -s)
TARGET_OS := $(OS)

# ----- Optional libusb support -----------------------------------------------
USE_LIBUSB ?= 0
ifeq ($(USE_LIBUSB),1)
    LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0 2>/dev/null)
    LIBUSB_LDLIBS := $(shell pkg-config --libs libusb-1.0 2>/dev/null)
    ifeq ($(LIBUSB_LDLIBS),)
        LIBUSB_CFLAGS := -I/usr/include/libusb-1.0
        LIBUSB_LDLIBS := -lusb-1.0
        LIBUSB_LDFLAGS := -L/usr/lib/x86_64-linux-gnu
    endif
    DEFS += -DNRF_OCD_USE_LIBUSB=1
endif

ifeq ($(TARGET_OS),)
    ifeq ($(HOST_OS),Linux)
        TARGET_OS := linux
    else ifeq ($(HOST_OS),Darwin)
        TARGET_OS := darwin
    else ifeq ($(OS),Windows_NT)
        TARGET_OS := windows
    else
        $(error Unsupported host OS: $(HOST_OS))
    endif
endif

# ----- Tools ------------------------------------------------------------------
CC      ?= cc
AR      ?= ar
STRIP   ?= strip
CROSS   ?=
PREFIX  ?= /usr/local

# ----- Source layout ----------------------------------------------------------
SRC_DIR   := src
OBJ_DIR   := build/obj
BIN_DIR   := build/bin
LIB_DIR   := build/lib
TEST_DIR  := tests
INCLUDE   := -I$(SRC_DIR)

# ----- Files ------------------------------------------------------------------
# One HID source per OS, selected automatically.
ifeq ($(TARGET_OS),linux)
    ifeq ($(USE_LIBUSB),1)
        HID_SRC := $(SRC_DIR)/hid_libusb.c
        HID_OBJ := $(OBJ_DIR)/hid_libusb.o
    else
        HID_SRC := $(SRC_DIR)/hid_linux.c
        HID_OBJ := $(OBJ_DIR)/hid_linux.o
    endif
    OS_LDLIBS :=
    OS_CFLAGS :=
    BIN_EXT :=
endif
ifeq ($(TARGET_OS),darwin)
    HID_SRC := $(SRC_DIR)/hid_macos.c
    HID_OBJ := $(OBJ_DIR)/hid_macos.o
    OS_LDLIBS := -framework IOKit -framework CoreFoundation -framework Foundation
    OS_CFLAGS := -Wno-deprecated-declarations
    BIN_EXT :=
endif
ifeq ($(TARGET_OS),windows)
    HID_SRC := $(SRC_DIR)/hid_windows.c
    HID_OBJ := $(OBJ_DIR)/hid_windows.o
    OS_LDLIBS := -lhid -lsetupapi -lws2_32
    OS_CFLAGS := -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601
    BIN_EXT := .exe
endif

COMMON_SRC := \
    $(SRC_DIR)/log.c \
    $(SRC_DIR)/util.c \
    $(SRC_DIR)/hex.c \
    $(SRC_DIR)/elf.c \
    $(SRC_DIR)/probe.c \
    $(SRC_DIR)/cmsis_dap.c \
    $(SRC_DIR)/swd.c \
    $(SRC_DIR)/dap.c \
    $(SRC_DIR)/target.c \
    $(SRC_DIR)/target_nrf54l.c \
    $(SRC_DIR)/target_nrf54lm20a.c \
    $(SRC_DIR)/flash_algo_nrf54l.c \
    $(SRC_DIR)/flash.c \
    $(SRC_DIR)/commander.c \
    $(SRC_DIR)/cli.c

ifeq ($(USE_LIBUSB),1)
    COMMON_SRC += $(SRC_DIR)/hid_libusb.c
endif

COMMON_OBJ := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(COMMON_SRC))
ALL_OBJ    := $(COMMON_OBJ) $(HID_OBJ) $(OBJ_DIR)/main.o
DEP        := $(ALL_OBJ:.o=.d)

APP        := $(BIN_DIR)/nrf_ocd$(BIN_EXT)
LIB        := $(LIB_DIR)/libnrf_ocd.a

# ----- Compile flags ----------------------------------------------------------
WARN     := -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align \
            -Wwrite-strings -Wmissing-prototypes -Wstrict-prototypes \
            -Wredundant-decls -Wnested-externs -Winline -Wno-long-long \
            -Wuninitialized -Wconversion -Wno-sign-conversion \
            -Wformat=2
STD      := -std=c11
DEFS     := -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -DNRF_OCD_VERSION=\"0.1.0\"

ifeq ($(DEBUG),1)
    OPT     := -O0 -g3
else
    OPT     := -O2 -g
endif

CFLAGS   := $(STD) $(WARN) $(OPT) $(DEFS) $(OS_CFLAGS) $(LIBUSB_CFLAGS) $(INCLUDE) -MMD -MP
LDFLAGS  := $(LIBUSB_LDFLAGS)
LDLIBS   := $(OS_LDLIBS) $(LIBUSB_LDLIBS)

# ----- Default target ---------------------------------------------------------
.PHONY: all
all: dirs $(APP)

.PHONY: dirs
dirs:
	@mkdir -p $(OBJ_DIR) $(BIN_DIR) $(LIB_DIR)

# ----- Application link -------------------------------------------------------
$(APP): $(ALL_OBJ)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# ----- Static library (for reuse / tests) -------------------------------------
$(LIB): $(COMMON_OBJ) $(HID_OBJ)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

# ----- Generic compile rule ----------------------------------------------------
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# ----- Tests ------------------------------------------------------------------
TEST_BINS := $(BIN_DIR)/test_hex$(BIN_EXT) $(BIN_DIR)/test_elf$(BIN_EXT) \
             $(BIN_DIR)/test_target$(BIN_EXT)

.PHONY: test
test: $(TEST_BINS) $(APP)
	@echo "=== Running test_hex ==="
	$(BIN_DIR)/test_hex
	@echo "=== Running test_elf ==="
	$(BIN_DIR)/test_elf
	@echo "=== Running test_target ==="
	$(BIN_DIR)/test_target

$(BIN_DIR)/test_hex$(BIN_EXT): $(TEST_DIR)/test_hex.c $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $(TEST_DIR)/test_hex.c $(LIB) $(LDLIBS)

$(BIN_DIR)/test_elf$(BIN_EXT): $(TEST_DIR)/test_elf.c $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $(TEST_DIR)/test_elf.c $(LIB) $(LDLIBS)

$(BIN_DIR)/test_target$(BIN_EXT): $(TEST_DIR)/test_target.c $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $(TEST_DIR)/test_target.c $(LIB) $(LDLIBS)

# ----- Maintenance ------------------------------------------------------------
.PHONY: clean
clean:
	rm -rf build

.PHONY: install
install: $(APP)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(APP) $(DESTDIR)$(PREFIX)/bin/nrf_ocd

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/nrf_ocd

.PHONY: print-vars
print-vars:
	@echo "TARGET_OS=$(TARGET_OS)"
	@echo "HOST_OS=$(HOST_OS)"
	@echo "HID_SRC=$(HID_SRC)"
	@echo "CC=$(CC)"
	@echo "CFLAGS=$(CFLAGS)"

# ----- Cross-compilation targets -----------------------------------------------
.PHONY: linux-x64 linux-arm64 linux-armhf win64 clean-all

# Linux x86_64 (native, with libusb)
linux-x64:
	$(MAKE) clean
	$(MAKE) USE_LIBUSB=1 CC=/usr/bin/gcc-13
	cp build/bin/nrf_ocd build/bin/nrf_ocd-linux-x64

# Linux aarch64 (cross, HID backend)
linux-arm64:
	$(MAKE) clean
	$(MAKE) CC=aarch64-linux-gnu-gcc USE_LIBUSB=0 \
		CFLAGS_EXTRA="-D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L" \
		LDFLAGS_EXTRA="-static"
	cp build/bin/nrf_ocd build/bin/nrf_ocd-linux-arm64

# Linux armhf (cross, HID backend)
linux-armhf:
	$(MAKE) clean
	$(MAKE) CC=arm-linux-gnueabihf-gcc USE_LIBUSB=0 \
		CFLAGS_EXTRA="-D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L" \
		LDFLAGS_EXTRA="-static"
	cp build/bin/nrf_ocd build/bin/nrf_ocd-linux-armhf

# Windows x86_64 (cross, HID backend)
win64:
	$(MAKE) clean
	$(MAKE) CC=x86_64-w64-mingw32-gcc USE_LIBUSB=0 \
		OS=windows \
		CFLAGS_EXTRA="-D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L" \
		LDFLAGS_EXTRA="-static"
	cp build/bin/nrf_ocd.exe build/bin/nrf_ocd-win64.exe

# Build all release binaries
release: linux-x64 linux-arm64 linux-armhf win64
	@echo "=== Release binaries in build/bin/ ==="
	ls -la build/bin/

-include $(DEP)
