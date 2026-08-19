CROSS_PREFIX ?= i686-w64-mingw32-
CC := $(CROSS_PREFIX)gcc
OBJDUMP := $(CROSS_PREFIX)objdump
TOP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
MINHOOK := $(TOP)/third_party/minhook
OPENVR  := $(TOP)/third_party/openvr
EXP04   := $(TOP)/experiments/04_live_fntable
POSES   := $(TOP)/experiments/12_poses
HEADTRK := $(TOP)/experiments/14_headtrack

# poses.c and headtrack.c are compiled here, into THIS experiment's out/, from
# sources that are not modified.  12_poses is another workstream's directory and
# nothing is written into it.
#
# -isystem for third_party/openvr: openvr_capi.h emits ~40 -Wunused-variable
# warnings of its own per translation unit (12_poses/Makefile explains), and
# drowning our warnings in the header's noise is how a real one gets missed.
CFLAGS  := -m32 -O1 -gdwarf-4 -std=gnu11 -Wall -I$(MINHOOK)/include \
           -I$(POSES) -I$(HEADTRK) -I$(EXP04) -isystem $(OPENVR)/headers
LDFLAGS := -m32 -static-libgcc -static

OBJS := out/poses.o out/headtrack.o

all: out/camera.asi verify

out:
	@mkdir -p out

$(MINHOOK)/libMinHook.a:
	$(MAKE) -C $(TOP) minhook

out/poses.o: $(POSES)/poses.c $(POSES)/poses.h | out
	$(CC) $(CFLAGS) -c -o $@ $<

out/headtrack.o: $(HEADTRK)/headtrack.c $(HEADTRK)/headtrack.h | out
	$(CC) $(CFLAGS) -Wextra -c -o $@ $<

out/camera.asi: camera.c $(OBJS) $(MINHOOK)/libMinHook.a | out
	$(CC) $(CFLAGS) -shared -o $@ $< $(OBJS) $(MINHOOK)/libMinHook.a $(LDFLAGS)

verify: out/camera.asi
	@echo "==> the two hand-written thunks must survive optimisation intact"
	@$(OBJDUMP) -d out/camera.asi | grep -A6 "<_hk_R_SetViewParms>:" | sed 's/^/    /'
	@$(OBJDUMP) -d out/camera.asi | grep -A9 "<_call_original>:" | sed 's/^/    /'
	@echo "==> runtime DLL dependencies (libgcc must NOT appear)"
	@$(OBJDUMP) -p out/camera.asi | grep "DLL Name" | sed 's/^/    /'
	@if $(OBJDUMP) -p out/camera.asi | grep -qi libgcc; then echo "FAIL: libgcc"; exit 1; \
	 else echo "OK: no libgcc runtime dependency"; fi
	@echo "==> openvr_api.dll must be resolved at RUN time, never imported"
	@if $(OBJDUMP) -p out/camera.asi | grep -qi "DLL Name: openvr"; then \
	   echo "FAIL: camera.asi imports openvr_api.dll"; exit 1; \
	 else echo "OK: no OpenVR import"; fi
	@echo "==> WaitGetPoses must NOT appear: gameframe.asi owns the frame clock"
	@if $(OBJDUMP) -s -j .rdata out/camera.asi 2>/dev/null | grep -qi "WaitGetPoses"; then \
	   echo "FAIL: a WaitGetPoses reference reached camera.asi"; exit 1; \
	 else echo "OK: no WaitGetPoses reference"; fi
	@echo "==> the head-orientation maths must pass its own check"
	@$(MAKE) -s -C $(HEADTRK) check

# Override PLUGDIR if your Steam library is not the default one.
PLUGDIR ?= $(HOME)/.local/share/Steam/steamapps/compatdata/42700/pfx/drive_c/bo1vr
install: out/camera.asi
	@mkdir -p $(PLUGDIR)
	cp $< $(PLUGDIR)/
	@echo "installed -> $(PLUGDIR)/camera.asi"

clean:
	rm -rf out
.PHONY: all verify install clean
