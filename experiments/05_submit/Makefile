CROSS_PREFIX ?= i686-w64-mingw32-
CC := $(CROSS_PREFIX)gcc
TOP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
OPENVR := $(TOP)/third_party/openvr
EXP04  := $(TOP)/experiments/04_live_fntable

CFLAGS  := -m32 -O1 -gdwarf-4 -std=c11 -Wall -I$(OPENVR)/headers -I$(EXP04)
LDFLAGS := -m32 -static-libgcc -static

all: out/vrsubmit.dll out/host.exe out/openvr_api.dll

out:
	@mkdir -p out

out/vrsubmit.dll: vrsubmit.c vk_min.h d3d9_dxvk.h | out
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDFLAGS)

out/host.exe: host.c | out
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

out/openvr_api.dll: $(OPENVR)/bin/win32/openvr_api.dll | out
	cp $< $@

# The same probe in the shape it will really ship: an .asi picked up by the
# repo's own dist/dinput8.dll loader. `make asi`, then run out/asi/asihost.exe.
asi: out/asi/vrsubmit.asi out/asi/asihost.exe out/asi/dinput8.dll out/asi/openvr_api.dll

out/asi:
	@mkdir -p out/asi

out/asi/vrsubmit.asi: vrsubmit.c vk_min.h d3d9_dxvk.h | out/asi
	$(CC) $(CFLAGS) -DBO1VR_ASI -shared -o $@ $< $(LDFLAGS)

out/asi/asihost.exe: asihost.c | out/asi
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

out/asi/dinput8.dll: $(TOP)/dist/dinput8.dll | out/asi
	cp $< $@

$(TOP)/dist/dinput8.dll:
	$(MAKE) -C $(TOP)

out/asi/openvr_api.dll: $(OPENVR)/bin/win32/openvr_api.dll | out/asi
	cp $< $@

# vk_min.h is hand-written and layout-critical. This target compiles it side by
# side with the system Vulkan headers and _Static_assert()s that every size,
# offset and enumerant agrees. Needs libvulkan-dev, which nothing else does, so
# it is deliberately not part of `all`. -idirafter (not -I) keeps mingw's own
# stdint.h ahead of glibc's; only vulkan/*.h is picked up from /usr/include.
vkcheck: | out
	@test -f /usr/include/vulkan/vulkan_core.h || \
	  { echo "vkcheck: needs libvulkan-dev (/usr/include/vulkan/vulkan_core.h)"; exit 1; }
	$(CC) -m32 -std=c11 -Wall -Wextra -idirafter /usr/include -c vkcheck.c -o out/vkcheck.o
	@echo "vkcheck: vk_min.h agrees with vulkan_core.h VK_HEADER_VERSION" \
	  "$$(awk '/^#define VK_HEADER_VERSION / {print $$3; exit}' /usr/include/vulkan/vulkan_core.h)"

clean:
	rm -rf out

.PHONY: all asi vkcheck clean
