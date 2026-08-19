# bo1-vr -- 32-bit dinput8.dll ASI loader for Call of Duty: Black Ops (2010)
#
# WHY A MAKEFILE AND NOT CMAKE
# ----------------------------
# CMake earns its keep when a project has several configurations, several
# platforms, or dependencies that must be discovered. This project has exactly
# one target triple, forever: i686-w64-mingw32. It will never be built for
# x86_64, never for Linux, never for MSVC. Under those constraints CMake adds a
# toolchain file, a generator, a cache and a build directory in exchange for
# nothing.
#
# It also actively hurts here. MinHook must be built through its own
# build/MinGW/Makefile (see the MINHOOK section below). The natural CMake idiom
# for a vendored dependency is add_subdirectory(third_party/minhook), which pulls
# in MinHook's CMakeLists.txt -- precisely the path that is broken for us. A
# plain Makefile makes the correct call the obvious one and the wrong call
# something you would have to go out of your way to write.
#
# USAGE
#   make            build dist/dinput8.dll
#   make install    copy dist/dinput8.dll next to BlackOps.exe (set BO1_DIR)
#   make clean
#
# Override the toolchain location if it is not on PATH:
#   make CROSS_PREFIX=/path/to/i686-w64-mingw32-

CROSS_PREFIX ?= i686-w64-mingw32-

CC      := $(CROSS_PREFIX)gcc
AR      := $(CROSS_PREFIX)ar
OBJDUMP := $(CROSS_PREFIX)objdump
STRIP   := $(CROSS_PREFIX)strip

TOP     := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
SRCDIR  := $(TOP)/src
BUILD   := $(TOP)/build
DIST    := $(TOP)/dist
MINHOOK := $(TOP)/third_party/minhook

TARGET  := $(DIST)/dinput8.dll
DEFFILE := $(TOP)/dinput8.def

SRCS := $(wildcard $(SRCDIR)/*.c)
OBJS := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------
#
# -m32
#     BlackOps.exe is a 32-bit PE. Non-negotiable.
#
# -gdwarf-4  AND  no -s / no strip
#     THIS IS THE SUBTLE ONE. Wine's dbghelp hard-clamps
#     max_supported_dwarf_version to 4. GCC 11+ and Clang 14+ emit DWARF 5 by
#     default (this tree's compiler is GCC 13, so the default *is* 5). Build
#     without -gdwarf-4 and Wine does not warn, does not degrade -- it simply
#     finds no symbols at all, and you spend a day assuming your DLL never
#     loaded. Similarly, MinHook's own MinGW makefile passes -s in its LDFLAGS;
#     we override CFLAGS/LDFLAGS when invoking it for exactly that reason.
#
# -static-libgcc -static
#     Otherwise the DLL acquires a runtime dependency on libgcc_s_dw2-1.dll,
#     which will not be sitting next to BlackOps.exe, and the load fails with a
#     bare "module not found" that names our DLL rather than the missing one.
#
# -Wl,--kill-at
#     mingw decorates __stdcall exports as _Name@N. The game imports plain
#     "DirectInput8Create". --kill-at strips the decoration so the .def names
#     land undecorated. Verified by `make verify`.
#
# -Wl,--no-insert-timestamp
#     Reproducible builds; also stops Wine's PE loader cache from being confused
#     by rebuilt-but-identical DLLs.

CPPFLAGS := -I$(SRCDIR) -I$(MINHOOK)/include
CFLAGS   := -m32 -O2 -gdwarf-4 -std=c11 \
            -Wall -Wextra -Wno-unused-parameter \
            -fno-strict-aliasing

LDFLAGS  := -m32 -shared -static-libgcc -static \
            -Wl,--kill-at \
            -Wl,--enable-stdcall-fixup \
            -Wl,--no-insert-timestamp
LDLIBS   := -lkernel32 -luser32 -lole32

MINHOOK_LIB := $(MINHOOK)/libMinHook.a

# ---------------------------------------------------------------------------
.PHONY: all clean verify install minhook toolchain-check
all: $(TARGET) verify

$(BUILD) $(DIST):
	@mkdir -p $@

# ---------------------------------------------------------------------------
# MINHOOK
# ---------------------------------------------------------------------------
# Built through MinHook's own build/MinGW/Makefile with CROSS_PREFIX, NOT
# through its CMakeLists.txt.
#
# Reason: MinHook's CMake picks hde32.c vs hde64.c based on CMAKE_SIZEOF_VOID_P,
# which reflects the *host* under a cross compiler and therefore selects the
# 64-bit disassembler for our 32-bit build. The result compiles and links and
# then mis-decodes every prologue at runtime.
#
# The MinGW makefile instead globs `src/*.c src/hde/*.c` and compiles both
# hde32.c and hde64.c. That is correct because each file self-guards:
#   hde32.c: #if defined(_M_IX86) || defined(__i386__)
#   hde64.c: #if defined(_M_X64)  || defined(__x86_64__)
# so under i686-w64-mingw32 hde64.c compiles to an empty object and hde32.c is
# the one that survives. The preprocessor decides, not the build system.
#
# Note the makefile lives in build/MinGW but its paths are relative to the
# MinHook root, so it must be invoked with -f from the root.
#
# We override CFLAGS to add -gdwarf-4 and drop -Werror (GCC 13 is stricter than
# the code's vintage), and keep -masm=intel which the sources require.
$(MINHOOK_LIB):
	@echo "==> building MinHook (MinGW makefile, CROSS_PREFIX=$(CROSS_PREFIX))"
	$(MAKE) -C $(MINHOOK) -f build/MinGW/Makefile libMinHook.a \
		CROSS_PREFIX=$(CROSS_PREFIX) \
		CFLAGS="-m32 -masm=intel -O2 -gdwarf-4 -std=c11 -Wall"

minhook: $(MINHOOK_LIB)

# ---------------------------------------------------------------------------
$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(TARGET): $(OBJS) $(MINHOOK_LIB) $(DEFFILE) | $(DIST)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(DEFFILE) $(MINHOOK_LIB) $(LDLIBS)
	@echo "==> built $@"

# ---------------------------------------------------------------------------
# verify: fail loudly if any of the three easy-to-get-wrong properties regress.
verify: $(TARGET)
	@echo "==> verifying $(TARGET)"
	@echo "--- machine type (must be i386) ---"
	@file $(TARGET) | grep -q "PE32 executable (DLL).*Intel 80386" \
		&& echo "OK: 32-bit PE" || { echo "FAIL: not a 32-bit PE"; exit 1; }
	@echo "--- exports (must be undecorated, all five) ---"
	@$(OBJDUMP) -p $(TARGET) | sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$$/p'
	@for s in DirectInput8Create DllCanUnloadNow DllGetClassObject \
	          DllRegisterServer DllUnregisterServer; do \
		$(OBJDUMP) -p $(TARGET) | grep -qE "\] $$s$$" \
			&& echo "OK: $$s" \
			|| { echo "FAIL: export $$s missing or decorated"; exit 1; }; \
	done
	@echo "--- debug info (our CUs must be DWARF <= 4 for Wine dbghelp) ---"
	@$(OBJDUMP) --dwarf=info $(TARGET) 2>/dev/null > $(BUILD)/dwarf.txt || true
	@test -s $(BUILD)/dwarf.txt || { echo "FAIL: no DWARF info at all (stripped?)"; exit 1; }
	@# Pair each CU's version with its DW_AT_name, then judge only the CUs we compiled.
	@awk '/Compilation Unit @/{v="";n=""} \
	      /Version:/{if(v=="")v=$$2} \
	      /DW_AT_name/{if(n==""){n=$$NF; print v"\t"n}}' $(BUILD)/dwarf.txt > $(BUILD)/cus.txt
	@# Identify OUR CUs positively rather than blocklisting toolchain paths:
	@# ours are absolute paths under $(TOP), plus MinHook's relative "src/...".
	@# Everything else came out of the distro's prebuilt mingw-w64 (crt,
	@# winpthreads, libgcc) and no compiler flag of ours can change its version.
	@bad=$$(awk -F'\t' -v top="$(TOP)" \
	        '$$1>4 && (index($$2, top)==1 || $$2 ~ /^src\//) {print}' $(BUILD)/cus.txt); \
	 if [ -n "$$bad" ]; then \
		echo "FAIL: our own CUs emitted DWARF > 4 -- Wine dbghelp will not read them:"; \
		echo "$$bad"; exit 1; \
	 fi
	@echo "OK: all first-party + MinHook CUs are DWARF 4"
	@ext=$$(awk -F'\t' -v top="$(TOP)" \
	        '$$1>4 && !(index($$2, top)==1 || $$2 ~ /^src\//)' $(BUILD)/cus.txt | wc -l); \
	 if [ "$$ext" -gt 0 ]; then \
		echo "NOTE: $$ext CU(s) from the distro's prebuilt mingw-w64 runtime are DWARF 5"; \
		echo "      (mingw-w64-crt, winpthreads, libgcc). These ship precompiled;"; \
		echo "      -gdwarf-4 cannot change them. Wine's dbghelp skips over-version"; \
		echo "      CUs individually, so our own symbols still resolve --"; \
		echo "      see experiments/03_winedbg/RESULTS.md."; \
	 fi
	@echo "--- runtime DLL dependencies (libgcc must NOT appear) ---"
	@$(OBJDUMP) -p $(TARGET) | grep "DLL Name:" | sort -u
	@$(OBJDUMP) -p $(TARGET) | grep -qi "libgcc" \
		&& { echo "FAIL: depends on libgcc DLL"; exit 1; } \
		|| echo "OK: no libgcc runtime dependency"
	@echo "==> all checks passed"

# ---------------------------------------------------------------------------
toolchain-check:
	@command -v $(CC) >/dev/null 2>&1 \
		&& { echo "OK: $(CC) -> $$($(CC) -dumpversion)"; } \
		|| { echo "MISSING: $(CC)"; \
		     echo "Install with:"; \
		     echo "  sudo apt install gcc-mingw-w64-i686 g++-mingw-w64-i686 binutils-mingw-w64-i686"; \
		     exit 1; }

# BO1_DIR should point at the directory containing BlackOps.exe.
#
# NOTE: this is the legacy bench route, and it writes into the game directory.
# The shipping route writes nothing there: the winmm shim goes into the Proton
# prefix instead -- use experiments/09_noinstall/install.sh (see Exp. 7/9).
BO1_DIR ?= $(HOME)/.local/share/Steam/steamapps/common/Call of Duty Black Ops
install: $(TARGET)
	@test -d "$(BO1_DIR)" || { echo "BO1_DIR does not exist: $(BO1_DIR)"; exit 1; }
	cp $(TARGET) "$(BO1_DIR)/dinput8.dll"
	@echo "==> installed to $(BO1_DIR)/dinput8.dll"
	@echo "    No WINEDLLOVERRIDES needed: Proton 10/11 already prefer-native dinput8."

clean:
	rm -rf $(BUILD) $(DIST)
	$(MAKE) -C $(MINHOOK) -f build/MinGW/Makefile clean CROSS_PREFIX=$(CROSS_PREFIX) || true
	rm -f $(MINHOOK_LIB)

-include $(DEPS)
