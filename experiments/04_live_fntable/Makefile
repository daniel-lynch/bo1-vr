CROSS_PREFIX ?= i686-w64-mingw32-
CC := $(CROSS_PREFIX)gcc
TOP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
OPENVR := $(TOP)/third_party/openvr

CFLAGS  := -m32 -O1 -gdwarf-4 -std=c11 -Wall -I$(OPENVR)/headers
LDFLAGS := -m32 -static-libgcc -static

all: out/vrlive.dll out/host.exe out/openvr_api.dll

# The same probe in the shape it will really ship: an .asi picked up by the
# repo's own dist/dinput8.dll loader. `make asi` then run out/asi/asihost.exe.
asi: out/asi/vrlive.asi out/asi/asihost.exe out/asi/dinput8.dll out/asi/openvr_api.dll

out/asi:
	@mkdir -p out/asi

out/asi/vrlive.asi: vrlive.c | out/asi
	$(CC) $(CFLAGS) -DBO1VR_ASI -shared -o $@ $< $(LDFLAGS)

out/asi/asihost.exe: asihost.c | out/asi
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

out/asi/dinput8.dll: $(TOP)/dist/dinput8.dll | out/asi
	cp $< $@

$(TOP)/dist/dinput8.dll:
	$(MAKE) -C $(TOP)

out/asi/openvr_api.dll: $(OPENVR)/bin/win32/openvr_api.dll | out/asi
	cp $< $@

out:
	@mkdir -p out

out/vrlive.dll: vrlive.c | out
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDFLAGS)

out/host.exe: host.c | out
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

out/openvr_api.dll: $(OPENVR)/bin/win32/openvr_api.dll | out
	cp $< $@

clean:
	rm -rf out

.PHONY: all asi clean
