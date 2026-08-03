# experiments/14_headtrack -- the HMD-orientation maths for the camera hook.
#
# Two artefacts from one source file:
#   out/headtrack.o                the object experiments/13_camera links
#   out/headtrack_mathcheck.exe    the same maths, offline, `make check`
#
# The .exe imports KERNEL32 and msvcrt only.  It never loads openvr_api.dll,
# never touches a compositor, and never starts a game -- so `make check` is safe
# to run while BlackOps.exe is up.

CROSS_PREFIX ?= i686-w64-mingw32-
CC      := $(CROSS_PREFIX)gcc
AR      := $(CROSS_PREFIX)ar
OBJDUMP := $(CROSS_PREFIX)objdump

CFLAGS  := -m32 -O1 -gdwarf-4 -std=gnu11 -Wall -Wextra
LDFLAGS := -m32 -static-libgcc -static

all: out/headtrack.o out/headtrack_mathcheck.exe verify

out:
	@mkdir -p out

out/headtrack.o: headtrack.c headtrack.h | out
	$(CC) $(CFLAGS) -c -o $@ $<

out/headtrack_mathcheck.exe: headtrack.c headtrack.h | out
	$(CC) $(CFLAGS) -DHEADTRACK_MATHCHECK -o $@ $< $(LDFLAGS)

verify: out/headtrack_mathcheck.exe
	@echo "==> runtime DLL dependencies (libgcc must NOT appear)"
	@$(OBJDUMP) -p out/headtrack_mathcheck.exe | grep "DLL Name" | sed 's/^/    /'
	@if $(OBJDUMP) -p out/headtrack_mathcheck.exe | grep -qi libgcc; then \
	   echo "FAIL: libgcc"; exit 1; else echo "OK: no libgcc runtime dependency"; fi
	@if $(OBJDUMP) -p out/headtrack_mathcheck.exe | grep -qi openvr; then \
	   echo "FAIL: the offline check must not link OpenVR"; exit 1; \
	 else echo "OK: no OpenVR in the offline check"; fi

WINE ?= wine
check: out/headtrack_mathcheck.exe
	@WINEDEBUG=-all $(WINE) out/headtrack_mathcheck.exe

clean:
	rm -rf out
.PHONY: all verify check clean
