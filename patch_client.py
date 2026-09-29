#!/usr/bin/env python3
"""Build Buildo-local.exe from pristine Buildo.exe, pointing it at our own
local server. Every patch asserts its original bytes, so it applies cleanly
or refuses. The original file is never modified."""
import os, pathlib, sys

# Where your pristine copy of the 2012 release lives. Override with BUILDO_SRC.
SRC_DIR = pathlib.Path(os.environ.get(
    "BUILDO_SRC", pathlib.Path.home()/"Downloads"/"OldAssBuildoWinFrom2012"))
OUT_DIR = pathlib.Path(os.environ.get("BUILDO_OUT", pathlib.Path.home()/"buildo-run"))
OUT     = [OUT_DIR/"Buildo-local.exe"]
HTTP_PORT = 8081          # >1024 so the responder needs no root

d = bytearray((SRC_DIR/"Buildo.exe").read_bytes())
orig = len(d)

def patch(off, expect, new, what):
    got = bytes(d[off:off+len(expect)])
    assert got == expect, f"{what}: at {off:#x} expected {expect.hex()}, got {got.hex()}"
    assert len(new) == len(expect), f"{what}: size change not allowed"
    d[off:off+len(new)] = new
    print(f"  ok {off:#08x}  {what}")

port = HTTP_PORT.to_bytes(4, "little")

# --- THE one that matters: App::GetServerInfo(host*, port*) at VA 0x402450 ---
# push 0x0A / push "rtsoft.com"  -> the domain every HTTP job uses
patch(0x0C85AC, b"rtsoft.com\x00\x00", b"127.0.0.1\x00\x00\x00", "app domain string")
patch(0x0002454, bytes.fromhex("6a0a"), bytes.fromhex("6a09"),   "push strlen(domain)")
# mov dword [eax], 80  -> the port that goes with it
patch(0x0002464, bytes.fromhex("c700") + (80).to_bytes(4,"little"),
                 bytes.fromhex("c700") + port, f"app HTTP port -> {HTTP_PORT}")

# --- secondary/legacy endpoint, harmless to redirect too ---
patch(0x0C9F80, b"hamumu.com\x00\x00", b"127.0.0.1\x00\x00\x00", "legacy host string")
patch(0x001533B, bytes.fromhex("6a0a"), bytes.fromhex("6a09"),   "push strlen(legacy host)")
patch(0x00305F7, bytes.fromhex("c78424dc00000050000000"),
                 bytes.fromhex("c78424dc000000") + port, "HTTP job default port")
patch(0x0096F69, bytes.fromhex("c74500") + (80).to_bytes(4,"little"),
                 bytes.fromhex("c74500") + port,          "URL parser default port")

assert len(d) == orig
for o in OUT:
    if o.parent.exists():
        o.write_bytes(bytes(d)); print(f"wrote {o}")
print(f"size unchanged: {len(d)} bytes")
