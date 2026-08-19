CROSS_PREFIX ?= i686-w64-mingw32-
CC := $(CROSS_PREFIX)gcc
OBJDUMP := $(CROSS_PREFIX)objdump
TOP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
MINHOOK := $(TOP)/third_party/minhook
OPENVR  := $(TOP)/third_party/openvr
EXP04   := $(TOP)/experiments/04_live_fntable
EXP05   := $(TOP)/experiments/05_submit

CFLAGS  := -m32 -O1 -gdwarf-4 -std=gnu11 -Wall \
           -I$(MINHOOK)/include -I$(OPENVR)/headers -I$(EXP04) -I$(EXP05)
LDFLAGS := -m32 -static-libgcc -static

all: out/gameframe.asi out/fakegame.exe out/bo1vr_loader.dll out/openvr_api.dll verify

out:
	@mkdir -p out

$(MINHOOK)/libMinHook.a:
	$(MAKE) -C $(TOP) minhook

out/gameframe.asi: gameframe.c $(MINHOOK)/libMinHook.a | out
	$(CC) $(CFLAGS) -shared -o $@ $< $(MINHOOK)/libMinHook.a -ld3d9 $(LDFLAGS)

out/fakegame.exe: fakegame.c | out
	$(CC) $(CFLAGS) -o $@ $< -ld3d9 $(LDFLAGS)

out/bo1vr_loader.dll: $(TOP)/dist/dinput8.dll | out
	cp $< $@

out/openvr_api.dll: $(OPENVR)/bin/win32/openvr_api.dll | out
	cp $< $@

verify: out/gameframe.asi
	@echo "==> runtime DLL dependencies (libgcc must NOT appear)"
	@$(OBJDUMP) -p $< | grep "DLL Name" | sed 's/^/    /'
	@if $(OBJDUMP) -p $< | grep -qi libgcc; then echo "FAIL: libgcc"; exit 1; \
	 else echo "OK: no libgcc runtime dependency"; fi

# Override PLUGDIR if your Steam library is not the default one.
PLUGDIR ?= $(HOME)/.local/share/Steam/steamapps/compatdata/42700/pfx/drive_c/bo1vr
install: out/gameframe.asi
	@mkdir -p $(PLUGDIR)
	cp $< $(PLUGDIR)/
	@echo "installed -> $(PLUGDIR)/gameframe.asi"

clean:
	rm -rf out
.PHONY: all verify install clean
