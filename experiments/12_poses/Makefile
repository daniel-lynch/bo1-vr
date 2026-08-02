# experiments/12_poses -- controller/HMD poses (BAC-283)
#
# Same build shape as 11_gameframe: i686 mingw, -static-libgcc -static, and a
# `verify` target that FAILS the build if libgcc-*.dll shows up as a runtime
# dependency (BlackOps.exe's process must not need a DLL we did not ship).
#
# Three artefacts from one source file:
#   out/poses.o / out/libposes.a  the module gameframe.c and the camera hook link
#   out/poses_selftest.asi        a plugin that reports what it can see
#   out/poses_mathcheck.exe       the coordinate maths, offline, no OpenVR
#
# `make check` runs poses_mathcheck.exe under wine.  That program never loads
# openvr_api.dll and never calls VR_InitInternal2, so it cannot disturb a
# compositor session -- unlike the .asi, which must not be run casually.

CROSS_PREFIX ?= i686-w64-mingw32-
CC      := $(CROSS_PREFIX)gcc
AR      := $(CROSS_PREFIX)ar
OBJDUMP := $(CROSS_PREFIX)objdump
TOP     := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
OPENVR  := $(TOP)/third_party/openvr
EXP04   := $(TOP)/experiments/04_live_fntable

# -isystem for third_party/openvr: openvr_capi.h defines ~40 file-scope
# `static const char *IVR..._Version` strings, so every translation unit that
# includes it emits ~40 -Wunused-variable warnings.  That is the header's noise,
# not ours, and drowning our own warnings in it is how a real one gets missed.
# Our own headers stay on -I so we still see warnings from them.
CFLAGS  := -m32 -O1 -gdwarf-4 -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
           -isystem $(OPENVR)/headers -I$(EXP04)
LDFLAGS := -m32 -static-libgcc -static

all: out/poses.o out/libposes.a out/poses_selftest.asi out/poses_mathcheck.exe \
     out/bo1vr_loader.dll out/openvr_api.dll verify

out:
	@mkdir -p out

# The linkable module.  No OpenVR import library and no libm: openvr_api.dll is
# loaded by name at run time (or not at all, if the host binds its own FnTable).
out/poses.o: poses.c poses.h | out
	$(CC) $(CFLAGS) -c -o $@ $<

out/libposes.a: out/poses.o
	$(AR) rcs $@ $<

out/poses_selftest.asi: poses.c poses.h | out
	$(CC) $(CFLAGS) -DPOSES_SELFTEST -shared -o $@ $< $(LDFLAGS)

out/poses_mathcheck.exe: poses.c poses.h | out
	$(CC) $(CFLAGS) -DPOSES_MATHCHECK -o $@ $< $(LDFLAGS)

out/bo1vr_loader.dll: $(TOP)/dist/dinput8.dll | out
	cp $< $@

$(TOP)/dist/dinput8.dll:
	$(MAKE) -C $(TOP)

out/openvr_api.dll: $(OPENVR)/bin/win32/openvr_api.dll | out
	cp $< $@

verify: out/poses_selftest.asi out/poses_mathcheck.exe out/poses.o
	@echo "==> runtime DLL dependencies (libgcc must NOT appear)"
	@for f in out/poses_selftest.asi out/poses_mathcheck.exe; do \
	    echo "  $$f:"; \
	    $(OBJDUMP) -p $$f | grep "DLL Name" | sed 's/^/      /'; \
	  done
	@if $(OBJDUMP) -p out/poses_selftest.asi | grep -qi libgcc; then \
	    echo "FAIL: libgcc in poses_selftest.asi"; exit 1; fi
	@if $(OBJDUMP) -p out/poses_mathcheck.exe | grep -qi libgcc; then \
	    echo "FAIL: libgcc in poses_mathcheck.exe"; exit 1; fi
	@echo "OK: no libgcc runtime dependency"
	@echo "==> the module must not reference the compositor's frame clock"
	@if $(OBJDUMP) -t out/poses.o | grep -qi waitgetposes; then \
	    echo "FAIL: poses.o references WaitGetPoses"; exit 1; fi
	@echo "OK: no WaitGetPoses reference in poses.o"

# Offline maths only.  Safe while another workstream holds the compositor.
check: out/poses_mathcheck.exe
	wine out/poses_mathcheck.exe

PLUGDIR ?= /mnt/games/steam/steamapps/compatdata/42700/pfx/drive_c/bo1vr
install: out/poses_selftest.asi
	@mkdir -p $(PLUGDIR)
	cp $< $(PLUGDIR)/
	@echo "installed -> $(PLUGDIR)/poses_selftest.asi"
	@echo "NOTE: it does nothing until a marker file exists next to it:"
	@echo "      poses_selftest.on            attach to an existing session"
	@echo "      poses_selftest_standalone.on may CREATE one (takes the compositor)"

clean:
	rm -rf out

.PHONY: all verify check install clean
