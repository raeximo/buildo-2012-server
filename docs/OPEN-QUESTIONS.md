# Open questions

Work that is available, with the evidence already gathered so nobody has to
start from zero. Roughly ordered by how much the answer is worth.

If you take one, open an issue saying so — and read
[METHOD.md](METHOD.md) first, because the standard here is *pinned or
labelled*, never *plausible*.

---

## Server work, no reverse engineering needed

**More than one player at a time.** The netID plumbing, `OnSpawn`/`OnRemove`
and the per-player inventory are all there; nobody has sat two clients in one
world and watched what breaks.

---

## Pinned in the binary, unused by us

Each of these is a real, registered call or packet type. The client will act on
it. Nobody has built the server side.

| thing | where | what is known |
|---|---|---|
| `OnSetFreezeState` | writes avatar+0x190 | the click handler already refuses to act while it is non-zero, so this is a working freeze |
| `OnAddNotification` | console component 0x432919 | takes `msg\|`, `imageFile\|`, `audioFile\|`, `delayMS\|` — a notification can carry an image *and* a sound |
| `OnAddLog` | 0x4329b9 | console line |
| `OnNameChanged` | NetAvatar | never sent |
| `OnFailedToEnterWorld` | GameLogicComponent | the polite refusal path we never use |
| `OnReconnect` | GameLogicComponent | — |
| `OnZoomCamera` / `OnPinchMod` | GameLogicComponent | camera control, untouched |
| `OnChangeSkin` | 0x448a20 | we set skin through `OnSetClothing`'s third argument instead; the dedicated call is unused |
| receive packetType 6 | 0x433F86 | "many tiles" — we push tiles one at a time |

**The SHOP and STORE buttons.** STORE is one of the three buttons the inventory
panel actually has (built at 0x4209f3..0x420a82). What it sends, and whether
anything answers it, is unexamined.

---

## Genuinely unknown

**Outgoing packetType 11 from 0x431c35.** One of exactly four outgoing packet
builders and the only one whose purpose is not identified. Find where 0x431c35
is called from and the answer follows.

**items.dat fields 6, 11, 17, 23, 24, 26, 27, 28.** Read and written by the
serialiser, referenced by nothing anyone has found. `tools/itemsdat.py`
bisects a field in about a minute — set one to a wild value, relaunch, look for
a difference.

**`set_max_can_hold` has no field.** The token exists in
`item_definitions.txt` and no item record field corresponds to it. The server
reads the text file instead. Either the field is one of the unknowns above or
the prototype dropped the concept.

**How many fruit a tree yields.** The tree renderer reads the *tile's* flags at
0x4449aa (`mov al,[edi+4]; shr al,4`) and `SetTile` fills flags 0x08/0x10 from
a packet's count1 — but driving count1 changed nothing on screen (0, 2 and 4
all drew the same sapling), and a second packetType 3 for the same tile risks
`SetItem` allocating a fresh extra and losing the growth stage. In practice the
fruit appears from the **stage** alone; a ripe tree drew five blocks with no
flag set. So the count may be emergent rather than chosen — but that is a
conclusion from absence, which is the weakest kind here.

**Which inert material each plain block originally used.** 8, 9 and 11 are
compared against nowhere and are therefore indistinguishable. Probably
unanswerable from this binary.

---

## Things we invented and would rather replace with evidence

Listed in full in
[WHAT-EXISTS.md §6](WHAT-EXISTS.md#6-things-we-implemented-that-the-binary-does-not-pin).
The ones most worth someone disproving:

* **the gem drop rate and source** — only item 112 is pinned, never a rate,
  never a trigger. `BUILDO_GEM_CHANCE=0` removes it entirely.
* **the ids, names and HP of the six art-proven items** — the textures are
  facts, the numbers are ours.
* **the boombox animation period (250 ms)** — has to be non-zero or the client
  divides by zero, but the value is a choice.
* **the starter kit** — the published build had no server, so there is no
  original to restore.
* **pairing each server-played sound with its moment** — the files and
  `OnPlayPositioned` are pinned; that `tile_removed.wav` belongs to removing a
  tile is inference from a filename.

If anyone has a **server-side capture, a build with symbols, or any
2012-era Growtopia server artefact**, several of these stop being guesses.
That is the single highest-value thing another archivist could bring.

---

## Not worth chasing

* **`hamumu.com`.** It sits next to the `growtopia/server_data.php` string and
  is the only host cross-referenced from that code. It is a decoy — the request
  goes through the app's default domain. Already cost one afternoon.
* **`RtlUnwindEx` in a Wine `+seh` trace.** Almost always just
  `OutputDebugStringA`; Wine raises DBG_PRINTEXCEPTION_C for every log line. A
  clean session has zero `c0000005`.
