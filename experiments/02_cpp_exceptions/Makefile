CROSS_PREFIX ?= i686-w64-mingw32-
CXX     := $(CROSS_PREFIX)g++
CC      := $(CROSS_PREFIX)gcc
OBJDUMP := $(CROSS_PREFIX)objdump

BASEFLAGS := -m32 -O1 -gdwarf-4 -Wall -Wno-format-zero-length

# Two link variants, because the answer may differ between them and the
# difference is a decision we have to make for the real plugins:
#
#   out-static/ : -static-libgcc -static-libstdc++ -static
#             Each module carries its own unwinder and its own copy of the
#             std:: type_info objects. This is what the ASI loader uses (so it
#             has no external DLL dependencies at all), and it is the
#             configuration most likely to break cross-module catch.
#
#   out-shared/ : libgcc_s_dw2-1.dll + libstdc++-6.dll alongside
#             One unwinder, one set of type_info. Standard C++ semantics, at the
#             cost of shipping two extra DLLs next to BlackOps.exe.
#
#   out-noeh/ : shared, but cframe.c built without unwind tables. GCC emits
#             .eh_frame for plain C on i686 by default, so the shared build's
#             case D only proves unwinding through a *GCC* C frame. Suppressing
#             the tables is the closest proxy we have for an MSVC game frame.
STATIC_LD := -static-libgcc -static-libstdc++ -static
SHARED_LD :=
NOEH_CFLAGS := -fno-asynchronous-unwind-tables -fno-unwind-tables

all: static shared noeh
static: out-static/host2.exe out-static/throwlib.dll
shared: out-shared/host2.exe out-shared/throwlib.dll out-shared/.runtime
noeh:   out-noeh/host2.exe   out-noeh/throwlib.dll   out-noeh/.runtime

out-static out-shared out-noeh:
	@mkdir -p $@

out-static/cframe.o: cframe.c | out-static
	$(CC) $(BASEFLAGS) -c -o $@ $<
out-shared/cframe.o: cframe.c | out-shared
	$(CC) $(BASEFLAGS) -c -o $@ $<

out-static/throwlib.dll: throwlib.cpp out-static/cframe.o | out-static
	$(CXX) $(BASEFLAGS) -shared -o $@ throwlib.cpp out-static/cframe.o $(STATIC_LD)
out-static/host2.exe: host2.cpp | out-static
	$(CXX) $(BASEFLAGS) -o $@ $< $(STATIC_LD)

out-shared/throwlib.dll: throwlib.cpp out-shared/cframe.o | out-shared
	$(CXX) $(BASEFLAGS) -shared -o $@ throwlib.cpp out-shared/cframe.o $(SHARED_LD)
out-shared/host2.exe: host2.cpp | out-shared
	$(CXX) $(BASEFLAGS) -o $@ $< $(SHARED_LD)

out-noeh/cframe.o: cframe.c | out-noeh
	$(CC) $(BASEFLAGS) $(NOEH_CFLAGS) -c -o $@ $<
	@$(OBJDUMP) -h $@ | grep -q eh_frame \
		&& { echo "FAIL: $@ still has .eh_frame"; exit 1; } \
		|| echo "  OK: $@ has no .eh_frame"
out-noeh/throwlib.dll: throwlib.cpp out-noeh/cframe.o | out-noeh
	$(CXX) $(BASEFLAGS) -shared -o $@ throwlib.cpp out-noeh/cframe.o $(SHARED_LD)
out-noeh/host2.exe: host2.cpp | out-noeh
	$(CXX) $(BASEFLAGS) -o $@ $< $(SHARED_LD)

# Copy exactly the runtime DLLs the binaries actually import, transitively --
# do NOT hardcode the list. Which DLLs those are depends on the compiler's
# threading model: GCC 13-posix's libgcc/libstdc++ import libwinpthread-1.dll,
# GCC 13-win32's do not. Reading the import tables keeps this correct either way.
out-%/.runtime: out-%/host2.exe out-%/throwlib.dll
	@d=$(@D); \
	 pending="$$($(OBJDUMP) -p $$d/host2.exe $$d/throwlib.dll \
	              | sed -n 's/^\tDLL Name: \(lib.*\)$$/\1/p' | sort -u)"; \
	 done_list=""; \
	 while [ -n "$$pending" ]; do \
		next=""; \
		for n in $$pending; do \
			case " $$done_list " in *" $$n "*) continue;; esac; \
			done_list="$$done_list $$n"; \
			p=$$($(CXX) -print-file-name=$$n); \
			if [ -f "$$p" ]; then \
				cp -f "$$p" $$d/ && echo "  copied $$n"; \
				next="$$next $$($(OBJDUMP) -p $$d/$$n \
				         | sed -n 's/^\tDLL Name: \(lib.*\)$$/\1/p')"; \
			else echo "  NOT FOUND: $$n"; fi; \
		done; \
		pending="$$(echo $$next | tr ' ' '\n' | sort -u)"; \
	 done
	@touch $@

clean:
	rm -rf out-static out-shared out-noeh

.PHONY: all clean static shared noeh
