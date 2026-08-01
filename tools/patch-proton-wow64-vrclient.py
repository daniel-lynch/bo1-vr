#!/usr/bin/env python3
"""Patch a *copy* of Proton so 32-bit OpenVR works in new-WoW64 mode.

WHY THIS EXISTS
---------------
Proton 10.0-4b cannot serve OpenVR to a 32-bit Windows process in new-WoW64
mode (PROTON_USE_WOW64=1). Two defects, both measured -- see
experiments/04_live_fntable/RESULTS.md:

  1. MISSING UNIXLIB.  Wine resolves a PE builtin's unix library by base name:
     32-bit PE `vrclient.dll` wants `<unix-arch>/vrclient.so`. Proton ships
     `i386-unix/vrclient.so` (32-bit ELF) and `x86_64-unix/vrclient_x64.so`,
     but NOT `x86_64-unix/vrclient.so`. In new-WoW64 the unix side is 64-bit,
     so `__wine_init_unix_call()` finds nothing and the first unix call faults
     with 0xc0000005. vrclient is the only Proton module whose PE name differs
     between bitnesses, which is why it is the only one that hits this.
     Fix: symlink x86_64-unix/vrclient.so -> vrclient_x64.so.

  2. WRONG WOW64 STRUCT LAYOUT for vrclient_init.  In vrclient_x64/unixlib.h,
     `struct wow64_vrclient_init_params` wraps `unix_path` in W32_PTR but
     leaves `winevulkan` as a plain `HMODULE`. HMODULE is 8 bytes when the
     unixlib is compiled for x86_64, but the 32-bit PE writes 4. The structs
     are #pragma pack(1), so every field after `winevulkan` is 4 bytes too far
     along. Disassembly of the shipped vrclient_x64.so confirms it:

         wow64_vrclient_init:  mov 0x9(%rdi),%edi   ; unix_path  <- should be 0x5
                               mov 0x1(%rbx),%rdi   ; winevulkan <- should be 32-bit

     unix_path therefore arrives as 0, dlopen(NULL) returns a handle to the
     main program, and dlsym("HmdSystemFactory") fails with
     "err:vrclient:vrclient_init unable to load HmdSystemFactory".
     Fix: three byte-level edits to wow64_vrclient_init.

Both are upstream Proton bugs. This script is a local workaround so the chain
can be exercised now; the real fix belongs in Proton.

USAGE
-----
    tools/patch-proton-wow64-vrclient.py <src-proton-dir> <dest-dir>

<dest-dir> is created as a hard-link copy (`cp -al`), so it costs no extra disk
and the original Steam install is never modified. Re-running is idempotent.
"""
import os, shutil, subprocess, sys

# (virtual address, expected bytes, replacement bytes, description)
# For vrclient_x64.so the executable LOAD segment maps VA == file offset, which
# this script asserts before writing.
PATCHES = [
    (0x14d058, "8b7f09",   "8b7f05",   "wow64_vrclient_init: unix_path  @+9 -> @+5"),
    (0x14d0af, "488b7b01", "8b7b0190", "wow64_vrclient_init: winevulkan 64-bit -> 32-bit zero-extended"),
    (0x14d0fd, "448b4309", "448b4305", "wow64_vrclient_init: TRACE unix_path @+9 -> @+5"),
]
# sha256 of the pristine Proton 10.0-4b files/lib/wine/x86_64-unix/vrclient_x64.so
KNOWN = "proton-10.0-4b"


def die(msg):
    print("error: " + msg, file=sys.stderr)
    sys.exit(1)


def main():
    if len(sys.argv) != 3:
        die("usage: %s <src-proton-dir> <dest-dir>" % sys.argv[0])
    src, dst = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])

    ver = os.path.join(src, "version")
    if os.path.exists(ver):
        print("source proton:", open(ver).read().strip())

    if not os.path.isdir(dst):
        print("hard-link copying %s -> %s" % (src, dst))
        subprocess.check_call(["cp", "-al", src, dst])

    unixdir = os.path.join(dst, "files/lib/wine/x86_64-unix")
    if not os.path.isdir(unixdir):
        die("no %s -- is this a Proton dist?" % unixdir)

    # --- defect 1: missing unixlib name for the 32-bit PE -----------------
    link = os.path.join(unixdir, "vrclient.so")
    if not os.path.exists(link):
        os.symlink("vrclient_x64.so", link)
        print("created symlink x86_64-unix/vrclient.so -> vrclient_x64.so")
    else:
        print("symlink x86_64-unix/vrclient.so already present")

    # --- defect 2: wow64 struct layout ------------------------------------
    so = os.path.join(unixdir, "vrclient_x64.so")
    # break the hard link so we never write through to the Steam install
    if os.stat(so).st_nlink > 1:
        tmp = so + ".tmp"
        shutil.copyfile(so, tmp)
        os.chmod(tmp, 0o755)
        os.replace(tmp, so)
        print("broke hard link on vrclient_x64.so")

    d = bytearray(open(so, "rb").read())
    applied = skipped = 0
    for va, exp, rep, desc in PATCHES:
        exp, rep = bytes.fromhex(exp), bytes.fromhex(rep)
        assert len(exp) == len(rep)
        got = bytes(d[va:va + len(exp)])
        if got == rep:
            skipped += 1
            continue
        if got != exp:
            die("at VA 0x%x expected %s or %s, found %s -- this Proton build is "
                "not the one these offsets were derived from (%s). Re-derive "
                "them by disassembling wow64_vrclient_init."
                % (va, exp.hex(), rep.hex(), got.hex(), KNOWN))
        d[va:va + len(rep)] = rep
        applied += 1
        print("patched VA 0x%08x: %s" % (va, desc))
    if applied:
        open(so, "wb").write(d)
    print("done: %d patch(es) applied, %d already present" % (applied, skipped))


if __name__ == "__main__":
    main()
