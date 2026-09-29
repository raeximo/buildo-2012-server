# Protocol reference

Everything on this page was read out of `Buildo.exe` and then confirmed on a
running client. Virtual addresses (`0x43e793`) are into the shipped
`Buildo.exe`, unpatched — see [SETUP.md](SETUP.md) for the SHA-256 of the build
these refer to.

[WHAT-EXISTS.md](WHAT-EXISTS.md) is the companion to this page: the closed
lists (every server→client call, every packet type, every protocol key, every
material the client branches on) plus an explicit note of which behaviours here
are OURS rather than restored. Check a name against those lists before building
on it, and read [METHOD.md](METHOD.md) for how to check one yourself.

## Contents

- [Transport](#transport) · [Framing](#framing) · [server_data.php](#server_dataphp)
- [Login and the item-database handshake](#login-and-the-item-database-handshake)
- [items.dat](#itemsdat) — the record layout and all four enums
- [World](#world) — the blob, tiles and TileExtras
- [Tile edits](#tile-edits) · [Inventory](#inventory) · [Other messages](#other-messages)
- [Dropped objects, gems and growing things](#dropped-objects-gems-and-growing-things)
- [Dialogs, emotes and the other things the client asks for](#dialogs-emotes-and-the-other-things-the-client-asks-for)


## Transport
ENet/UDP, 2 channels, port 17091. The client enables enet's range coder AND
enet's crc32 checksum. Observed connect header 0xCFFF = SENT_TIME|COMPRESSED
with peer id 0xFFF, then 4 bytes of crc32, then a compressed command block (a
48-byte Connect arrives as ~31). Without both, every packet is silently
dropped and the client only says "Connection timed out":

    enet_host_compress_with_range_coder(host);
    host->checksum = enet_crc32;

## Framing
int32 NetMessage type first: 1 SERVER_HELLO, 2 GENERIC_TEXT, 3 GAME_MESSAGE,
4 GAME_PACKET, 5 ERROR, 6 TRACK.

GameUpdatePacket is 56 bytes (0x38); flags at 0x0C, bit 0x08 = extended data
follows; extended length at 0x34. From the client's bounds check at 0x43C5B0
(`cmp $0x3C` = 4+56) and the accessor at 0x43C590 (`add $0x38` when flags & 8).

VariantList: uint8 count, then per entry uint8 index, uint8 type, payload.
Types 1 float, 2 string (uint32 len + bytes), 3 vec2, 4 vec3, 5 uint32,
9 int32. A server->client function call is packetType 1 + a VariantList; the
GameUpdatePacket's netID says which NetObject the call is addressed to.

Packet types — jump table at VA 0x434418, index = packetType:

     0 -> 0x433936  avatar state      9 -> 0x434130  inventory
     1 -> 0x433961  call function    10 -> default
     2 -> default                   11 -> default
     3 -> 0x433C03  set tile         12 -> 0x433E72
     4 -> 0x433FF4  map data         13 -> 0x433E26
     5 -> 0x433F28                  14 -> 0x434173  objects
     6 -> 0x433F86                  15 -> 0x433E59
     7 -> default                   16 -> 0x434304  items.dat
     8 -> 0x433B26  damage tile

Numbering matches modern Growtopia (16 = SEND_ITEM_DATABASE_DATA).

## server_data.php
The client speaks HTTP/1.0 with BARE \n line endings. Python's http.server
rejects that and replies with its own 400 page (~226 bytes) — the red herring
that made this look like a dead endpoint. Use `raw_httpd.py`.

    POST /growtopia/server_data.php     body: version=0%2E01&platform=0&api=1
    reply: server|... port|... type|... then RTENDMARKERBS1001

The client scans the buffer for that marker before firing OnFinish (substring
search at 0x494630).

## Login and the item-database handshake
Client sends GENERIC_TEXT:

    requestedName|  protocol|  game_version|  hash|  hash2|
    platformID|  deviceVersion|  country|   [reconnect|1]

`hash|` is NOT the items.dat hash — it is constant regardless of that file.

The server replies with `OnInitialLogonAccepted` whose **first argument is a
uint32: the expected hash of cache/items.dat**. ProcessConnection at 0x414480:

    49CC00  FileManager::Get("cache/items.dat")
    453410  hash(buffer, len) -> edi
    414596  cmp edi, [esp+0x68]      ; the uint32 we sent
    41459A  je  skip_refresh         ; equal -> use the cached file

Hash function (VA 0x453410), the classic Growtopia one:

    h = 0x55555555; for each byte: h = (h >> 27) + (h << 5) + byte;

If the hashes differ the client sends `action|refresh_item_data` and waits
for packetType 16, whose extended data it writes to cache/items.dat. The
server answers that properly now, so the cached copy no longer has to be
staged by hand — `play.sh` still copies it in to skip the round trip.

## items.dat
Container (0x43C090):

    uint16 version           (2 works)
    uint32 itemCount
    itemCount x item record

Item record — **30 fields, 57 fixed bytes** plus three strings, from the
symmetric (de)serializer at 0x43ACC0 (`bl` selects read vs write). Strings are
uint16 length + raw bytes (0x456380). In-memory record is 0xAC = 172 bytes.

    on-disk pattern:
    4,1,1,S,S,4,1,4,1,1,1,1,1,1,4,1,2,1,S,4,4,1,1,1,1,4,4,2,2,4

Two traps, both of which cost hours:

* the FINAL field's write and read branches each carry their own
  `add $N,(%esi)` before returning, so a naive scan counts it twice and you
  build 61-byte records instead of 57.
* the client indexes this array **by item id**, not sequentially —
  `imul ebp, ebp, 0xac` / `add ebp, [esi+0xc]` at 0x443916. The array must be
  DENSE: entry N describes item id N.

     #  on disk   mem     meaning
     0  int32     +0x00   item id
     1  uint8     +0x04   eTileMaterial
     2  uint8     +0x08   eTileVisualEffect
     3  string    +0x0C   display name
     4  string    +0x2C   texture filename   (0x44393b prefixes "game/")
     5  int32     +0x28   texture hash
     6  uint8     +0x48   unknown (leave 0)
     7  int32     +0x4C   colour (set_color RGBA)
     8  uint8     +0x50   frameX
     9  uint8     +0x51   frameY
    10  uint8     +0x54   eTileStorage
    11  uint8     +0x58   unknown (leave 0)
    12  uint8     +0x5C   eCollisionType
    13  uint8     +0x60   HP — punches to break
    14  int32     +0x64   seconds before the damage heals
    15  uint8     +0x68   clothing body part
    16  uint16    +0x84   RARITY — 0x4355df reads it as a uint16 and
                         prints 'Selected `w%s``. Rarity: `w%d``'
                         (0x4cb9d0). No column for it in
                         item_definitions.txt, so it stays 0.
    17  uint8     +0x86   unknown
    18  string    +0x88   SOUND path, played by materials 5 and 6
    19  int32     +0xA4   seed bg colour
    20  int32     +0xA8   seed fg colour, or the material 5/6 period in ms
                         (must not be 0 — see eTileMaterial below)
    21  uint8     +0x6C   seed1
    22  uint8     +0x6D   seed2
    23  uint8     +0x6E   unknown
    24  uint8     +0x6F   unknown
    25  int32     +0x70   seconds to bloom
    26  int32     +0x74   unknown
    27  uint16    +0x78   unknown
    28  uint16    +0x7A   unknown
    29  int32     +0x7C   unknown

`set_max_can_hold` from item_definitions.txt has no identified field. The
server reads that line out of the text file instead, so the Fist and the
Wrench stay single copies.

### eTileStorage — where a tile's frame comes from

The renderer at 0x444522 is literally

    frame = (storage == 1) ? frameY * sheetColumns + frameX : tile[+0x0a]

and tile+0x0a is filled by the dispatch at 0x440f38 (jump table 0x440fe8):

    0, 1 -> frame 0     2 -> 0x4404a0 four-neighbour smart edge
                        3 -> 0x440390 left/right smart edge

so **SINGLE_FRAME_IN_TILESHEET = 1 and SMART_EDGE = 2**. With SMART_EDGE
written as 1 the dirt renders as one flat frame with no grass; with 2 the
grass, corners and cave edges all appear. The background layer uses the same
dispatch into tile+0x0c (0x440fac / table 0x440ff8).

### eTileMaterial — the values the client branches on

     0  SetTile takes the remove path: clear fg, else clear bg
                                            (0x4412b5 / 0x44133e)
     2  TileExtra type 1                    (0x43e7ac -> 0x43ecb0 -> 0x43eccd)
     3  TileExtra type 3, and the renderer draws the lock overlay using the
        owner id from it                    (0x444662). tiles_lock.rttex has
        exactly the 4 frames that branch selects, which is what pins LOCK=3.
     4  TileExtra type 2                    (0x43ecd5)
     5  punching plays the sound named by the item's THIRD STRING, rate
        limited by ItemInfo+0xa8            (0x43e56b / 0x4445ea)
     6  punching toggles tile flag 0x40 and the renderer animates the tile
        while it is set, stepping frame +1/+2 on a period from ItemInfo+0xa8
                                            (0x43e55c / 0x444556)
     7  TileExtra type 1                    (0x43eccd)
    12  SetTile writes the BACKGROUND       (0x4412de -> 0x43e1f0)
    13  seed: only plantable on an empty tile, TileExtra type 4
    14  clothing: equipped by ItemInfo+0x68 (0x43dcbf)
    15  used by 0x442cd6

     1  NOT PLACEABLE -- the wrench material. See below.
    10  LAVA: the collision response at 0x4474e0 negates the avatar's
        velocity, plays audio/burn.wav (0x4cc3a0) and raises state flag 0x40.

The material also picks the item-box border colour in the inventory
(0x443080: 12 -> yellow, 13 -> green, 14 -> the clothing frame, selected ->
red).

### Material 1, the wrench, and why solid blocks would not place

The world-click handler is one function, 0x436c80, and it decides everything
from the HELD item's material:

    0x436d0a  cmp dword [ebp+4], 0      ; held material == 0 (FIST)?
    0x436d0e  sete bl                   ; -> "punch mode"
    ...
    0x436efa  mov eax, [ebp+4]
    0x436efd  cmp eax, 0xc  / sete cl   ; background
    0x436f03  cmp eax, 1    / sete bl   ; <-- material 1
    0x436f31  cmp word [edi], 0         ; is the target tile empty?
    0x436f35  jne 0x436fce
    0x436f3b  test al,al / jne act      ; fist -> punch
    0x436f3f  test bl,bl / je  place     ; material 1 -> fall through to:
              audio/cant_place_tile.wav  ; ... and send the server NOTHING

and for a tile that is NOT empty:

    0x436fd8  call 0x43e450             ; material = ItemInfo[tile->fg].material
                                        ; return (material - 2) <= 2
    0x436fdd  test al,al / jne act      ; true for DOOR(2), LOCK(3), SIGN(4)
              audio/cant_place_tile.wav ; anything else -> refuse

So material 1 is exactly a wrench: an item that can never be placed and that
acts only on a door, a lock or a sign. `make_items.py` used to hand it to
Dirt, Lava, Bedrock, Rock, Wood **and** the Wrench as a supposedly inert
filler, which is why none of those blocks could be placed and why the wrench
looked dead. It was never dead — the client sends the ordinary packetType 3
with the wrench as the held item and the server was answering it as a failed
placement (`-- place refused ... foreground occupied (Wrench)`).

**8, 9 and 11 are the genuinely inert values** — the only ones compared
against nowhere in the binary, and the SetItem switch's index table at
0x43e83c sends all three to the no-TileExtra case. Dirt, Bedrock, Rock and
Wood now use 8. Which of the three the original build gave each block is not
recoverable: nothing in the binary tells them apart.

Every action -- punch, place, wrench, wear -- leaves the client the same way,
through 0x430f00: a 56-byte packet zeroed, `packetType = 3`, intX/intY from
tile+0x06/+0x07 and intData = the held item id. The server is what decides
which of the four it was.

Everything else behaves identically everywhere in the binary, so DIRT, LAVA,
BEDROCK, ROCK, WRENCH and WOOD all get one value that is provably inert (1).
**5 is not inert** — giving Dirt material 5 makes punching it try to play its
texture filename as a sound. BOOMBOX is 6: tiles_boombox.rttex is 4x4 with one
item per row, which is exactly the extra frames the material-6 animation steps
through, and "punch it to turn it on" is what that branch does.

**Materials 5 and 6 must have a non-zero ItemInfo+0xa8.** The renderer's
material-6 branch is

    0x444564  mov ebp,[esi+0xa8]   ; the period, in milliseconds
    0x44456c  call 0x44d290        ; now_ms
    0x444571  xor edx,edx
    0x444573  div ebp              ; <-- 0 here is a divide by zero

and it runs on the first frame after a punch sets tile flag 0x40, so punching
a Boombox or the Olde Timey Radio killed the client outright
(`+seh` gives `code=c0000094 (EXCEPTION_INT_DIVIDE_BY_ZERO) addr=00444573`
with ebp=0). item_definitions.txt has no column for that period, nor for the
sound in the item's third string (ItemInfo+0x88) that the same branch plays,
so `make_items.py` supplies both — the build ships
`audio/mp3/{boombox,old_timey}.mp3` and the only two material-6 items in the
file are Boombox and Olde Timey Radio, so the pairing is not a guess. Verified
by `lsof`: with the radio switched on the client holds
`audio/mp3/old_timey.mp3` open, and the tile alternates between two frames on
the 250 ms period.

### eTileCollision — three values, not two

Tile::SetItem (0x43e793) spends ItemInfo+0x5c twice:

    mov   eax,[edi+0x5c]      ; ItemInfo.collision
    cmp   eax,0
    setne cl
    mov   [esi+0x10],eax      ; tile.collision
    mov   [esi+0x0e],cl       ; does this tile take part in collision at all

and **tile+0x0e is the gate**: the collision query at 0x44207f skips a tile
whose +0x0e is 0 outright, and 0x4420e0 then picks the box for the ones that
are left — the full 32x32 tile when collision != 2, or a **one-pixel strip at
the tile's top edge** when it is exactly 2. So:

    0  no collision at all, the tile is not even considered   <- Blank/air
    1  solid                                                 <- dirt, bedrock
    2  one-way platform: land on its top edge, pass through otherwise.
       No item in this build uses it.

NetAvatar::IsTileBlocking (0x4497b0) is `blocked = (tile->[0x10] != 2)` and
the ground check in the physics step (0x4470da) is the same test, which is why
2 looks like "NONE" if that one function is all you read. **It is not**, and
writing NONE as 2 is what made the avatar jump and never come back down: every
one of the 6000 empty tiles got +0x0e = 1 and became a 1-pixel platform, so
the avatar landed on the top edge of whatever tile row it fell into, sat there
with the ground flag set, and rose exactly one tile per jump (bonking its head
on the strip above, which is why the jump also looked far too short).
`tools/probe.sh av` prints the tile column under the avatar including the
empty tiles the world dump skips, which is how the before/after was measured:

    before  ( 50, 28) fg=0 bg=0 blocked=1 coll=2     <- 1-px platform in the sky
    after   ( 50, 28) fg=0 bg=0 blocked=0 coll=0     <- not considered
    after   ( 50, 30) fg=2 bg=14 blocked=1 coll=1    <- the dirt it lands on

Two earlier traps in the same field, both still worth knowing: writing
collision into disk slot 11 instead of 12 put item HP where the collision
value belongs, and no item has HP 2, so every tile came out solid and the
avatar could not move at all.

### Clothing body part = the sprite layer

The inventory writes the worn item id to PlayerItems + 6 + bodyPart*2
(0x43dcd0) and the read path clears exactly 12 bytes there (0x43dbdf), so
there are six slots. The body part is **nothing but the slot index**, and the
slot index alone picks which sheet the avatar is drawn from — an item's own
texture filename is ignored for clothing. OnSetClothing's two vec3s become six
uint16s, and 0x44a000 turns each into a frame index:

    movzx ecx,[eax+0x51]   ; frameY
    movzx edx,[eax+0x50]   ; frameX
    lea   eax,[edx+ecx*8]  ; frame = frameX + frameY*8

every sheet being 256px wide in 32px cells. The order is

    slot 0  player_hair.rttex        slot 3  player_feet.rttex
    slot 1  player_shirt.rttex       slot 4  player_faceitem.rttex
    slot 2  player_pants.rttex       slot 5  player_handitem.rttex

**Measured off the screen, not deduced.** The texture loader at
0x44bd49..0x44c091 walks the sheets in a different order (player_arm,
player_puncharm and player_face are interleaved), and believing it swapped
SHOES with FACEITEM. The mapping was pinned instead by wearing six hair items
whose frames are 1..6, one per slot, and reading off which frame each body part
drew: red hair on the head names slot 0, ruby slippers on the feet name slot 3,
shades on the face name slot 4. Earlier the table had HAT at 3 and a top hat
came out as red hair — the hair layer was drawing whatever sat in slot 0, and
hair frame 1 is the red hair.

`make_items.py` compiles game/item_definitions.txt (the SERVER-side source;
the client holds none of its tokens) into this format. `tools/itemsdat.py`
reads and rewrites a compiled items.dat field by field, which is how the
enums above were bisected.

## World
World::Deserialize at 0x43F450:

    uint16 version              -> world+0x38   (2 works)
    uint32                      -> world+0x74   (read at buf+2, so the
                                                 cursor starts at 6)
    string name
    TileMap                     0x441B60
      int32 width
      int32 height
      int32 tileCount
      tileCount x Tile          0x43E850, in-memory stride 0x38
        uint16 fg               -> tile+0x00
        uint16 bg               -> tile+0x02
        uint16                  -> tile+0x34
        uint16 flags            -> tile+0x04
        uint16                  -> tile+0x34   only if flags bit 1
        TileExtra                              only if flags bit 0
    Objects                     0x43FFC0
      int32 count, int32 nextId, then the object list

**The trap that corrupted every world for a week:** Tile::Serialize reads
fg/bg/word/flags and then calls SetItem with the fg it just read. SetItem
runs the material switch at 0x43e7ac and, for materials 2/3/4/7/13,
allocates a TileExtra and ORs flags bit 0 into the tile — *before* Serialize
tests that bit at 0x43e92c. So a tile whose foreground needs an extra must be
followed by one no matter what flags you wrote, or the cursor desynchronises
and the rest of the world is garbage. Keep flags at 0 and emit an extra
exactly for those materials.

TileExtra::Serialize 0x43F070 — uint8 type first, then by type
(primitives: 0x456380 string = uint16 len + bytes, 0x43abe0 uint8,
0x43ac20 uint32):

    type 1   string, uint8                      materials 2 and 7
    type 2   string, uint32                     material 4
    type 3   uint8, uint32 owner, uint32 count, count x uint32   material 3
    type 4   uint32 age-seconds, uint8 stage    material 13
             (the plant time is stamped to "now" on read, not transmitted)

Type 1 has two shapes, chosen by an argument TileMap::Serialize passes down:
one string (0x43f0bd) or three (0x43f130). **The world blob takes the
one-string shape** — measured, not deduced: with one string the blob the
server sends and the size the client reports decompressing are identical and
every tile after the door survives; with three strings the client consumes a
different number of bytes and the parse runs off the end. `BUILDO_DOOR_EXTRA=3`
switches back for re-testing.

## Tile edits
World::ApplyPacket 0x43F270 is what both edit packets go through:

    packetType 3 -> WorldTileMap::SetTile(intX, intY, intData)   0x441260
                    material 0  clears the foreground, else the background
                    material 12 writes the background
                    material 13 plants a seed, only on an empty tile
                    anything else writes the foreground
                    then flags 0x08/0x10 from count1, and tile mirror flag
                    0x20 from packet flags bit 0x10. The placement builder
                    copies the avatar's facing byte into that packet bit;
                    the tile renderer mirrors materials 4, 5 and 6 with it.
    packetType 8 -> Tile::Damage(intData)                        0x43E4A0
                    damage = min(damage + n, ItemInfo.hp)
                    tile+0x2c = now_ms + ItemInfo.healSecs*1000
                    the renderer draws game/crack.rttex from that

Both need a real netID: the client looks the actor up and bails if it is not
a NetObject it knows (0x433b7d). On packetType 3 the client also removes one
of that item from its own inventory when the actor is the local player
(0x433ca6 -> 0x43d800), so the server must make the same deduction.

The client sends the same packetType 3 upward — intX/intY are the tile it
aimed at and intData is the item it is holding, so the Fist means punch and
anything else means place. The server owns HP and decides when a tile breaks.

## Inventory
PlayerItems::Serialize 0x43DB00 (packetType 9):

    uint8 capacity
    uint8 slotCount
    slotCount x { uint16 itemId, uint8 amount, uint8 flags }

flags bit 0 means "worn"; the client then equips it by body part (0x43dcbf).
PlayerItems itself is +0x04 capacity, +0x06..+0x11 the six worn slots,
+0x14 the std::list of entries (node: next, prev, uint16 id, uint8 amount,
uint8 flags), +0x20 the selected item.

The item bar is the InventoryComponent (global at 0x4EC158). Its tool vector
at +0x98/+0x9c is refilled from the whole inventory at 0x435ac8, and it draws
a grid clipped to the window — at 1024x768 only the first row of four is
visible, which is why it looks like a four-slot bar. Drag the handle above it
to slide the whole panel up.

The four boxes above the panel are the quick-tool slots; unassigned ones hold
item 0, whose frame in tileset_1 is the red "EMPTY" placeholder.

## Other messages
Client verbs   action|enter_game, join_request (with name|WORLD),
               refresh_item_data, input (with text|), dialog_return, drop
               (with itemID|), respawn, setSkin (color|N), info, quit
               The inventory panel has exactly three buttons -- DROP, STORE
               and INFO (built at 0x4209f3..0x420a82) -- and DROP/INFO send
               "action|drop\n|itemID|N" and "action|info\n|itemID|N"
               (0x4ca868/0x4ca888). There is no wear button anywhere: a click
               on a panel cell only selects, and the client says
               "Selected `w<name>``. Rarity: `w<n>``".
Server calls   OnAction, OnConsoleMessage, OnInitialLogonAccepted(uint32 itemHash),
               OnRequestWorldSelectMenu(NEWLINE-separated world names),
               OnSpawn, OnRemove, OnSetPos, OnSetBux,
               OnSetClothing(vec3 slots 0-2, vec3 slots 3-5, uint32 skin),
               OnDialogRequest, OnTalkBubble, OnFailedToEnterWorld,
               OnNameChanged, OnReconnect
Spawn text     spawn|avatar, netID|, userID|, colrect|, posXY|, name|,
               country| (needed or it tries to load interface/flags/.rttex),
               invis|, mstate|, type|local
Tile ids       even numbers: 0 Blank, 2 Dirt, 4 Lava, 6 Door, 8 Bedrock,
               10 Rock, 12 User Door, 14 Cave Wall, 18 Fist, 20 Sign,
               32 Wrench, 34..50 and 66..98 clothing, 52 Wood Wall, 60 Lock,
               62 Boombox, 100 Wood Block; 3, 5, 15 are seeds
UI             MainMenu: "Online" = Play Online (394,472), "About" — if a
               launcher click lands on About instead, its Back button is at
               (70,705) and the walk-in sequence can be re-driven by hand
               OnlineMenu: name_input_box, tankid_name/password, check_tankid,
               entity "Start" labelled "Connect" (600,630)
               World select reuses name_input_box + Start, remembers lastworld
               In world: pause menu (978,45), inventory handle (512,656)

**OnSetClothing is addressed to a netObject that does not exist yet in the
same burst as OnSpawn** — send it in the same packet run and the client prints
"Server wants to call OnSetClothing ... but it doesn't exist". The server
holds it until the client speaks again.


## Dropped objects, gems and growing things

**Objects.** A world's third section (0x43ffc0) is `uint32 count`, `uint32
nextId`, then one 16-byte record each, and the record serialiser (0x43f990)
walks it in exactly this order:

    uint16 itemId, float x, float y, uint8 count, uint8 flags, uint32 id

Live changes ride packetType 14, which World::ApplyPacket hands to the object
manager (0x440150):

    netID == -1   spawn: AddObject(intData, (vecX,vecY), (int)floatVar, objType)
    otherwise     remove object [intData], and the client at 0x4341ba checks
                  whether that netID is its own -- NetAvatar+0x30 -- and if so
                  puts the item in its inventory

**Mind which field carries the count.** AddObject (0x43feb0) stores its third
argument into obj+0x0e, the count, and its fourth into obj+0x0f, the flags —
so the count travels in **floatVar**, not in objType. Sending it in objType
gives every drop a count of 0: the pickup box still draws perfectly (the sprite
comes from the item id) and still vanishes when collected, and you simply
receive nothing. The object's id is never transmitted on a spawn — 0x43ff8a
assigns `++nextId` from the client's own counter — so the server has to run the
same counter in the same order.

**Gems are item 112**, the one id the client hard-codes: 0x434206 compares the
picked-up item against 0x70 and, on a match, does `add [app+0x130], count`
straight onto the counter RenderBuxComponent draws, and PlayerItems::Add
(0x43dd25) refuses the id outright so it can never reach the inventory.
`tiles_bux.rttex` is one of the textures no item in item_definitions.txt
claims, so make_items.py synthesises the item from those two facts.

**A slot holds 99.** PlayerItems::Add is
`if (amount + n > 0x63) n = 0x63 - amount` (0x43dda6), so handing out 100 of
something made the very first pickup add -1 and the count visibly drop to 99.

**Growing.** The client does NOT grow anything by itself. A tile planted by
packetType 3 gets a TileExtra with stage 0, and **stage 0 draws nothing at
all** — measured: a seed left for minutes with a 15-second bloom stayed
invisible, and the same tile with stage 3 in the world blob drew a full tree out
of tree_trunks/tree_greens. The age in the extra only feeds the label; the stage
picks the sprite, and the server owns it. Push it with packetType 12, which
ApplyPacket handles at 0x43f32b:

    tile = GetTile(intX, intY); if (!Tile::IsPlanted(tile)) -> the client
        prints "Error handling tree state change"
    netID2 == -1   clear the tile
    otherwise      stage = (uint8)intData, applied to the extra

The label comes from ItemInfo+0x7c, the seconds to bloom, minus
Tile::GetPlantAge (0x43ed30 = `(now_ms - tile+0x60)/1000 + tile+0x64`), and
reads "<time> to harvest" until it goes negative and then
"(`wPunch to harvest``)". seconds_to_bloom used to be compiled into field 25
(+0x70), which nothing reads, so every seed was ripe the moment it landed.

Seeds sit at blockId + 1 in item_definitions.txt — seed 3 beside Dirt 2, seed 5
beside Lava 4, seed 15 beside Cave Wall 14 — and the client confirms the pairing
on screen: a ripe Seed 3 tree hangs actual Dirt blocks on itself as fruit. What
a tree YIELDS is not pinned, though: every setup_seed line ships
`seconds_to_bloom|0` and `max_fruit|0`, so the growing time (BUILDO_BLOOM,
default 60s) and the two-fruit-plus-a-seed payout are ours.

## Dialogs, emotes and the other things the client asks for

**Dialogs.** The client builds a dialog from a newline-separated script sent as
`OnDialogRequest`, and the parser at 0x417e40 checks each line's token count
and logs `Error with <cmd> parms` when one is short — which is how the shapes
below were pinned rather than guessed:

    add_label|<size>|<text>|<align>                     >= 4   0x41abbc
    add_label_with_icon|<size>|<text>|<align>|<itemID>   >= 5   0x41ad64
    add_textbox|<text>|<align>                          >= 3   0x41af4a
    end_dialog|<name>|<cancelText>|<okText>             >= 4   0x41b569

plus add_spacer, add_button, add_checkbox, add_text_input, add_player_picker,
set_default_color, disable_resize and tileIcon in the same vocabulary
(0x4ca4a4..0x4ca5b0). The reply is `action|dialog_return` carrying
`dialog_name|`, `buttonClicked|` and one line per input (0x4ca358..0x4ca388).
`add_label_with_icon` draws the real item sprite from the id, which is what the
INFO and wrench dialogs use.

**Emotes.** The client knows exactly two, and it does NOT parse them itself.
NetAvatar registers a handler for a server call named **OnAction** (name at
0x4cc454, handler 0x4471f0) which compares its one string argument to `/dance`
(0x4cc370 -> animation 0x44a500) and `/wave` (0x4cc368 -> 0x44a4e0) and prints
`Unknown action: %s` for anything else. So the server is what turns typed chat
into an emote, and no other command name exists anywhere in this build —
anything else would be pure server-side invention.

OnAction is addressed to a netObject and hits the same timing trap as
OnSetClothing: sent in the burst after OnSpawn the client answers *"WARNING:
Server wants to call OnAction with the parms of Parm 0: /dance on netObj 1, but
it doesn't exist"*. `BUILDO_TEST_ACTION="/dance"` fires one at the player and
re-arms it on every tile click, because synthetic keystrokes do not reach the
client's chat box and an emote fired at world entry is over before a frame can
be grabbed.
