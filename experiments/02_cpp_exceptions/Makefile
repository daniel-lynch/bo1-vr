CROSS_PREFIX ?= i686-w64-mingw32-
CXX := $(CROSS_PREFIX)g++
CC  := $(CROSS_PREFIX)gcc

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
STATIC_LD := -static-libgcc -static-libstdc++ -static
SHARED_LD :=

all: static shared
static: out-static/host2.exe out-static/throwlib.dll
shared: out-shared/host2.exe out-shared/throwlib.dll out-shared/.runtime

out-static out-shared:
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

out-shared/.runtime: out-shared/host2.exe
	@for d in libgcc_s_dw2-1.dll libstdc++-6.dll libwinpthread-1.dll; do \
		p=$$($(CXX) -print-file-name=$$d); \
		if [ -f "$$p" ]; then cp -f "$$p" out-shared/ && echo "  copied $$d"; \
		else echo "  NOT FOUND: $$d"; fi; \
	done
	@touch $@

clean:
	rm -rf out-static out-shared

.PHONY: all clean static shared
