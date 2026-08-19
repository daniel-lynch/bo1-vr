CROSS_PREFIX ?= i686-w64-mingw32-
CC := $(CROSS_PREFIX)gcc
OBJDUMP := $(CROSS_PREFIX)objdump
TOP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
MINHOOK := $(TOP)/third_party/minhook

# -fexceptions so __try/__except in the hook bodies actually works. README
# Decision 6: an exception escaping a hook callback unwinds through the game's
# own frames and kills the process -- and a crash here would look exactly like
# the DRM exit of Exp. 8, which is a debugging trail nobody needs twice.
CFLAGS  := -m32 -O1 -gdwarf-4 -std=gnu11 -Wall -fexceptions -I$(MINHOOK)/include
LDFLAGS := -m32 -static-libgcc -static

all: out/d3d9hook.asi verify

out:
	@mkdir -p out

$(MINHOOK)/libMinHook.a:
	$(MAKE) -C $(TOP) minhook

out/d3d9hook.asi: d3d9hook.c $(MINHOOK)/libMinHook.a | out
	$(CC) $(CFLAGS) -shared -o $@ $< $(MINHOOK)/libMinHook.a -ld3d9 $(LDFLAGS)

# An .asi that drags in libgcc_s_dw2-1.dll will fail to load inside the game,
# where that DLL is not on the search path -- and the loader reports only
# "err=126", which says nothing about which import was missing.
verify: out/d3d9hook.asi
	@echo "==> runtime DLL dependencies (libgcc must NOT appear)"
	@$(OBJDUMP) -p $< | grep "DLL Name" | sed 's/^/    /'
	@if $(OBJDUMP) -p $< | grep -qi "libgcc"; then \
	    echo "FAIL: links libgcc at runtime"; exit 1; \
	 else echo "OK: no libgcc runtime dependency"; fi

# Drop into the prefix's plugin directory (Exp. 9: C:\bo1vr, inside the Wine
# prefix, so the game install stays untouched).
# Override PLUGDIR if your Steam library is not the default one.
PLUGDIR ?= $(HOME)/.local/share/Steam/steamapps/compatdata/42700/pfx/drive_c/bo1vr
install: out/d3d9hook.asi
	@mkdir -p $(PLUGDIR)
	cp $< $(PLUGDIR)/
	@echo "installed -> $(PLUGDIR)/d3d9hook.asi"

clean:
	rm -rf out
.PHONY: all verify install clean
