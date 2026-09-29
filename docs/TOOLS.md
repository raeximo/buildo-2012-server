# Tools

Two problems dominate working on this: you cannot see what the client is
drawing, and you cannot type into it. Both are solved from *inside* the
process, and everything below exists because of that.

## Seeing what the game is doing

The game renders through OpenGL, so GDI `BitBlt` returns black (`shot.exe`
is useless) and macOS `screencapture` refuses without Screen Recording
permission. The only place the pixels are readable is inside the process
with the GL context current.

`tools/cap.dll` is injected by `tools/inj.exe` (CreateRemoteThread +
LoadLibraryA) and patches the exe's import-address-table entry for
GDI32!SwapBuffers. On each frame it looks for a request file and, when it
finds one, writes what was asked for:

    ./shot.sh [out.png]      C:\shot.req  -> glReadPixels -> PNG
    tools/probe.sh items     the parsed ItemInfo array
    tools/probe.sh world     every non-empty tile: fg, bg, flags, frames,
                             collision, TileExtra pointer
    tools/probe.sh bar       the item bar's tool vector
    tools/probe.sh inv       PlayerItems: capacity, every slot, the selected
                             quick-slot and the six worn slots
    tools/probe.sh av        the local NetAvatar: position, velocity, the
                             ground/jump flags, and the whole tile column
                             under it INCLUDING empty tiles (the world dump
                             skips those, and they are exactly where the
                             collision bugs hide)

Objects are found by scanning committed memory for their RTTI vtable address
and then validating the hit — the scan is bounded to the low 1 GB and to
regions of 64 MB or less, because an unvalidated hit on the stack once sent
the dumper off a wild pointer and wedged the render thread.

## Driving the window

`tools/ui.exe` drives the window from inside Wine (macOS Accessibility is not
granted to the terminal, so synthetic input has to originate on the Windows
side): `click`, `down`, `up`, `move`, `drag x1 y1 x2 y2`, `key vk ms`, `type`.
`drag` is what opens the inventory — the handle above the item bar is
`interface/handle_horizontal.rttex` and the panel slides up.

## Two traps that cost a day between them

* **macOS `pgrep` has no `-c` flag.** `pgrep -fc X` exits 2 with a usage
  error, so `ALIVE=$(pgrep -fc X 2>/dev/null || echo 0)` is always 0. That is
  the only reason the client ever looked like it crashed on world entry. Use
  `pgrep -f Buildo-local.exe | wc -l`.
* **`RtlUnwindEx` in a Wine `+seh` trace is usually just
  `OutputDebugStringA`** — Wine raises DBG_PRINTEXCEPTION_C (0x40010006) and
  unwinds for every line the game logs. A full session contains zero
  `c0000005` and zero `e06d7363`. Group a `+seh` log by exception code before
  concluding anything from it.


## Everything in the tree

    play.sh          start everything and walk into a world
    tools/run.sh     same, but never regenerates items.dat
    tools/trial.sh   one world-format experiment: run, report if it parsed
    dbg.sh           same as play.sh under WINEDEBUG (default +seh) into
                     seh.log, recording wine's exit code in exit.txt
    start.sh         start the servers + client, drive the UI yourself
    shot.sh          grab the OpenGL framebuffer as a PNG
    tools/probe.sh   items | inv | world | bar | av memory dumps
    gs               the server (C++/enet), server.cpp
    make_items.py    compile item_definitions.txt -> items.dat
    tools/itemsdat.py  read/rewrite a compiled items.dat field by field
    patch_client.py  rebuild Buildo-local.exe from the pristine Buildo.exe
    raw_httpd.py     server_data.php responder, tolerant of bare-\n HTTP
    udp_probe.py     raw UDP dump on 17091 (stop gs first) — how the range
                     coder + checksum were found
    drive.py         harness: kill / launch / click / read logs
    tools/inj.exe    DLL injector (CreateRemoteThread + LoadLibraryA)
    tools/cap.dll    SwapBuffers hook: screenshots + memory dumps
    tools/ui.exe     click / drag / key / type inside Wine
    poke.exe         the older input injector (list | click | rtext | rhold |
                     release). ui.exe supersedes it and adds drag.
    ra.py            static analysis of Buildo.exe: strings, xrefs, dis, fn,
                     callers, find, vt, classes, str2fn
    shot.exe         GDI window capture — returns black on an OpenGL window,
                     superseded by cap.dll

## Environment knobs

Everything the server and the item compiler let you change without editing
code. The ones marked **ours** switch off behaviour the binary does not pin —
set them to the honest value if you want only what is provably original (see
[WHAT-EXISTS.md §6](WHAT-EXISTS.md#6-things-we-implemented-that-the-binary-does-not-pin)).

### World generation (`gs`)

| variable | default | what it does |
|---|---|---|
| `BUILDO_WORLDS` | `START\nSECOND\nTHIRD` | newline-separated world list the menu offers |
| `BUILDO_WORLD_DIR` | `worlds` | directory for persistent world saves; loaded on first entry and saved every 5 seconds plus clean shutdown |
| `BUILDO_WORLD_VER` | `2` | world blob version byte |
| `BUILDO_FEAT` | `ds` | which special tiles a fresh world gets: `d` door, `s` sign, `l` lock, `p` seed, `b` boombox, `r` Olde Timey Radio, `w` a run of wood platforms |
| `BUILDO_NO_DOOR` | unset | equivalent to `BUILDO_FEAT=` |
| `BUILDO_DOOR_ITEM` | Door (6) | which of the three doors the generator lays down |
| `BUILDO_DOOR_DEST` | — | the generated main door's destination label |
| `BUILDO_DOOR_EXTRA` | `1` | TileExtra type-1 shape: `1` string (correct) or `3` strings, for re-testing the world-blob desync |
| `BUILDO_NO_EXTRA` | unset | omit TileExtras from the world blob (breaks the parse — diagnostic only) |
| `BUILDO_SEED_STAGE` | `0` | growth stage a generated seed starts at |
| `BUILDO_LOCK_SIZE` | `10` | side of the square of tiles a lock claims — **ours**, the binary has no radius |
| `BUILDO_SPAWN_X` / `_Y` | world spawn | override where the avatar lands |

### Player state (`gs`)

| variable | default | what it does |
|---|---|---|
| `BUILDO_PLAYER_DIR` | `players` | directory for persistent clothing and skin profiles, keyed by player name |
| `BUILDO_INV` | starter kit | inventory as `id:count,id:count,…` |
| `BUILDO_WEAR` | — | worn items as `id,id,…` |
| `BUILDO_GEM_CHANCE` | `50` | percent chance a broken block drops gems — **ours**; `0` turns gem drops off |
| `BUILDO_FRUIT` | `1` | fruit a harvested tree yields — **ours**; `max_fruit` ships as 0 |
| `BUILDO_TEST_ACTION` | — | fire one `OnAction` at the player, e.g. `/dance`, re-armed on every tile click |

### Item compilation (`make_items.py`)

| variable | default | what it does |
|---|---|---|
| `BUILDO_ITEMS` | regenerated | pin a prebuilt `items.dat` so experiments are reproducible |
| `BUILDO_ITEM_DEFS` | `~/buildo-run/game/item_definitions.txt` | source item table |
| `BUILDO_BLOOM` | `0` | seconds for a seed to ripen — **ours**; the shipped data says 0 |
| `BUILDO_PLATFORM_ID` | `102` | item id for the Wood Platform — **ours**, art-proven item, invented id |
| `BUILDO_NO_EXTRA_ITEMS` | unset | leave out all six art-proven items (102, 104, 106, 108, 110, 114) |

Escape hatches inside `make_items.py` for bisecting a field: `ZERO_MATERIAL`,
`FORCE_STORAGE`, `KEEP_VISUAL`.

### Plumbing

| variable | default | what it does |
|---|---|---|
| `BUILDO_SRC` | `~/Downloads/OldAssBuildoWinFrom2012` | where your pristine copy of the 2012 release lives — `ra.py` and `patch_client.py` read `Buildo.exe` from here |
| `BUILDO_HTTP_PORT` | `8081` | `server_data` port — **must match `patch_client.HTTP_PORT`**; re-run `patch_client.py` after changing it |
| `BUILDO_VERBOSE` | unset | log every packet |
| `BUILDO_DUMP_STATE` | unset | dump server-side player/world state on each tile click |

## Do not send ESC or SPACE from a test script

ESC quits to the main menu and SPACE opens the chat keyboard
("Keyboard active: 1" in `log.txt`), so either one silently ends a scripted
run.
