CROSS_PREFIX ?= i686-w64-mingw32-
CC := $(CROSS_PREFIX)gcc
OBJDUMP := $(CROSS_PREFIX)objdump
TOP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
MINHOOK := $(TOP)/third_party/minhook

CFLAGS  := -m32 -O1 -gdwarf-4 -std=gnu11 -Wall -I$(MINHOOK)/include
LDFLAGS := -m32 -static-libgcc -static

all: out/camera.asi verify

out:
	@mkdir -p out

$(MINHOOK)/libMinHook.a:
	$(MAKE) -C $(TOP) minhook

out/camera.asi: camera.c $(MINHOOK)/libMinHook.a | out
	$(CC) $(CFLAGS) -shared -o $@ $< $(MINHOOK)/libMinHook.a $(LDFLAGS)

verify: out/camera.asi
	@echo "==> the two hand-written thunks must survive optimisation intact"
	@$(OBJDUMP) -d out/camera.asi | grep -A6 "<_hk_R_SetViewParms>:" | sed 's/^/    /'
	@$(OBJDUMP) -d out/camera.asi | grep -A4 "<_call_original>:" | sed 's/^/    /'
	@echo "==> runtime DLL dependencies (libgcc must NOT appear)"
	@$(OBJDUMP) -p out/camera.asi | grep "DLL Name" | sed 's/^/    /'
	@if $(OBJDUMP) -p out/camera.asi | grep -qi libgcc; then echo "FAIL: libgcc"; exit 1; \
	 else echo "OK: no libgcc runtime dependency"; fi

PLUGDIR ?= /mnt/games/steam/steamapps/compatdata/42700/pfx/drive_c/bo1vr
install: out/camera.asi
	@mkdir -p $(PLUGDIR)
	cp $< $(PLUGDIR)/
	@echo "installed -> $(PLUGDIR)/camera.asi"

clean:
	rm -rf out
.PHONY: all verify install clean
