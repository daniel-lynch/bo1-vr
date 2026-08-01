CROSS_PREFIX ?= i686-w64-mingw32-
CC := $(CROSS_PREFIX)gcc
OBJDUMP := $(CROSS_PREFIX)objdump
TOP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)

# -O1 -gdwarf-4 unstripped: Wine's dbghelp clamps at DWARF 4 (README Decision 4)
# and Exp. 3 shows breakpoints and line numbers then work in both winedbg modes.
CFLAGS  := -m32 -O1 -gdwarf-4 -std=gnu11 -Wall
LDFLAGS := -m32 -static-libgcc -static

all: out/bo1probe.asi out/winmm.dll out/dinput8.dll verify

out:
	@mkdir -p out

# The probe. Built as an .asi, i.e. a plain DLL under another extension.
out/bo1probe.asi: bo1probe.c | out
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDFLAGS)

# The bootstrap shim. --kill-at + the .def keep the eleven export names
# undecorated, exactly as BlackOps.exe's import table spells them.
out/winmm.dll: winmm_shim.c winmm.def | out
	$(CC) $(CFLAGS) -shared -o $@ winmm_shim.c winmm.def $(LDFLAGS) -Wl,--kill-at

# The loader itself is the repository artifact, copied unmodified.
out/dinput8.dll: $(TOP)/dist/dinput8.dll | out
	cp $< $@

$(TOP)/dist/dinput8.dll:
	$(MAKE) -C $(TOP)

# Fail the build rather than the game if an export name ever regresses. A
# missing/decorated winmm export means BlackOps.exe dies at load with
# "The procedure entry point ... could not be located".
verify: out/winmm.dll out/bo1probe.asi
	@echo "==> verifying out/winmm.dll exports against BlackOps.exe's import list"
	@for s in mixerClose mixerGetControlDetailsA mixerGetLineControlsA \
	          mixerGetLineInfoA mixerGetNumDevs mixerOpen mixerSetControlDetails \
	          timeBeginPeriod timeEndPeriod timeGetTime waveInGetNumDevs; do \
	    if $(OBJDUMP) -p out/winmm.dll | grep -qE "\] $$s$$"; then \
	        echo "  OK: $$s"; \
	    else \
	        echo "  FAIL: $$s missing or decorated"; exit 1; \
	    fi; \
	done
	@echo "==> bo1probe.asi must export nothing but keep its symbols for gdb"
	@$(OBJDUMP) --dwarf=info out/bo1probe.asi | grep -q 'bo1probe.c' \
	    && echo "  OK: DWARF for bo1probe.c present" || { echo "  FAIL: no debug info"; exit 1; }
	@$(OBJDUMP) -t out/bo1probe.asi | grep -q 'bo1probe_breakpoint_target' \
	    && echo "  OK: bo1probe_breakpoint_target in the symbol table" \
	    || { echo "  FAIL: breakpoint target symbol missing"; exit 1; }

clean:
	rm -rf out

.PHONY: all clean verify
