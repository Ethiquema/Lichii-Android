#!/usr/bin/env python3
"""Symbolizes the last dump of files/alloc_profile.txt (EDEN_NX2_ALLOC_PROFILE).

Usage: alloc_profile_top.py <alloc_profile.txt> <unstripped libyuzu-android.so> [frames]
Needs llvm-addr2line from the NDK on PATH or in $NDK.
"""
import os, shutil, subprocess, sys

def addr2line():
    tool = shutil.which('llvm-addr2line')
    if tool:
        return tool
    ndk = os.environ.get('NDK', os.path.expanduser('~/Library/Android/sdk/ndk/28.2.13676358'))
    return os.path.join(ndk, 'toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-addr2line')

def main():
    lines = open(sys.argv[1]).read().splitlines()
    lib = sys.argv[2]
    frames = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    starts = [i for i, l in enumerate(lines) if l.startswith('dump ')]
    for i in starts:
        print(lines[i])
    if not starts:
        return
    last = lines[starts[-1] + 1:]
    pcs = sorted({pc for l in last for pc in l.split(':', 1)[1].split()[:frames]})
    out = subprocess.run([addr2line(), '-f', '-C', '-e', lib] + pcs, capture_output=True,
                         text=True).stdout.splitlines()
    names = {pc: out[2 * k].split('(')[0][:100] for k, pc in enumerate(pcs)}
    for l in last:
        head, rest = l.split(':', 1)
        print('=== ' + head)
        for pc in rest.split()[:frames]:
            print('   ' + names.get(pc, pc))

main()
