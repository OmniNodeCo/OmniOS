# OmniOS — os/Makefile
#
# Builds the from-scratch OmniOS userspace core: the PID-1 init, the desktop
# shell (window manager + display server) and the bundled desktop apps.
#
# Compiler: the musl static cross toolchain built into build/sysroot by
# scripts/build.sh (stage "musl"). Override with CC=...
#
#   make -C os                build everything
#   make -C os install        copy binaries to build/install/os/bin
#   make -C os clean
#
# Every binary is fully static (musl), so it runs inside the initramfs
# before any shared library exists.

ROOT      ?= ..
MUSL_GCC  ?= $(ROOT)/build/sysroot/usr/bin/musl-gcc
CC        := $(MUSL_GCC)
CFLAGS    := -std=c99 -Os -Wall -Wextra -ffunction-sections -fdata-sections \
             -I lib -I gui -I kernel
LDFLAGS   := -static -Wl,--gc-sections
LDLIBS    := -lm

OUTDIR    := $(ROOT)/build/install/os/bin

BIN       := omnios-desktop ominit omnios-term omnios-files omnios-calc \
             omnios-edit omnios-sysinfo omnios-about omnios-store \
             omnios-clock omnios-snake

# --- object lists -----------------------------------------------------
LIB_OBJ := lib/raster.o lib/canvas.o lib/osfb.o lib/input.o lib/devinput.o
GUI_OBJ := gui/wm.o gui/protocol.o gui/client.o gui/shell.o gui/desktop.o \
           gui/catalog.o
KER_OBJ := kernel/ominit.o kernel/ommount.o

APP_OBJ := apps/app.o

all: $(addprefix $(OUTDIR)/,$(BIN))

# --- desktop shell -----------------------------------------------------
$(OUTDIR)/omnios-desktop: gui/desktop.o gui/shell.o gui/wm.o gui/protocol.o \
		gui/catalog.o lib/raster.o lib/canvas.o lib/osfb.o lib/input.o \
		lib/devinput.o lib/gfx.o
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# --- PID 1 init ---------------------------------------------------------
$(OUTDIR)/ominit: $(KER_OBJ)
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --- bundled apps --------------------------------------------------------
$(OUTDIR)/omnios-term: apps/terminal.o apps/vt.o gui/client.o gui/protocol.o
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OUTDIR)/omnios-files: apps/files.o $(APP_OBJ) gui/client.o gui/protocol.o
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OUTDIR)/omnios-calc: apps/calc.o $(APP_OBJ) gui/client.o gui/protocol.o
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OUTDIR)/omnios-edit: apps/edit.o gui/client.o gui/protocol.o
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OUTDIR)/omnios-sysinfo: apps/sysinfo.o $(APP_OBJ) gui/client.o gui/protocol.o
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OUTDIR)/omnios-about: apps/about.o $(APP_OBJ) gui/client.o gui/protocol.o
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# App Store + the apps it offers
$(OUTDIR)/omnios-store: apps/store.o gui/catalog.o gui/client.o gui/protocol.o
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OUTDIR)/omnios-clock: apps/clock.o gui/client.o gui/protocol.o
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OUTDIR)/omnios-snake: apps/snake.o gui/client.o gui/protocol.o
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --- generic compile rules ----------------------------------------------
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

lib/gfx.o: lib/gfx.c lib/omni.h
kernel/ominit.o: kernel/ominit.c kernel/ominit.h
kernel/ommount.o: kernel/ommount.c kernel/ominit.h
gui/desktop.o: gui/shell.h
gui/shell.o: gui/shell.h gui/wm.h gui/widgets.h gui/catalog.h
gui/catalog.o: gui/catalog.h
gui/wm.o: gui/wm.h gui/protocol.h
gui/protocol.o: gui/protocol.h
gui/client.o: gui/client.h gui/protocol.h
apps/app.o: apps/app.h
apps/terminal.o: apps/vt.h gui/client.h gui/wm.h
apps/vt.o: apps/vt.h
apps/store.o: gui/catalog.h gui/client.h gui/wm.h
apps/clock.o apps/snake.o: gui/client.h gui/wm.h
apps/files.o: apps/app.h
apps/calc.o: apps/app.h
apps/edit.o: gui/client.h gui/wm.h
apps/sysinfo.o: apps/app.h
apps/about.o: apps/app.h
lib/raster.o lib/canvas.o lib/osfb.o lib/input.o lib/devinput.o: lib/omni.h

install: all

clean:
	rm -f $(LIB_OBJ) $(GUI_OBJ) $(KER_OBJ) apps/*.o
	rm -f $(addprefix $(OUTDIR)/,$(BIN))

.PHONY: all install clean
