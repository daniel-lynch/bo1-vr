CROSS_PREFIX ?= i686-w64-mingw32-
CC := $(CROSS_PREFIX)gcc
TOP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)

CFLAGS  := -m32 -O1 -gdwarf-4 -std=c11 -Wall
LDFLAGS := -m32 -static-libgcc -static

all: out/smoke.exe out/dummy.asi out/dinput8.dll

out:
	@mkdir -p out

out/smoke.exe: smoke.c | out
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lole32

out/dummy.asi: dummy.c | out
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDFLAGS)

# The loader under test, straight from the top-level build.
out/dinput8.dll: $(TOP)/dist/dinput8.dll | out
	cp $< $@
$(TOP)/dist/dinput8.dll:
	$(MAKE) -C $(TOP)

# Under PLAIN WINE the override is required, because Wine prefers its builtin
# dinput8 over a native DLL in the application directory. Under Proton 10/11 it
# is NOT required -- dinput8 is already listed prefer-native. See RESULTS.md.
run: all
	cd out && WINEDLLOVERRIDES="dinput8=n,b" wine smoke.exe

# Demonstrates the failure mode of a bare "n". Expect err=126 and E_FAIL.
run-bare-n: all
	cd out && WINEDLLOVERRIDES="dinput8=n" wine smoke.exe

clean:
	rm -rf out

.PHONY: all clean run run-bare-n
