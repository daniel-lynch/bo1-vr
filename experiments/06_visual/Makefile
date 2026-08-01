CROSS_PREFIX ?= i686-w64-mingw32-
CC     := $(CROSS_PREFIX)gcc
HOSTCC ?= cc
TOP    := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
OPENVR := $(TOP)/third_party/openvr
EXP04  := $(TOP)/experiments/04_live_fntable
EXP05  := $(TOP)/experiments/05_submit

# ivrsystem_023.h comes from Exp. 4 and d3d9_dxvk.h / vk_min.h from Exp. 5.
# They are the layouts the chain actually speaks (README decisions 10 and 11);
# copying them would let the copies drift.
CFLAGS  := -m32 -O1 -gdwarf-4 -std=c11 -Wall -I$(OPENVR)/headers -I$(EXP04) -I$(EXP05)
LDFLAGS := -m32 -static-libgcc -static

LAYER := out/libVkLayer_bo1vr_xlib_surface.so

all: out/visual.dll out/host.exe out/openvr_api.dll $(LAYER) out/VkLayer_bo1vr_xlib_surface.json

out:
	@mkdir -p out

out/visual.dll: visual.c $(EXP05)/vk_min.h $(EXP05)/d3d9_dxvk.h | out
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDFLAGS)

out/host.exe: host.c | out
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

out/openvr_api.dll: $(OPENVR)/bin/win32/openvr_api.dll | out
	cp $< $@

# The observation channel itself -- a native x86-64 Vulkan layer, NOT part of
# the 32-bit chain under test. It is loaded only by monado-service, and only
# because run.sh points VK_LAYER_PATH at this directory. See vkxlibsurface.c.
$(LAYER): vkxlibsurface.c | out
	$(HOSTCC) -O2 -Wall -Wextra -fPIC -shared -o $@ $<

out/VkLayer_bo1vr_xlib_surface.json: VkLayer_bo1vr_xlib_surface.json | out
	cp $< $@

clean:
	rm -rf out

.PHONY: all clean
