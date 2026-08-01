CROSS_PREFIX ?= i686-w64-mingw32-
CC := $(CROSS_PREFIX)gcc
TOP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
OPENVR := $(TOP)/third_party/openvr

CFLAGS  := -m32 -O1 -gdwarf-4 -std=c11 -Wall -I$(OPENVR)/headers
LDFLAGS := -m32 -static-libgcc -static

all: out/vrlive.dll out/host.exe out/openvr_api.dll

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

.PHONY: all clean
