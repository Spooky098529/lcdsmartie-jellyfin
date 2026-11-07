# Cross-compiles a 64-bit jellyfin.dll on Linux with clang + lld-link, using the
# MSVC CRT and Windows SDK downloaded by xwin (see README).
#
# Targets Windows 7+: the CRT is linked statically (/MT) because Win7 has no
# Universal CRT, and the PE is marked subsystem 6.01. `make check` lists the
# imports so nothing newer than Win7 sneaks in.
#
# Set WINSYSROOT to your xwin splat directory, on the command line or in a
# (git-ignored) config.mk:   WINSYSROOT = /path/to/xwin-sdk

-include config.mk
WINSYSROOT ?= xwin-sdk

CC      = clang
CFLAGS  = --target=x86_64-pc-windows-msvc -Xmicrosoft-windows-sys-root $(WINSYSROOT) \
          -fms-runtime-lib=static -O2 -Wall -Wextra \
          -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -D_CRT_SECURE_NO_WARNINGS
LDFLAGS = -fuse-ld=lld -shared \
          -Xlinker /subsystem:windows,6.01 -Wl,/opt:ref -Wl,/noimplib
LIBS    = -lwinhttp

jellyfin.dll: jellyfin.c
	@test -d "$(WINSYSROOT)/VC" || { echo "WINSYSROOT=$(WINSYSROOT) is not an xwin splat (see README)"; exit 1; }
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

check: jellyfin.dll
	llvm-readobj --coff-imports jellyfin.dll | grep -E 'Name:|Symbol:'
	llvm-readobj --coff-exports jellyfin.dll | grep 'Name:'

clean:
	rm -f jellyfin.dll jellyfin.lib jellyfin.exp

.PHONY: check clean
