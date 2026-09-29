/* Buildo (Growtopia prototype, Nov 2012) - local game server.
 *
 * Everything here is derived from Buildo.exe, not from the modern game.
 * The VAs in the comments are the client functions the behaviour was read
 * out of; ra.py disassembles them.
 *
 * Transport
 *   ENet/UDP 17091, 2 channels, range coder + crc32 checksum (the client
 *   turns both on, and without them every packet is silently dropped).
 *   Every message starts with an int32 Proton NetMessage type.
 *
 * GameUpdatePacket is 56 bytes; flags bit 0x08 means extended data follows,
 * with its length at +0x34 (client bounds check 0x43c5b0, accessor 0x43c590).
 *
 * Packet types (client jump table 0x434418):
 *    0 avatar state     3 set tile        4 map data     8 damage tile
 *    1 call function    9 inventory      14 objects     16 item database
 */
#include <enet/enet.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cerrno>
#include <csignal>
#include <string>
#include <vector>
#include <map>
#include <sys/stat.h>

/* ---------- Proton NetMessage ids ---------- */
enum {
    MSG_SERVER_HELLO        = 1,
    MSG_GENERIC_TEXT        = 2,
    MSG_GAME_MESSAGE        = 3,
    MSG_GAME_PACKET         = 4,
    MSG_ERROR               = 5,
    MSG_TRACK               = 6,
};

/* ---------- GameUpdatePacket: exactly 56 bytes ---------- */
#pragma pack(push, 1)
struct GameUpdatePacket {
    uint8_t  packetType;      // 0x00
    uint8_t  objType;         // 0x01
    uint8_t  count1;          // 0x02
    uint8_t  count2;          // 0x03
    int32_t  netID;           // 0x04
    int32_t  netID2;          // 0x08
    uint32_t flags;           // 0x0C  <- bit 0x08 = has extended data
    float    floatVar;        // 0x10
    int32_t  intData;         // 0x14
    float    vecX;            // 0x18
    float    vecY;            // 0x1C
    float    vec2X;           // 0x20
    float    vec2Y;           // 0x24
    float    particleRot;     // 0x28
    int32_t  intX;            // 0x2C
    int32_t  intY;            // 0x30
    uint32_t dataLength;      // 0x34
};
#pragma pack(pop)
static_assert(sizeof(GameUpdatePacket) == 56, "GameUpdatePacket must be 56 bytes");

/* ---------- VariantList serialisation (Proton) ----------
   uint8 count, then per entry: uint8 index, uint8 type, payload.
   types: 1=float 2=string(uint32 len+bytes) 3=vec2 4=vec3 5=uint32 9=int32 */
struct Variant {
    int type; std::string s; float f[3]; int32_t i;
    static Variant str(const std::string& v){ Variant a{}; a.type=2; a.s=v; return a; }
    static Variant flt(float v){ Variant a{}; a.type=1; a.f[0]=v; return a; }
    static Variant i32(int32_t v){ Variant a{}; a.type=9; a.i=v; return a; }
    static Variant u32(uint32_t v){ Variant a{}; a.type=5; a.i=(int32_t)v; return a; }
    static Variant vec2(float x,float y){ Variant a{}; a.type=3; a.f[0]=x; a.f[1]=y; return a; }
    static Variant vec3(float x,float y,float z){ Variant a{}; a.type=4; a.f[0]=x; a.f[1]=y; a.f[2]=z; return a; }
};

static void put8 (std::vector<uint8_t>& b, uint8_t v){ b.push_back(v); }
static void put32(std::vector<uint8_t>& b, const void* v){
    const uint8_t* p=(const uint8_t*)v; b.insert(b.end(), p, p+4);
}

static std::vector<uint8_t> serialize(const std::vector<Variant>& vl) {
    std::vector<uint8_t> b;
    put8(b, (uint8_t)vl.size());
    for (size_t i = 0; i < vl.size(); ++i) {
        const Variant& v = vl[i];
        put8(b, (uint8_t)i);
        put8(b, (uint8_t)v.type);
        switch (v.type) {
        case 2: { uint32_t n=(uint32_t)v.s.size(); put32(b,&n);
                  b.insert(b.end(), v.s.begin(), v.s.end()); break; }
        case 1: put32(b,&v.f[0]); break;
        case 3: put32(b,&v.f[0]); put32(b,&v.f[1]); break;
        case 4: put32(b,&v.f[0]); put32(b,&v.f[1]); put32(b,&v.f[2]); break;
        case 5:
        case 9: put32(b,&v.i); break;
        default: break;
        }
    }
    return b;
}

static bool g_verbose = false;

/* ---------- low level send ---------- */
static void send_raw(ENetPeer* p, const void* d, size_t n) {
    ENetPacket* pk = enet_packet_create(d, n, ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(p, 0, pk);
}

static void send_type_only(ENetPeer* p, int32_t t) { send_raw(p, &t, 4); }

/* send a GameUpdatePacket, optionally with extended data */
static void send_gup(ENetPeer* p, GameUpdatePacket g,
                     const uint8_t* ext = nullptr, size_t extLen = 0) {
    if (ext && extLen) { g.flags |= 0x08; g.dataLength = (uint32_t)extLen; }
    std::vector<uint8_t> b;
    int32_t t = MSG_GAME_PACKET;
    b.insert(b.end(), (uint8_t*)&t, (uint8_t*)&t + 4);
    b.insert(b.end(), (uint8_t*)&g, (uint8_t*)&g + sizeof g);
    if (ext && extLen) b.insert(b.end(), ext, ext + extLen);
    send_raw(p, b.data(), b.size());
}

/* server -> client function call */
static void call_fn(ENetPeer* p, const std::vector<Variant>& vl, int32_t netID = -1) {
    std::vector<uint8_t> data = serialize(vl);
    GameUpdatePacket g{};
    g.packetType = 1;              // call function
    g.netID      = netID;
    send_gup(p, g, data.data(), data.size());
    if (g_verbose)
        printf("  <- call %s\n", vl.empty()? "?" : vl[0].s.c_str());
}

/* ---------- helpers ---------- */
static std::string get_field(const std::string& txt, const std::string& key) {
    std::string k = key + "|";
    size_t p = txt.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    size_t e = txt.find_first_of("\n\r", p);
    return txt.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

static void dump(const uint8_t* d, size_t n, size_t maxb = 256) {
    size_t lim = n < maxb ? n : maxb;
    for (size_t i = 0; i < lim; i += 16) {
        printf("      %04zx  ", i);
        for (size_t j = 0; j < 16; ++j)
            if (i+j < lim) printf("%02x ", d[i+j]); else printf("   ");
        printf(" |");
        for (size_t j = 0; j < 16 && i+j < lim; ++j) {
            uint8_t c = d[i+j]; putchar(c>=0x20 && c<0x7f ? c : '.');
        }
        printf("|\n");
    }
    if (n > lim) printf("      ... (%zu more bytes)\n", n - lim);
}

static void w16(std::vector<uint8_t>& b, uint16_t v){ b.insert(b.end(),(uint8_t*)&v,(uint8_t*)&v+2); }
static void w32(std::vector<uint8_t>& b, uint32_t v){ b.insert(b.end(),(uint8_t*)&v,(uint8_t*)&v+4); }
static void w8 (std::vector<uint8_t>& b, uint8_t v){ b.push_back(v); }
static void wf (std::vector<uint8_t>& b, float v){ b.insert(b.end(),(uint8_t*)&v,(uint8_t*)&v+4); }
static void wstr(std::vector<uint8_t>& b, const std::string& s){
    w16(b, (uint16_t)s.size()); b.insert(b.end(), s.begin(), s.end());
}

/* ==================================================================== */
/*  Item database                                                        */
/* ==================================================================== */
/* packetType 16 == SEND_ITEM_DATABASE_DATA (jump table 0x434418 + 16*4 ->
   0x434304, which writes the extended data straight to cache/items.dat).

   The client compares the hash of its cached copy against the uint32 we
   pass as OnInitialLogonAccepted's first argument (ProcessConnection
   0x414480: mov edi,[esi+8]; cmp edi,[esp+0x68]; je skip_refresh). The
   hash function is Proton's, from 0x453410. */
static std::vector<uint8_t> g_items;
static uint32_t g_itemHash = 0;

static uint32_t proton_hash(const uint8_t* d, size_t n) {
    if (!d) return 0;
    uint32_t h = 0x55555555;
    for (size_t i = 0; i < n; ++i) h = (h >> 27) + (h << 5) + d[i];
    return h;
}

/* One parsed item record. The 30-field layout is the client's own symmetric
   (de)serializer at 0x43ACC0; only the fields the server needs are kept. */
struct ItemDef {
    int         id        = 0;
    uint8_t     material  = 0;    // +0x04  eTileMaterial
    uint8_t     visual    = 0;    // +0x08
    std::string name;             // +0x0c
    std::string file;             // +0x2c
    uint8_t     frameX    = 0;    // +0x50
    uint8_t     frameY    = 0;    // +0x51
    uint8_t     storage   = 0;    // +0x54  eTileStorage
    uint8_t     collision = 0;    // +0x5c  0 = no collision, 1 = solid,
                                  //        2 = one-way platform (unused here)
    uint8_t     hp        = 0;    // +0x60  punches to break  (Tile::Damage 0x43e4a0)
    int32_t     healSecs  = 0;    // +0x64  seconds before damage resets
    uint8_t     bodyPart  = 0;    // +0x68  clothing slot (material 14)
    int32_t     bloomSecs = 0;    // +0x7c  seconds to bloom (read at 0x43eaa8)
    bool        valid     = false;
};
static std::vector<ItemDef> g_defs;      // dense, indexed by item id

static bool rd_str(const std::vector<uint8_t>& d, size_t& p, std::string& out) {
    if (p + 2 > d.size()) return false;
    uint16_t n = (uint16_t)(d[p] | (d[p+1] << 8)); p += 2;
    if (p + n > d.size()) return false;
    out.assign((const char*)&d[p], n); p += n;
    return true;
}
template <typename T> static bool rd(const std::vector<uint8_t>& d, size_t& p, T& out) {
    if (p + sizeof(T) > d.size()) return false;
    memcpy(&out, &d[p], sizeof(T)); p += sizeof(T);
    return true;
}

/* Parse items.dat exactly the way the client does, so the server's idea of
   an item and the client's cannot drift. A desync is fatal, so it is
   reported rather than guessed around. */
static bool parse_items(const std::vector<uint8_t>& d) {
    size_t p = 0;
    uint16_t ver; uint32_t count;
    if (!rd(d, p, ver) || !rd(d, p, count) || count > 65535) return false;
    g_defs.assign(count, ItemDef());
    for (uint32_t i = 0; i < count; ++i) {
        ItemDef it;
        uint8_t  b; uint16_t h; int32_t v; std::string s;
        if (!rd(d,p,it.id)) return false;                    // 0
        if (!rd(d,p,it.material)) return false;              // 1
        if (!rd(d,p,it.visual)) return false;                // 2
        if (!rd_str(d,p,it.name)) return false;              // 3
        if (!rd_str(d,p,it.file)) return false;              // 4
        if (!rd(d,p,v)) return false;                        // 5  texture hash
        if (!rd(d,p,b)) return false;                        // 6
        if (!rd(d,p,v)) return false;                        // 7  colour
        if (!rd(d,p,it.frameX)) return false;                // 8
        if (!rd(d,p,it.frameY)) return false;                // 9
        if (!rd(d,p,it.storage)) return false;               // 10
        if (!rd(d,p,b)) return false;                        // 11
        if (!rd(d,p,it.collision)) return false;             // 12
        if (!rd(d,p,it.hp)) return false;                    // 13
        if (!rd(d,p,it.healSecs)) return false;              // 14
        if (!rd(d,p,it.bodyPart)) return false;              // 15
        if (!rd(d,p,h)) return false;                        // 16
        if (!rd(d,p,b)) return false;                        // 17
        if (!rd_str(d,p,s)) return false;                    // 18
        if (!rd(d,p,v)) return false;                        // 19
        if (!rd(d,p,v)) return false;                        // 20
        for (int k = 0; k < 4; ++k) if (!rd(d,p,b)) return false;   // 21-24
        if (!rd(d,p,v)) return false;                        // 25
        if (!rd(d,p,v)) return false;                        // 26
        if (!rd(d,p,h)) return false;                        // 27
        if (!rd(d,p,h)) return false;                        // 28
        if (!rd(d,p,it.bloomSecs)) return false;             // 29 seconds to bloom
        it.valid = true;
        if (it.id >= 0 && (uint32_t)it.id < count) g_defs[it.id] = it;
    }
    if (p != d.size()) {
        fprintf(stderr, "[gs] items.dat parse desync: %zu of %zu bytes\n", p, d.size());
        return false;
    }
    return true;
}

static const ItemDef* def(int id) {
    if (id < 0 || (size_t)id >= g_defs.size() || !g_defs[id].valid) return nullptr;
    return &g_defs[id];
}
static const char* item_name(int id) {
    const ItemDef* d = def(id);
    return (d && !d->name.empty()) ? d->name.c_str() : "?";
}

static bool load_items(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    g_items.resize((size_t)n);
    size_t got = fread(g_items.data(), 1, (size_t)n, f);
    fclose(f);
    g_items.resize(got);
    g_itemHash = proton_hash(g_items.data(), g_items.size());
    return got > 0 && parse_items(g_items);
}

static void send_item_db(ENetPeer* p) {
    if (g_items.empty()) { printf("  !! no items.dat loaded\n"); return; }
    GameUpdatePacket g{};
    g.packetType = 16;
    g.netID      = -1;
    g.intData    = (int32_t)g_items.size();
    send_gup(p, g, g_items.data(), g_items.size());
    printf("  <- items.dat (%zu bytes) as packetType 16\n", g_items.size());
}

/* ==================================================================== */
/*  eTileMaterial - only the values the client actually branches on      */
/* ==================================================================== */
/*  0   SetTile takes the remove path: clear fg, else clear bg
 *                                              (0x4412b5 / 0x44133e)
 *  2   TileExtra type 1   (Tile::SetItem 0x43e7ac -> 0x43ecb0 -> 0x43eccd)
 *  3   TileExtra type 3, and the renderer draws the lock overlay using the
 *      owner id out of that extra              (0x444662)
 *  4   TileExtra type 2                        (0x43ecd5)
 *  5   punching plays the sound named by the item's third string, rate
 *      limited by ItemInfo+0xa8                (0x43e56b / 0x4445ea)
 *  6   punching toggles tile flag 0x40; the renderer animates while set
 *                                              (0x43e55c / 0x444556)
 *  7   TileExtra type 1                        (0x43eccd)
 * 12   SetTile writes the background           (0x4412de -> 0x43e1f0)
 * 13   seed: only plantable on an empty tile, TileExtra type 4
 * 14   clothing: the inventory equips it by ItemInfo+0x68  (0x43dcbf)
 * 15   used by 0x442cd6
 *
 *  1   NOT PLACEABLE. The world-click handler at 0x436efa is
 *          mov  eax,[ebp+4]        ; the held item's material
 *          cmp  eax,1
 *          sete bl
 *      and on an empty tile `test bl,bl / jne` goes straight to
 *      audio/cant_place_tile.wav without sending anything at all. On an
 *      OCCUPIED tile it instead calls 0x43e450 -- which is
 *          material = ItemInfo[tile->fg].material; return (material-2) <= 2
 *      i.e. true for DOOR(2), LOCK(3) and SIGN(4) -- and only then acts.
 *      So material 1 is the WRENCH: a tool you cannot place, that works on
 *      doors, locks and signs. Giving Dirt/Rock/Lava/Bedrock/Wood material 1
 *      is what made them unplaceable, with the client refusing locally and
 *      the server never hearing about it.
 * 10   the collision response at 0x4474e0 negates the avatar's velocity,
 *      plays audio/burn.wav (0x4cc3a0) and sets state flag 0x40 -- LAVA.
 *
 * 8, 9 and 11 are compared against NOWHERE in the binary and the SetItem
 * switch (index table 0x43e83c) sends them to the no-extra case, so they are
 * the genuinely inert values for a plain block.
 */
enum {
    MAT_FIST       = 0,
    MAT_WRENCH     = 1,
    MAT_DOOR       = 2,
    MAT_LOCK       = 3,
    MAT_SIGN       = 4,
    MAT_BOOMBOX    = 6,
    MAT_USER_DOOR  = 7,
    MAT_INERT      = 8,
    MAT_LAVA       = 10,
    MAT_BACKGROUND = 12,
    MAT_SEED       = 13,
    MAT_CLOTHES    = 14,
};

/* Which TileExtra a foreground item forces onto its tile.
   Index table 0x43ed24, targets 0x43ed10. */
static int extra_type_for_material(int mat) {
    switch (mat) {
        case 2: case 7: return 1;
        case 3:         return 3;
        case 4:         return 2;
        case 13:        return 4;
        default:        return 0;
    }
}

/* ==================================================================== */
/*  World                                                               */
/* ==================================================================== */
/* World::Deserialize 0x43f450
 *     uint16 version              -> world+0x38
 *     uint32                      -> world+0x74   (read at buf+2, so the
 *                                                  cursor starts at 6)
 *     string name
 *     TileMap                     0x441b60
 *       int32 width, int32 height, int32 tileCount
 *       tileCount x Tile          0x43e850
 *         uint16 fg    -> tile+0x00
 *         uint16 bg    -> tile+0x02
 *         uint16       -> tile+0x34
 *         uint16 flags -> tile+0x04
 *         uint16       -> tile+0x34   only if flags bit 1
 *         TileExtra    -> only if flags bit 0
 *     Objects                     0x43ffc0:  int32 count, int32 nextId, list
 *
 * The trap: Tile::Serialize calls SetItem with the fg it just read, and
 * SetItem ORs flags bit 0 in for materials 2/3/4/7/13 BEFORE Serialize
 * tests that bit. So a tile whose foreground needs an extra must be
 * followed by one no matter what flags were written, or the cursor
 * desynchronises and every later tile is garbage.
 */
struct TileExtra {
    uint8_t  type = 0;
    std::string s1, s2, s3;     // type 1 (label / destination / password)
    uint8_t  b1 = 0;            // type 1 trailing uint8, type 3 leading uint8
    uint32_t u1 = 0;            // type 2 uint32, type 3 owner id
    std::vector<uint32_t> list; // type 3 access list
    uint32_t age = 0;           // type 4 seconds since planted
    uint8_t  stage = 0;         // type 4 growth stage
};

struct Tile {
    uint16_t  fg = 0, bg = 0;
    uint16_t  flags = 0;        // bit 0x20 mirrors directional tile artwork
    TileExtra extra;
    uint8_t   damage = 0;       // punches taken, mirrors client Tile+0x28
    time_t    healAt = 0;
    time_t    plantedAt = 0;    // seeds: when it went in, for the age in the extra
};

/* A dropped item lying in the world. The record the client keeps is
   16 bytes and the serialiser at 0x43f990 walks it in exactly this order:
       uint16 itemId, float x, float y, uint8 count, uint8 flags, uint32 id
   The id is NOT transmitted when one is spawned -- AddObject (0x43ff8a) does
   `[obj+0x10] = ++nextId` on the client's own counter -- so the server has to
   run the same counter in the same order or the two disagree about which
   object a later collect refers to. */
struct WorldObject {
    uint32_t id     = 0;
    uint16_t itemId = 0;
    float    x = 0, y = 0;
    uint8_t  count  = 1;
    uint8_t  flags  = 0;
};

/* A lock and the tiles it covers. packetType 15 (World::ApplyPacket ->
   0x441070) places the lock item, sets the TileExtra's owner from the packet's
   netID (0x43ee10 writes extra+8), and then walks netID2 uint16s out of the
   extended data as TILE INDICES, calling 0x43e2e0 on each: that writes the
   lock's own index into tile+0x34 and ORs tile flag 0x02. So a lock's area is
   an explicit LIST, not a radius -- there is no radius anywhere in the binary
   to find, the original server chose the set of tiles just as we do.

   The client draws the area (every read of tile+0x34 is in the renderer,
   0x444dda..0x445026) and picks the lock's frame from whether you are the owner
   or in the extra's access list (0x444662 / 0x43ee20), but it does NOT refuse
   anything. Enforcement is the server's, which is what audio/punch_locked.wav
   was for. */
struct WorldLock {
    int x = 0, y = 0;
    int owner = 0;                  /* netID of whoever placed it */
    std::vector<uint16_t> tiles;    /* tile indices it covers */
};

struct WorldData {
    std::string name;
    int w = 0, h = 0;
    std::vector<Tile> tiles;
    int spawnX = 0, spawnY = 0; // pixels
    Tile& at(int x, int y) { return tiles[(size_t)y * w + x]; }
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    std::vector<WorldObject> objects;
    uint32_t nextObjectId = 0;
    std::vector<WorldLock> locks;
};

static std::map<std::string, WorldData> g_worlds;
static volatile sig_atomic_t g_stop = 0;

static void request_stop(int) { g_stop = 1; }

/* ---------- World persistence ----------
   This is deliberately a server-owned format, separate from the client world
   blob. Files are replaced atomically so an interrupted write keeps the last
   good copy. World names are hex-encoded, so names such as ../foo can never
   escape the save directory. */
static std::string world_dir() {
    const char* p = getenv("BUILDO_WORLD_DIR");
    return (p && *p) ? p : "worlds";
}

static std::string world_path(const std::string& name) {
    static const char hex[] = "0123456789abcdef";
    std::string out = world_dir() + "/";
    for (size_t i = 0; i < name.size(); ++i) {
        unsigned char c = (unsigned char)name[i];
        out += hex[c >> 4]; out += hex[c & 15];
    }
    return out + ".bworld";
}

static bool ensure_world_dir() {
    std::string dir = world_dir();
    if (mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST) return true;
    fprintf(stderr, "[gs] cannot create world directory '%s': %s\n",
            dir.c_str(), strerror(errno));
    return false;
}

static void file_u8(FILE* f, uint8_t v) { fwrite(&v, 1, 1, f); }
static void file_u16(FILE* f, uint16_t v) { fwrite(&v, sizeof v, 1, f); }
static void file_u32(FILE* f, uint32_t v) { fwrite(&v, sizeof v, 1, f); }
static void file_i64(FILE* f, int64_t v) { fwrite(&v, sizeof v, 1, f); }
static void file_float(FILE* f, float v) { fwrite(&v, sizeof v, 1, f); }
static void file_string(FILE* f, const std::string& s) {
    file_u32(f, (uint32_t)s.size());
    if (!s.empty()) fwrite(s.data(), 1, s.size(), f);
}

static bool save_world(const WorldData& w) {
    if (!ensure_world_dir()) return false;
    std::string path = world_path(w.name), tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "[gs] cannot open world save '%s': %s\n",
                tmp.c_str(), strerror(errno));
        return false;
    }
    fwrite("BLDWORLD", 1, 8, f); file_u32(f, 2);
    file_string(f, w.name); file_u32(f, (uint32_t)w.w); file_u32(f, (uint32_t)w.h);
    file_u32(f, (uint32_t)w.spawnX); file_u32(f, (uint32_t)w.spawnY);
    file_u32(f, (uint32_t)w.tiles.size());
    for (size_t i = 0; i < w.tiles.size(); ++i) {
        const Tile& t = w.tiles[i];
        file_u16(f, t.fg); file_u16(f, t.bg); file_u16(f, t.flags);
        file_u8(f, t.damage);
        file_i64(f, (int64_t)t.healAt); file_i64(f, (int64_t)t.plantedAt);
        file_u8(f, t.extra.type); file_string(f, t.extra.s1);
        file_string(f, t.extra.s2); file_string(f, t.extra.s3);
        file_u8(f, t.extra.b1); file_u32(f, t.extra.u1);
        file_u32(f, t.extra.age); file_u8(f, t.extra.stage);
        file_u32(f, (uint32_t)t.extra.list.size());
        for (size_t k = 0; k < t.extra.list.size(); ++k) file_u32(f, t.extra.list[k]);
    }
    file_u32(f, w.nextObjectId); file_u32(f, (uint32_t)w.objects.size());
    for (size_t i = 0; i < w.objects.size(); ++i) {
        const WorldObject& o = w.objects[i];
        file_u32(f, o.id); file_u16(f, o.itemId); file_float(f, o.x); file_float(f, o.y);
        file_u8(f, o.count); file_u8(f, o.flags);
    }
    file_u32(f, (uint32_t)w.locks.size());
    for (size_t i = 0; i < w.locks.size(); ++i) {
        const WorldLock& l = w.locks[i];
        file_u32(f, (uint32_t)l.x); file_u32(f, (uint32_t)l.y);
        file_u32(f, (uint32_t)l.owner); file_u32(f, (uint32_t)l.tiles.size());
        for (size_t k = 0; k < l.tiles.size(); ++k) file_u16(f, l.tiles[k]);
    }
    bool ok = !ferror(f) && fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (ok) ok = rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) { fprintf(stderr, "[gs] failed saving world '%s': %s\n", w.name.c_str(), strerror(errno)); remove(tmp.c_str()); }
    return ok;
}

static bool read_exact(FILE* f, void* p, size_t n) { return fread(p, 1, n, f) == n; }
static bool read_u8(FILE* f, uint8_t& v) { return read_exact(f, &v, 1); }
static bool read_u16(FILE* f, uint16_t& v) { return read_exact(f, &v, sizeof v); }
static bool read_u32(FILE* f, uint32_t& v) { return read_exact(f, &v, sizeof v); }
static bool read_i64(FILE* f, int64_t& v) { return read_exact(f, &v, sizeof v); }
static bool read_float(FILE* f, float& v) { return read_exact(f, &v, sizeof v); }
static bool read_string(FILE* f, std::string& s, uint32_t limit = 65536) {
    uint32_t n; if (!read_u32(f, n) || n > limit) return false;
    s.resize(n); return n == 0 || read_exact(f, &s[0], n);
}

static bool load_world(const std::string& requested, WorldData& w) {
    std::string path = world_path(requested);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[8]; uint32_t version, width, height, count, u; int64_t stamp;
    bool ok = read_exact(f, magic, 8) && memcmp(magic, "BLDWORLD", 8) == 0 &&
              read_u32(f, version) && (version == 1 || version == 2) &&
              read_string(f, w.name, 1024) &&
              w.name == requested && read_u32(f, width) && read_u32(f, height) &&
              width > 0 && height > 0 && width <= 1000 && height <= 1000;
    if (ok) { w.w = (int)width; w.h = (int)height; ok = read_u32(f, u); w.spawnX = (int)u; }
    if (ok) { ok = read_u32(f, u); w.spawnY = (int)u; }
    if (ok) ok = read_u32(f, count) && count == width * height && count <= 1000000;
    if (ok) w.tiles.resize(count);
    for (uint32_t i = 0; ok && i < count; ++i) {
        Tile& t = w.tiles[i]; uint32_t listCount; stamp = 0;
        ok = read_u16(f, t.fg) && read_u16(f, t.bg);
        if (ok && version >= 2) ok = read_u16(f, t.flags);
        if (ok) ok = read_u8(f, t.damage) && read_i64(f, stamp);
        t.healAt = (time_t)stamp;
        if (ok) { ok = read_i64(f, stamp); t.plantedAt = (time_t)stamp; }
        if (ok) ok = read_u8(f, t.extra.type) && read_string(f, t.extra.s1) &&
                     read_string(f, t.extra.s2) && read_string(f, t.extra.s3) &&
                     read_u8(f, t.extra.b1) && read_u32(f, t.extra.u1) &&
                     read_u32(f, t.extra.age) && read_u8(f, t.extra.stage) &&
                     read_u32(f, listCount) && listCount <= 100000;
        if (ok) t.extra.list.resize(listCount);
        for (uint32_t k = 0; ok && k < listCount; ++k) ok = read_u32(f, t.extra.list[k]);
    }
    uint32_t objectCount = 0, lockCount = 0;
    if (ok) ok = read_u32(f, w.nextObjectId) && read_u32(f, objectCount) && objectCount <= 100000;
    if (ok) w.objects.resize(objectCount);
    for (uint32_t i = 0; ok && i < objectCount; ++i) {
        WorldObject& o = w.objects[i];
        ok = read_u32(f, o.id) && read_u16(f, o.itemId) && read_float(f, o.x) &&
             read_float(f, o.y) && read_u8(f, o.count) && read_u8(f, o.flags);
    }
    if (ok) ok = read_u32(f, lockCount) && lockCount <= 100000;
    if (ok) w.locks.resize(lockCount);
    for (uint32_t i = 0; ok && i < lockCount; ++i) {
        WorldLock& l = w.locks[i]; uint32_t tileCount;
        ok = read_u32(f, u); l.x = (int)u;
        if (ok) { ok = read_u32(f, u); l.y = (int)u; }
        if (ok) { ok = read_u32(f, u); l.owner = (int)u; }
        if (ok) ok = read_u32(f, tileCount) && tileCount <= count;
        if (ok) l.tiles.resize(tileCount);
        for (uint32_t k = 0; ok && k < tileCount; ++k) ok = read_u16(f, l.tiles[k]);
    }
    fclose(f);
    if (!ok) fprintf(stderr, "[gs] ignored invalid world save '%s'\n", path.c_str());
    return ok;
}

static void save_all_worlds() {
    for (std::map<std::string, WorldData>::const_iterator it = g_worlds.begin();
         it != g_worlds.end(); ++it) save_world(it->second);
}

static const int WORLD_W = 100, WORLD_H = 60;
static const int GROUND  = 30;           // first solid row

/* Item ids from game/item_definitions.txt. */
enum { IT_BLANK = 0, IT_DIRT = 2, IT_DOOR = 6, IT_BEDROCK = 8, IT_ROCK = 10,
       IT_CAVE_WALL = 14, IT_FIST = 18, IT_SIGN = 20, IT_WRENCH = 32,
       IT_WOOD_WALL = 52, IT_LOCK = 60, IT_BOOMBOX = 62, IT_RADIO = 64,
       /* The one item id the client hard-codes: an object with item 112 is
          picked up straight into the bux counter (0x434206), not the
          inventory. make_items.py synthesises it with tiles_bux.rttex. */
       IT_GEMS = 112, IT_PLATFORM = 102 };

/* A fresh world: bedrock floor, dirt below ground level, cave wall behind it,
   and a main door with a sign beside it at the spawn point. */
static WorldData& get_world(const std::string& name) {
    auto it = g_worlds.find(name);
    if (it != g_worlds.end()) return it->second;

    WorldData saved;
    if (load_world(name, saved)) {
        printf("[gs] loaded world '%s' %dx%d from disk\n", name.c_str(), saved.w, saved.h);
        return g_worlds.emplace(name, std::move(saved)).first->second;
    }

    WorldData w;
    w.name = name;
    w.w = WORLD_W; w.h = WORLD_H;
    w.tiles.assign((size_t)WORLD_W * WORLD_H, Tile());
    const int doorX = WORLD_W / 2, doorY = GROUND - 1;

    for (int y = 0; y < WORLD_H; ++y) {
        for (int x = 0; x < WORLD_W; ++x) {
            Tile& t = w.at(x, y);
            if (y == WORLD_H - 1)      t.fg = IT_BEDROCK;
            else if (y >= GROUND)      t.fg = IT_DIRT;
            if (y >= GROUND)           t.bg = IT_CAVE_WALL;
        }
    }
    /* BUILDO_FEAT selects which special tiles the world contains, so the
       TileExtra wire format can be bisected one material at a time:
         d door (material 2, extra type 1)   s sign (material 4, extra type 2)
         l lock (material 3, extra type 3)   p seed (material 13, extra 4)
         b boombox / r radio (material 6, no extra) -- the material-6 punch
           toggles tile flag 0x40 and the renderer then animates the tile,
           which is the path that used to divide by a zero animation period */
    const char* feat = getenv("BUILDO_FEAT");
    if (!feat) feat = getenv("BUILDO_NO_DOOR") ? "" : "ds";
    if (strchr(feat, 'd')) {
        Tile& t = w.at(doorX, doorY);
        /* Which door the generator lays down. All three of the build's doors --
           6 Door (material 2), 12 User Door and 30 Dungeon Door (both material
           7) -- get a type-1 TileExtra, which is exactly what the client's
           door-entry check tests (0x43e280: extra->type == 1), so all three are
           enterable. */
        const char* di = getenv("BUILDO_DOOR_ITEM");
        t.fg = (uint16_t)(di ? atoi(di) : IT_DOOR);
        t.extra.type = 1;
        /* A door's one string is where it goes; EXIT means out to the world
           list. BUILDO_DOOR_DEST points the main door at another world, which
           is what wrenching it does in game. */
        const char* dd = getenv("BUILDO_DOOR_DEST");
        t.extra.s1 = dd ? dd : "EXIT";
    }
    if (strchr(feat, 's')) {
        Tile& t = w.at(doorX + 2, doorY);
        t.fg = IT_SIGN; t.extra.type = 2; t.extra.s1 = "`2" + name + "``";
    }
    if (strchr(feat, 'l')) {
        Tile& t = w.at(doorX - 2, doorY);
        t.fg = IT_LOCK; t.extra.type = 3; t.extra.u1 = 1;
    }
    if (strchr(feat, 'p')) {
        Tile& t = w.at(doorX + 4, doorY);
        t.fg = 3; t.extra.type = 4;
        t.extra.stage = (uint8_t)atoi(getenv("BUILDO_SEED_STAGE")
                                      ? getenv("BUILDO_SEED_STAGE") : "0");
        t.plantedAt = time(NULL);
    }
    /* 'w' lays a run of wood platforms two rows above the ground: collision 2,
       the one-way platform, is the only pinned enum value nothing shipped in
       item_definitions.txt could have used. */
    if (strchr(feat, 'w'))
        for (int i = -3; i <= 3; ++i) w.at(doorX + 6 + i, GROUND - 3).fg = IT_PLATFORM;
    if (strchr(feat, 'b')) w.at(doorX - 1, doorY).fg = IT_BOOMBOX;
    if (strchr(feat, 'r')) w.at(doorX + 1, doorY).fg = IT_RADIO;
    w.spawnX = 32 * doorX;
    w.spawnY = 32 * (doorY - 1);
    if (getenv("BUILDO_SPAWN_X")) w.spawnX = atoi(getenv("BUILDO_SPAWN_X"));
    if (getenv("BUILDO_SPAWN_Y")) w.spawnY = atoi(getenv("BUILDO_SPAWN_Y"));

    printf("[gs] generated world '%s' %dx%d, spawn %d,%d\n",
           name.c_str(), w.w, w.h, w.spawnX, w.spawnY);
    WorldData& created = g_worlds.emplace(name, std::move(w)).first->second;
    save_world(created);
    return created;
}

/* TileExtra::Serialize 0x43f070: uint8 type, then by type
     1  (0x43f0b2)  one string + uint8      when the caller's arg5 != 0
        (0x43f130)  three strings + uint8   when arg5 == 0
     2  (0x43f16d)  string + uint32
     3  (0x43f1d6)  uint8 + uint32 + uint32 count + count x uint32
     4  (0x43f18f)  uint32 + uint8   (the plant time is stamped to "now" on
                                      read, not transmitted)
   Which shape type 1 takes in the world blob is chosen by an argument
   TileMap::Serialize passes down; BUILDO_DOOR_EXTRA overrides it so the two
   can be told apart by experiment. */
static void write_extra(std::vector<uint8_t>& b, const TileExtra& e) {
    if (!e.type) return;
    b.push_back(e.type);
    switch (e.type) {
    case 1: {
        /* Measured, not guessed: with the one-string shape the blob we send
           and the blob the client reports decompressing are the same size
           (48041) and every tile after the door survives; with the
           three-string shape the client consumes a different number of
           bytes and the parse runs off the end. So the world blob takes the
           arg5 != 0 branch at 0x43f0bd. BUILDO_DOOR_EXTRA=3 restores the
           other shape for re-testing. */
        const char* mode = getenv("BUILDO_DOOR_EXTRA");
        if (mode && atoi(mode) == 3) { wstr(b, e.s1); wstr(b, e.s2); wstr(b, e.s3); }
        else                         { wstr(b, e.s1); }
        b.push_back(e.b1);
        break;
    }
    case 2:
        wstr(b, e.s1);
        w32(b, e.u1);
        break;
    case 3:
        b.push_back(e.b1);
        w32(b, e.u1);
        w32(b, (uint32_t)e.list.size());
        for (size_t i = 0; i < e.list.size(); ++i) w32(b, e.list[i]);
        break;
    case 4:
        w32(b, e.age);
        b.push_back(e.stage);
        break;
    default: break;
    }
}

static std::vector<uint8_t> serialize_world(WorldData& w) {
    const uint16_t WORLD_VERSION =
        (uint16_t)(getenv("BUILDO_WORLD_VER") ? atoi(getenv("BUILDO_WORLD_VER")) : 2);
    std::vector<uint8_t> b;
    w16(b, WORLD_VERSION);
    w32(b, 0);
    wstr(b, w.name);

    w32(b, (uint32_t)w.w);
    w32(b, (uint32_t)w.h);
    w32(b, (uint32_t)(w.w * w.h));

    for (int y = 0; y < w.h; ++y) {
        for (int x = 0; x < w.w; ++x) {
            Tile& t = w.at(x, y);
            /* Only persist the artwork-mirror bit. The client sets bit 0
               itself for materials that need a TileExtra. */
            w16(b, t.fg);
            w16(b, t.bg);
            w16(b, 0);
            w16(b, (uint16_t)(t.flags & 0x20));
            const ItemDef* d = def(t.fg);
            int want = d ? extra_type_for_material(d->material) : 0;
            if (want && !getenv("BUILDO_NO_EXTRA")) {
                if (t.extra.type != want) {   // keep the blob self-consistent
                    t.extra = TileExtra();
                    t.extra.type = (uint8_t)want;
                }
                /* A type-4 extra carries the plant age in seconds, and the
                   client stamps tile+0x60 to "now" as it reads it, so the age
                   it computes later is this value plus the time since load
                   (0x43ed30). Send how long ago it actually went in. */
                if (want == 4 && t.plantedAt)
                    t.extra.age = (uint32_t)(time(NULL) - t.plantedAt);
                write_extra(b, t.extra);
            }
        }
    }
    /* Objects (0x43ffc0): count, then nextId, then one 16-byte record each
       (0x43f990). The count written is the list size and the client uses it as
       its loop bound; nextId seeds the counter AddObject increments. */
    w32(b, (uint32_t)w.objects.size());
    w32(b, w.nextObjectId);
    for (size_t i = 0; i < w.objects.size(); ++i) {
        const WorldObject& o = w.objects[i];
        w16(b, o.itemId);
        wf (b, o.x);
        wf (b, o.y);
        w8 (b, o.count);
        w8 (b, o.flags);
        w32(b, o.id);
    }
    return b;
}

/* ==================================================================== */
/*  Players                                                             */
/* ==================================================================== */
struct InvSlot { uint16_t id; uint8_t amount; uint8_t flags; };

struct Player {
    std::string name;
    int   netID   = 0;
    bool  inWorld = false;
    std::string world;
    std::vector<InvSlot> inv;
    /* Six worn slots, indexed by ItemInfo+0x68 (the body part). The client
       keeps the same six at PlayerItems+6..+0x11 (0x43dcd0) and
       OnSetClothing carries them as two vec3s (handler 0x448ad0). */
    uint16_t cloth[6];
    uint32_t skin;
    bool  clothPending = false;
    std::string pendingAction;   // BUILDO_TEST_ACTION, held until the netObject exists
    int   gems = 0;
    float x = 0, y = 0;
    int   stateCount = 0;
    Player() { memset(cloth, 0, sizeof cloth); skin = 0xFFFFFFFFu; }
};
static std::map<ENetPeer*, Player> g_players;

/* PlayerItems::Serialize 0x43db00 (packetType 9):
     uint8 capacity, uint8 slotCount, slotCount x {uint16 id, uint8 amount,
     uint8 flags}.  flags bit 0 means "worn", and the client equips it using
     the item's body part (0x43dcbf). */
static void send_inventory(ENetPeer* p, const Player& pl) {
    std::vector<uint8_t> ext;
    ext.push_back(200);                         /* capacity */
    ext.push_back((uint8_t)pl.inv.size());
    for (size_t i = 0; i < pl.inv.size(); ++i) {
        ext.push_back((uint8_t)(pl.inv[i].id & 0xFF));
        ext.push_back((uint8_t)(pl.inv[i].id >> 8));
        ext.push_back(pl.inv[i].amount);
        ext.push_back(pl.inv[i].flags);
    }
    GameUpdatePacket g{};
    g.packetType = 9;
    g.netID      = -1;
    send_gup(p, g, ext.data(), ext.size());
    printf("  <- inventory (%zu slots)\n", pl.inv.size());
}

/* The client caps a slot at 99: PlayerItems::Add (0x43dda6) is
       if (amount + n > 0x63) n = 0x63 - amount;
   so handing out 100 of something meant the very first pickup "added" -1 and
   the count visibly dropped to 99. Match the client exactly. */
enum { INV_SLOT_MAX = 99 };

static void inv_add(Player& pl, uint16_t id, int n) {
    for (size_t i = 0; i < pl.inv.size(); ++i)
        if (pl.inv[i].id == id) {
            int v = pl.inv[i].amount + n;
            pl.inv[i].amount = (uint8_t)(v > INV_SLOT_MAX ? INV_SLOT_MAX : v);
            return;
        }
    if (pl.inv.size() < 200) {
        InvSlot s; s.id = id;
        s.amount = (uint8_t)(n > INV_SLOT_MAX ? INV_SLOT_MAX : n); s.flags = 0;
        pl.inv.push_back(s);
    }
}
static bool inv_take(Player& pl, uint16_t id, int n) {
    for (size_t i = 0; i < pl.inv.size(); ++i) {
        if (pl.inv[i].id != id) continue;
        if (pl.inv[i].amount < n) return false;
        pl.inv[i].amount -= (uint8_t)n;
        if (pl.inv[i].amount == 0) pl.inv.erase(pl.inv.begin() + i);
        return true;
    }
    return false;
}
static bool inv_has(const Player& pl, uint16_t id) {
    for (size_t i = 0; i < pl.inv.size(); ++i)
        if (pl.inv[i].id == id && pl.inv[i].amount) return true;
    return false;
}

/* game/item_definitions.txt marks the Fist and the Wrench with
   "set_max_can_hold|id|0|". Which ItemInfo field that lands in is not
   identified, so items.dat cannot carry it; the server reads the same line
   out of the original text file instead so those two stay single copies. */
static std::vector<int> load_unstackable() {
    std::vector<int> out;
    const char* env = getenv("BUILDO_ITEM_DEFS");
    std::string path = env ? env : (std::string(getenv("HOME") ? getenv("HOME") : ".")
                                    + "/buildo-run/game/item_definitions.txt");
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return out;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        int id = 0, n = -1;
        if (sscanf(line, "set_max_can_hold|%d|%d|", &id, &n) == 2 && n == 0)
            out.push_back(id);
    }
    fclose(f);
    return out;
}

/* The starting kit. The published build ships no server, so nothing pins
   what a new player owned; these are the tools the client needs to be able
   to do anything (Fist to break, Wrench to inspect) plus building stock. */
static void give_starter_kit(Player& pl) {
    pl.inv.clear();
    /* BUILDO_INV="id:count,id:count,..." overrides it, for experiments. */
    if (const char* e = getenv("BUILDO_INV")) {
        const char* p = e;
        while (*p) {
            int id = atoi(p);
            const char* c = strchr(p, ':');
            int amt = c ? atoi(c + 1) : 1;
            if (id > 0) inv_add(pl, (uint16_t)id, amt ? amt : 1);
            const char* comma = strchr(p, ',');
            if (!comma) break;
            p = comma + 1;
        }
        /* fall through to BUILDO_WEAR: returning here made the two knobs
           mutually exclusive, so BUILDO_WEAR silently did nothing whenever
           BUILDO_INV was set. */
    }
    else {
    /* Nothing in the published build says what a new player owned -- there
       was no server. Handing over everything item_definitions.txt defines
       makes every tile, sprite rule and clothing slot in the build reachable
       in game, which is the point of running it at all. */
    std::vector<int> single = load_unstackable();
    for (size_t i = 0; i < g_defs.size(); ++i) {
        const ItemDef& d = g_defs[i];
        if (!d.valid || d.name.empty()) continue;
        if (d.id == IT_BLANK || d.name == "Unused") continue;
        bool one = d.material == MAT_CLOTHES;
        for (size_t k = 0; k < single.size(); ++k) if (single[k] == d.id) one = true;
        inv_add(pl, (uint16_t)d.id, one ? 1 : INV_SLOT_MAX);
    }
    }
    /* BUILDO_WEAR="id,id,..." marks items worn (inventory flag bit 0), which
       is what makes the client equip them by body part (0x43dcbf). */
    if (const char* w = getenv("BUILDO_WEAR")) {
        const char* p = w;
        while (*p) {
            int id = atoi(p);
            if (id > 0) {
                inv_add(pl, (uint16_t)id, 1);
                for (size_t i = 0; i < pl.inv.size(); ++i)
                    if (pl.inv[i].id == id) pl.inv[i].flags |= 1;
            }
            const char* c = strchr(p, ',');
            if (!c) break;
            p = c + 1;
        }
    }
}

/* ---------- broadcast ---------- */
static void broadcast_gup(const std::string& world, const GameUpdatePacket& g,
                          ENetPeer* skip = nullptr,
                          const uint8_t* ext = nullptr, size_t extLen = 0) {
    for (std::map<ENetPeer*, Player>::iterator it = g_players.begin();
         it != g_players.end(); ++it) {
        if (it->first == skip) continue;
        if (!it->second.inWorld || it->second.world != world) continue;
        send_gup(it->first, g, ext, extLen);
    }
}

static void broadcast_call(const std::string& world, const std::vector<Variant>& vl,
                           int32_t netID = -1, ENetPeer* skip = nullptr) {
    for (std::map<ENetPeer*, Player>::iterator it = g_players.begin();
         it != g_players.end(); ++it) {
        if (it->first == skip) continue;
        if (!it->second.inWorld || it->second.world != world) continue;
        call_fn(it->first, vl, netID);
    }
}

/* Rebuild the six worn slots from the inventory flags. */
static void refresh_clothing(Player& pl) {
    memset(pl.cloth, 0, sizeof pl.cloth);
    for (size_t i = 0; i < pl.inv.size(); ++i) {
        if (!(pl.inv[i].flags & 1)) continue;
        const ItemDef* d = def(pl.inv[i].id);
        if (!d || d->material != MAT_CLOTHES) continue;
        if (d->bodyPart < 6) pl.cloth[d->bodyPart] = pl.inv[i].id;
    }
}

/* Cosmetics are account state rather than world state. The prototype has no
   authentication, so the requested player name is the stable profile key. */
static std::string player_dir() {
    const char* p = getenv("BUILDO_PLAYER_DIR");
    return (p && *p) ? p : "players";
}

static std::string player_path(const std::string& name) {
    static const char hex[] = "0123456789abcdef";
    std::string out = player_dir() + "/";
    for (size_t i = 0; i < name.size(); ++i) {
        unsigned char c = (unsigned char)name[i];
        out += hex[c >> 4]; out += hex[c & 15];
    }
    return out + ".bplayer";
}

static bool save_player(const Player& pl) {
    if (pl.name.empty()) return false;
    std::string dir = player_dir();
    if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return false;
    std::string path = player_path(pl.name), tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    fwrite("BLDPLAYER", 1, 9, f); file_u32(f, 1); file_string(f, pl.name);
    file_u32(f, pl.skin);
    for (size_t i = 0; i < 6; ++i) file_u16(f, pl.cloth[i]);
    bool ok = !ferror(f) && fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (ok) ok = rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) remove(tmp.c_str());
    return ok;
}

static bool load_player(Player& pl) {
    FILE* f = fopen(player_path(pl.name).c_str(), "rb");
    if (!f) return false;
    char magic[9]; uint32_t version, skin; std::string savedName;
    uint16_t clothes[6] = {};
    bool ok = read_exact(f, magic, 9) && memcmp(magic, "BLDPLAYER", 9) == 0 &&
              read_u32(f, version) && version == 1 &&
              read_string(f, savedName, 1024) && savedName == pl.name &&
              read_u32(f, skin);
    for (size_t i = 0; ok && i < 6; ++i) ok = read_u16(f, clothes[i]);
    fclose(f);
    if (!ok) return false;

    pl.skin = skin;
    for (size_t i = 0; i < pl.inv.size(); ++i) pl.inv[i].flags &= (uint8_t)~1;
    for (size_t part = 0; part < 6; ++part) {
        uint16_t id = clothes[part];
        const ItemDef* d = def(id);
        if (!id || !d || d->material != MAT_CLOTHES || d->bodyPart != part) continue;
        if (!inv_has(pl, id)) inv_add(pl, id, 1);
        for (size_t i = 0; i < pl.inv.size(); ++i)
            if (pl.inv[i].id == id) pl.inv[i].flags |= 1;
    }
    refresh_clothing(pl);
    printf("[gs] loaded cosmetics for '%s'\n", pl.name.c_str());
    return true;
}

static void save_all_players() {
    for (std::map<ENetPeer*, Player>::const_iterator it = g_players.begin();
         it != g_players.end(); ++it) save_player(it->second);
}

/* OnSetClothing(vec3 slots 0-2, vec3 slots 3-5, uint32 skin colour).
   The handler at 0x448ad0 reads both vec3s field by field, rounds each to a
   uint16 and writes six slots; the third argument goes to 0x449ff0 as the
   skin colour, and it defaults to 0 when omitted, so it is always sent. */
static void send_clothing(ENetPeer* p, const Player& pl) {
    call_fn(p, { Variant::str("OnSetClothing"),
                 Variant::vec3((float)pl.cloth[0], (float)pl.cloth[1], (float)pl.cloth[2]),
                 Variant::vec3((float)pl.cloth[3], (float)pl.cloth[4], (float)pl.cloth[5]),
                 Variant::u32(pl.skin) }, pl.netID);
}

static std::string spawn_text(const Player& pl, const WorldData& w, bool local) {
    char buf[512];
    snprintf(buf, sizeof buf,
        /* Only keys this build actually parses. "invis|" and "mstate|" are
           NOT strings in this binary at all -- they come from later Growtopia
           versions, and the client was silently ignoring them. */
        "spawn|avatar\nnetID|%d\nuserID|%d\ncolrect|0|0|20|30\n"
        "posXY|%d|%d\nname|`w%s``\ncountry|us\n%s",
        pl.netID, pl.netID, w.spawnX, w.spawnY, pl.name.c_str(),
        local ? "type|local\n" : "");
    return buf;
}

static void console(ENetPeer* p, const std::string& msg) {
    call_fn(p, { Variant::str("OnConsoleMessage"), Variant::str(msg) });
}

/* ==================================================================== */
/*  Tile edits                                                           */
/* ==================================================================== */
/* World::ApplyPacket 0x43f270 dispatches what the server sends:
 *   packetType 3 -> WorldTileMap::SetTile(intX, intY, intData)   0x441260
 *                   material 0  clears the foreground, else the background
 *                   material 12 writes the background
 *                   material 13 plants a seed on an empty tile
 *                   anything else writes the foreground
 *   packetType 8 -> Tile::Damage(intData)                        0x43e4a0
 *                   damage = min(damage + n, ItemInfo.hp), and the heal
 *                   deadline moves to now + ItemInfo.healSecs; the renderer
 *                   draws game/crack.rttex from that.
 * Both need a real netID: the client looks the actor up and bails if it is
 * not a NetObject it knows (0x433b7d).
 */
static void send_set_tile(const std::string& world, int x, int y, int itemId,
                          int actorNetID, bool flipped = false) {
    GameUpdatePacket g{};
    g.packetType = 3;
    g.netID      = actorNetID;
    g.intData    = itemId;
    g.intX       = x;
    g.intY       = y;
    if (flipped) g.flags |= 0x10; /* ApplyPacket turns this into tile flag 0x20 */
    broadcast_gup(world, g);
}

static void send_damage_tile(const std::string& world, int x, int y, int amount,
                             int actorNetID) {
    GameUpdatePacket g{};
    g.packetType = 8;
    g.netID      = actorNetID;
    g.intData    = amount;
    g.intX       = x;
    g.intY       = y;
    broadcast_gup(world, g);
}

/* ==================================================================== */
/*  Dropped objects                                                      */
/* ==================================================================== */
/* World::ApplyPacket (0x43f270) hands packetType 14 to the object manager
   (0x440150), which reads it as:

     netID == -1   SPAWN: AddObject(intData, (vecX,vecY), (int)floatVar,
                          objType)   -- 0x43feb0, and mind which argument is
                          which: AddObject stores arg3 into obj+0x0e (the
                          COUNT) and arg4 into obj+0x0f (the flags byte), so
                          the count travels in floatVar and NOT in objType.
                          Sending it in objType gives every drop count 0, and
                          picking one up then adds nothing at all while still
                          looking perfectly normal on screen -- the sprite
                          comes from the item id. The object's id is the
                          client's own ++nextId, so we mirror the counter.
     otherwise     REMOVE object [intData]. The client at 0x4341ba then sees
                   whether that netID is itself and, if so, puts the item in
                   its inventory -- or, for item 112, straight onto the bux
                   counter (0x434215 `add [app+0x130], count`).
*/
static void send_object_spawn(const std::string& world, const WorldObject& o) {
    GameUpdatePacket g{};
    g.packetType = 14;
    g.netID      = -1;
    g.objType    = o.flags;          /* +0x01 -> obj+0x0f, the flags  */
    g.intData    = o.itemId;         /* +0x14                         */
    g.floatVar   = (float)o.count;   /* +0x10 -> obj+0x0e, the COUNT  */
    g.vecX       = o.x;
    g.vecY       = o.y;
    broadcast_gup(world, g, NULL);
}

static void spawn_object(WorldData& w, int itemId, int count, float x, float y) {
    if (count <= 0) return;
    const ItemDef* d = def(itemId);
    if (!d) return;
    WorldObject o;
    o.id     = ++w.nextObjectId;     /* matches AddObject's ++nextId */
    o.itemId = (uint16_t)itemId;
    o.count  = (uint8_t)(count > 200 ? 200 : count);
    o.x = x; o.y = y;
    w.objects.push_back(o);
    send_object_spawn(w.name, o);
    printf("  ** dropped %d x %s (object %u) at %.0f,%.0f\n",
           o.count, d->name.c_str(), o.id, x, y);
}

/* A tile's pixel box is 32x32; drop into the middle of it. */
static void drop_from_tile(WorldData& w, int itemId, int count, int tx, int ty) {
    spawn_object(w, itemId, count, (float)(tx * 32 + 8), (float)(ty * 32 + 8));
}

/* Gems out of a broken block. What IS pinned: item 112 is the gem, picking one
   up adds to the counter the HUD draws, and the inventory refuses the id. What
   is NOT pinned anywhere is where gems come from or how often -- so the rate
   lives in one place, as a percentage, and BUILDO_GEM_CHANCE=0 turns gem drops
   off completely for anyone who wants only what the binary proves. */
static int gems_for_block(const ItemDef* d) {
    if (!d) return 0;
    const char* e = getenv("BUILDO_GEM_CHANCE");
    int chance = e ? atoi(e) : 50;
    if (chance <= 0 || rand() % 100 >= chance) return 0;
    int n = 1 + (d->hp > 3 ? rand() % d->hp : 0);
    return n > 20 ? 20 : n;
}

/* The collector's client does its own inventory/bux update when it sees this,
   so the server must not also resend the inventory or it double-counts. */
static void collect_object(Player& pl, WorldData& w, size_t idx) {
    WorldObject o = w.objects[idx];
    w.objects.erase(w.objects.begin() + idx);
    GameUpdatePacket g{};
    g.packetType = 14;
    g.netID      = pl.netID;
    g.intData    = (int32_t)o.id;
    broadcast_gup(w.name, g, NULL);
    if (o.itemId == IT_GEMS) {
        pl.gems += o.count;
        printf("  ** %s picked up %u gems (total %d)\n",
               pl.name.c_str(), o.count, pl.gems);
    } else {
        inv_add(pl, o.itemId, o.count);
        printf("  ** %s picked up %u x %s\n", pl.name.c_str(), o.count,
               item_name(o.itemId));
    }
}

/* Lava. Material 10 is pinned as lava by the collision response at 0x4474e0 --
   it negates your velocity, plays audio/burn.wav and raises a state flag -- and
   OnKilled (handler 0x447660) is pinned as the death call: it takes no
   arguments, plays audio/male_scream.wav and runs a death animation. The client
   does the burn and the knockback by itself but has no notion of dying from it,
   so the server closes the loop: touch lava, die, wake up at the spawn point.
   audio/pain.wav and audio/wscream.wav are two more sounds the exe never names
   for itself, which is what death by something was for. */
static void check_hazards(ENetPeer* peer, Player& pl, WorldData& w) {
    const float PW = 20.0f, PH = 30.0f;
    int x0 = (int)(pl.x / 32.0f), x1 = (int)((pl.x + PW - 1) / 32.0f);
    int y0 = (int)(pl.y / 32.0f), y1 = (int)((pl.y + PH - 1) / 32.0f);
    for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
        if (!w.inside(x, y)) continue;
        const ItemDef* d = def(w.at(x, y).fg);
        if (!d || d->material != MAT_LAVA) continue;
        broadcast_call(pl.world, { Variant::str("OnKilled") }, pl.netID);
        call_fn(peer, { Variant::str("OnSetPos"),
                        Variant::vec2((float)w.spawnX, (float)w.spawnY) },
                pl.netID);
        pl.x = (float)w.spawnX; pl.y = (float)w.spawnY;
        console(peer, "`4You burned to death.``");
        printf("  ** %s died in lava at %d,%d\n", pl.name.c_str(), x, y);
        return;
    }
}

/* The client asks; the server confirms. 0x431bb0 runs when the avatar touches a
   dropped object: it plays audio/object_collect.wav at the object, raises state
   flag 0x4000, stamps obj+0x18 so it only ever asks once, and sends
   **packetType 11** carrying obj+0x10 -- the object id (builder 0x431c35).
   Nothing happens to the object until the server answers with a packetType 14.

   The server used to watch positions on every state packet and collect things
   by itself. That worked, but only by accident: the client was already asking
   and being ignored. This is the real handshake. */
static void handle_pickup_request(Player& pl, WorldData& w, uint32_t objectId) {
    for (size_t i = 0; i < w.objects.size(); ++i) {
        if (w.objects[i].id != objectId) continue;
        collect_object(pl, w, i);
        return;
    }
    printf("  -- pickup refused: no object %u in '%s'\n",
           objectId, w.name.c_str());
}

/* A punch. The client already knows how to draw the crack and play the
   sound; the server owns whether the tile actually breaks. */
/* OnPlayPositioned(string audioFile), handler 0x447300: it copies the string
   argument and then plays it at the avatar's own position (fld [esi+4]). This
   is the only way the server can make a noise, and it is how the 25 sounds
   shipped in audio/ that the exe never names for itself get used at all --
   tile_removed, punch_locked, use_lock, door_open, door_shut, punch_organic,
   pain and the rest. The client plays punch/wood_break/tile_created/
   cant_place_tile by itself; those are not ours to send. */
static void play_at(const std::string& world, int netID, const char* file) {
    broadcast_call(world, { Variant::str("OnPlayPositioned"),
                            Variant::str(file) }, netID);
}

/* ==================================================================== */
/*  Locks                                                                */
/* ==================================================================== */
/* packetType 15, as decoded above. The area is a list of tile indices, so the
   SIZE is policy, not protocol -- BUILDO_LOCK_SIZE, default a 10x10 square
   centred on the lock. */
static void send_lock(const std::string& world, const WorldLock& lk, int itemId) {
    std::vector<uint8_t> b;
    for (size_t i = 0; i < lk.tiles.size(); ++i) w16(b, lk.tiles[i]);
    GameUpdatePacket g{};
    g.packetType = 15;
    g.netID      = lk.owner;                  /* -> extra->owner (0x43ee10) */
    g.netID2     = (int32_t)lk.tiles.size();  /* how many indices follow    */
    g.intData    = itemId;
    g.intX = lk.x; g.intY = lk.y;
    broadcast_gup(world, g, NULL, b.data(), b.size());
}

static void add_lock(WorldData& w, int x, int y, int owner, int itemId) {
    int size = 10;
    if (const char* e = getenv("BUILDO_LOCK_SIZE")) size = atoi(e);
    if (size < 1) size = 1;
    WorldLock lk;
    lk.x = x; lk.y = y; lk.owner = owner;
    int half = size / 2;
    for (int ty = y - half; ty < y - half + size; ++ty)
        for (int tx = x - half; tx < x - half + size; ++tx) {
            if (!w.inside(tx, ty)) continue;
            lk.tiles.push_back((uint16_t)(ty * w.w + tx));
        }
    w.locks.push_back(lk);
    send_lock(w.name, w.locks.back(), itemId);
    printf("  ** lock at %d,%d by netID %d covers %zu tiles\n",
           x, y, owner, lk.tiles.size());
}

/* Who owns the tile you are trying to touch: 0 if nobody. */
static int lock_owner_of(WorldData& w, int x, int y) {
    uint16_t idx = (uint16_t)(y * w.w + x);
    for (size_t i = 0; i < w.locks.size(); ++i) {
        const WorldLock& lk = w.locks[i];
        if (lk.x == x && lk.y == y) return lk.owner;
        for (size_t k = 0; k < lk.tiles.size(); ++k)
            if (lk.tiles[k] == idx) return lk.owner;
    }
    return 0;
}

static bool locked_against(ENetPeer* peer, Player& pl, WorldData& w,
                          int x, int y) {
    int owner = lock_owner_of(w, x, y);
    if (!owner || owner == pl.netID) return false;
    play_at(w.name, pl.netID, "audio/punch_locked.wav");
    console(peer, "`4That area is locked.``");
    return true;
}

/* packetType 13 takes items out of the client's own inventory: the handler at
   0x433e26 reads count1 (+0x02) and intData (+0x14) and calls
   PlayerItems::Remove (0x43d800) with them, then rebuilds the item bar.

   This is needed because the client does NOT deduct on placement -- measured:
   placing Grass, a Painting and a Toilet left all three counts untouched at 5
   while the server had taken one of each, so the two sides drifted apart until
   the server started refusing placements for items the client still displayed. */
static void send_take_item(const std::string& world, int itemId, int count) {
    GameUpdatePacket g{};
    g.packetType = 13;
    g.count1     = (uint8_t)count;
    g.intData    = itemId;
    broadcast_gup(world, g);
}

/* packetType 5 is a whole tile: the handler at 0x433f28 takes intX/intY, looks
   the tile up, and hands the packet's EXTENDED DATA straight to Tile::Serialize
   (0x43e850) before re-rendering it (0x441050). So the payload is one tile in
   exactly the world-blob shape -- fg, bg, a word, flags, then a TileExtra --
   and this is the only way to push a tile's EXTRA at someone. packetType 3
   carries an item id and nothing else, which is why a label edited with the
   wrench used to sit there looking unchanged until the world was reloaded. */
static void send_tile_update(const std::string& world, int x, int y, Tile& t) {
    std::vector<uint8_t> b;
    w16(b, t.fg);
    w16(b, t.bg);
    w16(b, 0);
    w16(b, (uint16_t)(t.flags & 0x20));
    const ItemDef* d = def(t.fg);
    int want = d ? extra_type_for_material(d->material) : 0;
    if (want) {
        if (t.extra.type != want) { t.extra = TileExtra(); t.extra.type = (uint8_t)want; }
        write_extra(b, t.extra);
    }
    GameUpdatePacket g{};
    g.packetType = 5;
    g.intX = x; g.intY = y;
    broadcast_gup(world, g, NULL, b.data(), b.size());
}

/* ==================================================================== */
/*  Doors                                                                */
/* ==================================================================== */
static void leave_world(ENetPeer* peer, Player& pl);
static void join_world(ENetPeer* peer, Player& pl, const std::string& wname);
static const char* world_list();

/* Clicking the tile you are standing in, with the fist, when that tile has a
   type-1 TileExtra, makes the client run 0x4493b0 -- and what comes out on the
   wire is packetType 7 with the tile in intX/intY. Measured, standing in the
   main door:
       [gs] gup type=7 netID=0 flags=0x0 len=0 tile=50,29 iData=0
   The client's own receive jump table sends 7 to the default case, so it never
   accepts one; 7 only ever travels client -> server. That is the door.

   A type-1 extra carries one string. Ours is the door's label, and the main
   door's label is EXIT, which is also what the client's own
   "action|quit_to_exit" string is about: the main door takes you out to the
   world list. A label that names another world is a way into it. */
static void handle_door(ENetPeer* peer, Player& pl, WorldData& w, int x, int y) {
    if (!w.inside(x, y)) return;
    Tile& t = w.at(x, y);
    const ItemDef* d = def(t.fg);
    if (!d || (d->material != MAT_DOOR && d->material != MAT_USER_DOOR)) return;

    std::string dest = t.extra.s1;
    play_at(pl.world, pl.netID, "audio/door_open.wav");
    printf("  ** %s used the %s at %d,%d (label '%s')\n", pl.name.c_str(),
           d->name.c_str(), x, y, dest.c_str());

    /* EXIT, or no label at all, means out to the world list. */
    if (dest.empty() || dest == "EXIT" || dest == w.name) {
        play_at(pl.world, pl.netID, "audio/door_shut.wav");
        leave_world(peer, pl);
        call_fn(peer, { Variant::str("OnRequestWorldSelectMenu"),
                       Variant::str(world_list()) });
        return;
    }
    /* Anything else is a world name: go there. */
    leave_world(peer, pl);
    join_world(peer, pl, dest);
}

/* ==================================================================== */
/*  Growing things                                                       */
/* ==================================================================== */
/* packetType 12 is the tree-state packet, and World::ApplyPacket handles it at
   0x43f32b:

     tile = GetTile(intX, intY)
     if (!Tile::IsPlanted(tile))            -> returns 0 and the client prints
                                               "Error handling tree state change"
     if (netID2 == -1)  clear the tile      (0x43e6d0)
     else               stage = (uint8)intData; extra->SetStage(stage)
                        and count1 == 1 additionally ...  (0x43f37d)

   This matters because the CLIENT DOES NOT GROW ANYTHING BY ITSELF. A tile
   planted by packetType 3 gets a TileExtra with stage 0, and stage 0 draws
   nothing at all -- measured: a seed left sitting for minutes with a 15-second
   bloom stayed invisible, and the same tile with stage 3 in the world blob drew
   a full tree out of tree_trunks/tree_greens. The age in the extra only feeds
   the "<time> to harvest" label; the STAGE is what picks the sprite, and the
   server owns it. */
enum { TREE_STAGES = 6 };

static void send_tree_state(const std::string& world, int x, int y, int stage) {
    GameUpdatePacket g{};
    g.packetType = 12;
    g.intX = x; g.intY = y;
    g.intData = stage;
    broadcast_gup(world, g);
}

/* How grown a plant is, 1 (just in the ground) to TREE_STAGES (ripe). */
static int tree_stage_for(const Tile& t, const ItemDef* d) {
    if (!d || d->bloomSecs <= 0 || !t.plantedAt) return TREE_STAGES;
    long age = (long)(time(NULL) - t.plantedAt);
    if (age >= d->bloomSecs) return TREE_STAGES;
    if (age < 0) age = 0;
    int st = 1 + (int)((long long)age * (TREE_STAGES - 1) / d->bloomSecs);
    return st < 1 ? 1 : (st > TREE_STAGES ? TREE_STAGES : st);
}

/* Walked every couple of seconds for worlds that have someone in them. */
static void grow_plants() {
    for (std::map<std::string, WorldData>::iterator it = g_worlds.begin();
         it != g_worlds.end(); ++it) {
        WorldData& w = it->second;
        bool watched = false;
        for (std::map<ENetPeer*, Player>::iterator p = g_players.begin();
             p != g_players.end(); ++p)
            if (p->second.inWorld && p->second.world == w.name) { watched = true; break; }
        if (!watched) continue;
        for (int y = 0; y < w.h; ++y) for (int x = 0; x < w.w; ++x) {
            Tile& t = w.at(x, y);
            if (!t.fg || t.extra.type != 4) continue;
            const ItemDef* d = def(t.fg);
            if (!d || d->material != MAT_SEED) continue;
            int st = tree_stage_for(t, d);
            if (st == (int)t.extra.stage) continue;
            t.extra.stage = (uint8_t)st;
            send_tree_state(w.name, x, y, st);
        }
    }
}

/* Seed -> tree. The client grows it entirely on its own: the tile keeps a
   plant timestamp at +0x60 (stamped as the TileExtra is read) plus the age the
   extra carried at +0x64, and Tile::GetPlantAge (0x43ed30) is
       (now_ms - tile+0x60)/1000 + tile+0x64
   which the label code (0x43eaa8) subtracts from ItemInfo+0x7c, the seconds to
   bloom. So the client already prints "1m 0s to harvest" and then
   "(Punch to harvest)" without the server saying anything. All the server has
   to own is what a ripe tree gives you. */
static bool tree_is_ripe(const Tile& t, const ItemDef* d) {
    if (!d || d->bloomSecs <= 0) return true;      /* 0 = ripe on arrival */
    if (!t.plantedAt) return true;
    return (long)(time(NULL) - t.plantedAt) >= (long)d->bloomSecs;
}

/* What a tree yields. Seeds in item_definitions.txt sit at blockId + 1 --
   seed 3 next to Dirt 2, seed 5 next to Lava 4, seed 15 next to Cave Wall 14 --
   so a tree drops the block it grew from. The COUNT is not pinned: every
   setup_seed line ships max_fruit|0, so the 2-plus-a-seed here is ours. */
static void harvest_tree(ENetPeer* peer, Player& pl, WorldData& w, int x, int y,
                         uint16_t seedId) {
    play_at(w.name, pl.netID, "audio/tree_harvest.wav");
    Tile& t = w.at(x, y);
    t.fg = 0; t.flags = 0; t.extra = TileExtra();
    t.damage = 0; t.healAt = 0; t.plantedAt = 0;
    send_set_tile(w.name, x, y, IT_FIST, pl.netID);
    /* Seeds sit at blockId + 1 in item_definitions.txt and the client proves
       the pairing by hanging that very block on the ripe tree as fruit, so the
       fruit itself is restored. The COUNT is not pinned -- every setup_seed
       line ships max_fruit|0 -- so it is one unless BUILDO_FRUIT says
       otherwise, and the tree does not hand back a spare seed: that was
       invented. */
    int fruit = seedId - 1;
    int n = getenv("BUILDO_FRUIT") ? atoi(getenv("BUILDO_FRUIT")) : 1;
    if (def(fruit) && n > 0) drop_from_tile(w, fruit, n, x, y);
    console(peer, std::string("Harvested a `w") + item_name(seedId) + "`` tree.");
    printf("  ** %s harvested a %s tree at %d,%d\n",
           pl.name.c_str(), item_name(seedId), x, y);
}

static void handle_punch(ENetPeer* peer, Player& pl, WorldData& w, int x, int y) {
    Tile& t = w.at(x, y);
    uint16_t target = t.fg ? t.fg : t.bg;      // same choice as Tile::Damage
    if (!target) return;
    const ItemDef* d = def(target);
    if (!d) return;

    time_t now = time(NULL);
    if (t.healAt && now >= t.healAt) t.damage = 0;

    /* A planted seed is a tree: punching a ripe one harvests it instead of
       chipping away at it, which is what "(Punch to harvest)" promises. */
    if (t.fg && d->material == MAT_SEED) {
        if (tree_is_ripe(t, d)) { harvest_tree(peer, pl, w, x, y, t.fg); return; }
        play_at(w.name, pl.netID, "audio/punch_organic.wav");
        long left = (long)d->bloomSecs - (long)(now - t.plantedAt);
        char msg[128];
        snprintf(msg, sizeof msg, "`oStill growing -- `w%ld`` second%s to go.",
                 left, left == 1 ? "" : "s");
        console(peer, msg);
        return;
    }

    if (target == IT_BEDROCK) {                 // the world floor stays put
        send_damage_tile(w.name, x, y, 1, pl.netID);
        return;
    }
    if (locked_against(peer, pl, w, x, y)) return;
    uint8_t hp = d->hp ? d->hp : 1;
    t.damage = (uint8_t)(t.damage + 1);
    t.healAt = now + (d->healSecs ? d->healSecs : 8);

    if (t.damage < hp) {
        send_damage_tile(w.name, x, y, 1, pl.netID);
        return;
    }

    /* broken: clear the same layer the client will clear, and hand it over */
    t.damage = 0; t.healAt = 0;
    if (t.fg) { t.fg = 0; t.flags = 0; t.extra = TileExtra(); }
    else      { t.bg = 0; t.flags = 0; }
    send_set_tile(w.name, x, y, IT_FIST, pl.netID);
    play_at(w.name, pl.netID, "audio/tile_removed.wav");
    /* The block does not teleport into your pockets any more: it lands in the
       world as an object and you walk over it. Gems come out of the same
       break. */
    drop_from_tile(w, target, 1, x, y);
    int g = gems_for_block(d);
    if (g) drop_from_tile(w, IT_GEMS, g, x, y);
    printf("  ** %s broke %s at %d,%d\n", pl.name.c_str(), item_name(target), x, y);
}

/* Why a placement was refused. Silence here used to make it impossible to
   tell a client-side refusal (the client plays audio/cant_place_tile.wav and
   sends nothing) from a server-side one, so every reject says so. */
static void place_refused(int x, int y, const char* why, const ItemDef* d) {
    printf("  -- place refused at %d,%d: %s (%s)\n", x, y, why,
           d ? d->name.c_str() : "?");
}

static void handle_place(ENetPeer* peer, Player& pl, WorldData& w, int x, int y,
                         int itemId, bool flipped) {
    const ItemDef* d = def(itemId);
    if (!d) { printf("  -- place refused: no such item %d\n", itemId); return; }
    Tile& t = w.at(x, y);

    if (d->material == MAT_CLOTHES) {           // worn, not placed
        place_refused(x, y, "clothing is worn, not placed", d);
        return;
    }
    if (!inv_has(pl, (uint16_t)itemId)) {
        console(peer, "`4You don't have that.``");
        send_inventory(peer, pl);
        place_refused(x, y, "not in inventory", d);
        return;
    }
    /* Do not let anyone brick themselves in. The client has a guard of its own
       but it only protects ONE tile -- 0x446d00 takes the centre of the
       avatar's box and 0x436d81 compares the clicked tile against that -- and
       the box is 30px tall in a 32px row, so in mid-air it straddles two rows
       and the row your head is in is unprotected. Check the whole box here,
       for every player in the world, and only for tiles that actually block. */
    if (locked_against(peer, pl, w, x, y)) {
        place_refused(x, y, "inside someone else's lock", d);
        return;
    }

    if (d->material == MAT_BACKGROUND) {
        if (t.bg) { place_refused(x, y, "background occupied", d); return; }
        t.bg = (uint16_t)itemId;
    } else if (d->material == MAT_SEED) {
        if (t.fg) { place_refused(x, y, "seed needs an empty tile", d); return; }
        t.fg = (uint16_t)itemId;
        t.extra = TileExtra(); t.extra.type = 4;
        t.plantedAt = time(NULL);
        t.extra.stage = (uint8_t)tree_stage_for(t, d);
    } else {
        if (t.fg) { place_refused(x, y, "foreground occupied", d); return; }
        t.fg = (uint16_t)itemId;
        int want = extra_type_for_material(d->material);
        t.extra = TileExtra();
        if (want) {
            t.extra.type = (uint8_t)want;
            if (want == 3) t.extra.u1 = (uint32_t)pl.netID;   // lock owner
        }
    }
    t.flags = flipped ? 0x20 : 0;
    t.damage = 0; t.healAt = 0;

    /* The client deducts one from its own inventory when it sees this packet
       with itself as the actor (0x433ca6 -> 0x43d800), so the server makes
       the same deduction and does not resend the inventory. */
    inv_take(pl, (uint16_t)itemId, 1);
    send_set_tile(w.name, x, y, itemId, pl.netID, flipped);
    send_take_item(w.name, itemId, 1);
    /* A lock claims the ground around it the moment it goes down. */
    if (d->material == MAT_LOCK) {
        t.extra.u1 = (uint32_t)pl.netID;
        add_lock(w, x, y, pl.netID, itemId);
    }
    /* SetTile gives the new tile a TileExtra with stage 0, which draws
       nothing, so tell the client the stage straight away. */
    /* The tree renderer takes the fruit from the TILE's flags -- 0x4449aa is
       `mov al,[edi+4]; shr al,4` on the tile -- and SetTile fills flags
       0x08/0x10 from the packet's count1. Driving count1 from here changed
       nothing on screen though (0, 2 and 4 all drew the same sapling), and a
       second packetType 3 for the same tile only risks SetTile allocating a
       fresh extra and losing the growth stage. So the fruit count stays
       unpinned; what IS pinned is where it lives. */
    if (d->material == MAT_SEED)
        send_tree_state(w.name, x, y, t.extra.stage);
    printf("  ** %s placed %s at %d,%d\n", pl.name.c_str(), d->name.c_str(), x, y);
}

/* ==================================================================== */
/*  Dialogs                                                              */
/* ==================================================================== */
/* The client builds a dialog out of a newline-separated script; the parser at
   0x417e40 checks each line's token count and logs "Error with <cmd> parms"
   when one is short, which is how these were pinned:

     add_label|<size>|<text>|<align>                     >= 4  (0x41abbc)
     add_label_with_icon|<size>|<text>|<align>|<itemID>   >= 5  (0x41ad64)
     add_textbox|<text>|<align>                          >= 3  (0x41af4a)
     end_dialog|<name>|<cancelText>|<okText>             >= 4  (0x41b569)

   add_spacer, add_button, add_checkbox, add_text_input, add_player_picker,
   set_default_color and disable_resize are in the same vocabulary
   (0x4ca4a4..0x4ca5b0). The reply comes back as action|dialog_return. */
static void send_dialog(ENetPeer* p, const std::string& script) {
    call_fn(p, { Variant::str("OnDialogRequest"), Variant::str(script) });
}

/* What the client can actually be told about an item, in its own words. */
static std::string describe_material(int mat) {
    switch (mat) {
        case MAT_FIST:       return "a fist: punches tiles";
        case MAT_WRENCH:     return "a tool: cannot be placed, works on doors, locks and signs";
        case MAT_DOOR:       return "a door";
        case MAT_LOCK:       return "a lock";
        case MAT_SIGN:       return "a sign";
        case 5:              return "plays a sound when punched";
        case MAT_BOOMBOX:    return "punch it to switch it on; it animates and plays music";
        case MAT_USER_DOOR:  return "a door you can label";
        case MAT_LAVA:       return "lava: it burns and throws you back";
        case MAT_BACKGROUND: return "a background: goes behind other tiles";
        case MAT_SEED:       return "a seed: plant it on an empty tile";
        case MAT_CLOTHES:    return "clothing: click a tile with it held to wear it";
        default:             return "a plain block";
    }
}

/* action|info|itemID|N -- sent by the INFO button in the inventory panel
   (0x4ca868 "action|info", built at 0x41ee8b). */
static void handle_info(ENetPeer* peer, Player& pl, int itemId) {
    const ItemDef* d = def(itemId);
    if (!d || !d->valid) { console(peer, "`4No such item.``"); return; }
    int have = 0;
    for (size_t i = 0; i < pl.inv.size(); ++i)
        if (pl.inv[i].id == itemId) have = pl.inv[i].amount;
    char buf[1400];
    snprintf(buf, sizeof buf,
        "set_default_color|`o\n"
        "add_label_with_icon|big|`w%s``|left|%d|\n"
        "add_spacer|small|\n"
        "add_textbox|You have `w%d`` of these. Item id `w%d``.|left|\n"
        "add_textbox|It is %s.|left|\n"
        "add_textbox|Takes `w%d`` %s to break, heals after `w%d``s.|left|\n"
        "add_textbox|%s|left|\n"
        "add_spacer|small|\n"
        "end_dialog|iteminfo|Close||\n",
        d->name.c_str(), itemId, have, itemId,
        describe_material(d->material).c_str(),
        d->hp, d->hp == 1 ? "punch" : "punches", d->healSecs,
        d->collision == 1 ? "Solid: you cannot walk through it."
                          : "You can walk through it.");
    send_dialog(peer, buf);
    printf("  ** %s asked about %s (id %d)\n", pl.name.c_str(),
           d->name.c_str(), itemId);
}

/* The wrench. The client sends the ordinary packetType 3 with the wrench as
   the held item, but ONLY for a tile whose material is 2, 3 or 4 -- see the
   0x43e450 guard quoted at the eTileMaterial table. So this is reached
   exactly for a door, a lock or a sign. */
static void handle_wrench(ENetPeer* peer, Player& pl, WorldData& w, int x, int y) {
    Tile& t = w.at(x, y);
    const ItemDef* d = def(t.fg);
    if (!d || !t.fg) { console(peer, "`4Nothing to wrench there.``"); return; }
    char buf[1600];
    std::string extra;
    bool editable = false;
    if (d->material == MAT_LOCK) {
        char o[160];
        snprintf(o, sizeof o, "add_textbox|Owned by netID `w%u``.|left|\n",
                 t.extra.u1);
        extra = o;
    } else if (d->material == MAT_SIGN) {
        /* A type-2 extra is a string, a type-1 extra is a string plus a byte --
           either way there is exactly one string to edit, and the client has
           add_text_input and sends the result back as action|dialog_return. */
        extra = "add_text_input|label|Sign says|" + t.extra.s1 + "|100|\n";
        editable = true;
    } else {
        /* A door's one string is its label, and the main door's label is EXIT.
           Put a world name in here and the door leads there -- linking two
           doors is nothing more than this. */
        extra = "add_text_input|label|Goes to|" + t.extra.s1 + "|24|\n"
                "add_textbox|`oEXIT or empty means out to the world list.|left|\n";
        editable = true;
    }
    /* The dialog's name carries the tile back to us, and it must not contain a
       pipe: end_dialog is name|cancelText|okText, so pipes here become button
       labels. */
    char pos[64]; snprintf(pos, sizeof pos, "wrench_%d_%d", x, y);
    snprintf(buf, sizeof buf,
        "set_default_color|`o\n"
        "add_label_with_icon|big|`wWrench: %s``|left|%d|\n"
        "add_spacer|small|\n"
        "add_textbox|At `w%d, %d`` in world `w%s``.|left|\n"
        "%s"
        "add_spacer|small|\n"
        "end_dialog|%s|Close|%s|\n",
        d->name.c_str(), (int)t.fg, x, y, w.name.c_str(), extra.c_str(),
        pos, editable ? "Set" : "");
    send_dialog(peer, buf);
    printf("  ** %s wrenched %s at %d,%d\n", pl.name.c_str(),
           d->name.c_str(), x, y);
}

/* Wearing clothes. There is no wear button anywhere in the client: the
   inventory panel has exactly DROP, STORE and INFO (0x4209f3..0x420a82) and a
   click on a cell only selects. What the client DOES do is send the ordinary
   packetType 3 tile action with the clothing item as the held item -- material
   14 is neither 0 (fist) nor 1 (not placeable), so 0x436efa lets it through --
   and then waits to be told what it is wearing via OnSetClothing. Measured:
   selecting a Top Hat and clicking a tile arrives here as item 66. */
static void handle_wear(ENetPeer* peer, Player& pl, int itemId) {
    const ItemDef* d = def(itemId);
    if (!d || d->material != MAT_CLOTHES) return;
    if (!inv_has(pl, (uint16_t)itemId)) {
        console(peer, "`4You don't have that.``");
        send_inventory(peer, pl);
        return;
    }
    bool worn = false;
    for (size_t i = 0; i < pl.inv.size(); ++i) {
        if (pl.inv[i].id != itemId) continue;
        pl.inv[i].flags ^= 1;                       // flag bit 0 == worn
        worn = (pl.inv[i].flags & 1) != 0;
    }
    /* One item per body part, or the client would have two hats fighting over
       the same slot in PlayerItems+6+part*2. */
    if (worn) {
        for (size_t i = 0; i < pl.inv.size(); ++i) {
            if (pl.inv[i].id == itemId) continue;
            const ItemDef* o = def(pl.inv[i].id);
            if (o && o->material == MAT_CLOTHES && o->bodyPart == d->bodyPart)
                pl.inv[i].flags &= (uint8_t)~1;
        }
    }
    refresh_clothing(pl);
    save_player(pl);
    send_inventory(peer, pl);
    send_clothing(peer, pl);
    for (std::map<ENetPeer*, Player>::iterator it = g_players.begin();
         it != g_players.end(); ++it)
        if (it->first != peer && it->second.inWorld && it->second.world == pl.world)
            send_clothing(it->first, pl);
    console(peer, std::string(worn ? "Wearing `w" : "Took off `w") + d->name + "``.");
    printf("  ** %s %s %s\n", pl.name.c_str(), worn ? "put on" : "took off",
           d->name.c_str());
}

/* ==================================================================== */
static const char* world_list() {
    const char* e = getenv("BUILDO_WORLDS");
    return e ? e : "START\nSECOND\nTHIRD";
}

static void leave_world(ENetPeer* peer, Player& pl) {
    if (!pl.inWorld) return;
    std::string old = pl.world;
    pl.inWorld = false;
    for (std::map<ENetPeer*, Player>::iterator it = g_players.begin();
         it != g_players.end(); ++it) {
        if (it->first == peer) continue;
        if (!it->second.inWorld || it->second.world != old) continue;
        char buf[64];
        snprintf(buf, sizeof buf, "netID|%d\n", pl.netID);
        call_fn(it->first, { Variant::str("OnRemove"), Variant::str(buf) });
    }
    printf("  ** %s left '%s'\n", pl.name.c_str(), old.c_str());
}

static void join_world(ENetPeer* peer, Player& pl, const std::string& wname) {
    leave_world(peer, pl);
    WorldData& w = get_world(wname);
    pl.world   = wname;
    pl.inWorld = true;
    pl.x = (float)w.spawnX; pl.y = (float)w.spawnY;

    std::vector<uint8_t> blob = serialize_world(w);
    GameUpdatePacket g{};
    g.packetType = 4;                  // map data
    g.netID      = -1;
    g.intData    = (int32_t)blob.size();
    send_gup(peer, g, blob.data(), blob.size());
    printf("  <- world '%s' %dx%d (%zu bytes)\n", wname.c_str(), w.w, w.h, blob.size());

    refresh_clothing(pl);
    send_inventory(peer, pl);
    call_fn(peer, { Variant::str("OnSpawn"), Variant::str(spawn_text(pl, w, true)) });
    /* OnSetClothing is addressed to a netObject, and the client only creates
       it a frame after it processes OnSpawn -- sending it in the same burst
       gets "Server wants to call OnSetClothing ... but it doesn't exist".
       Hold it until the client speaks again. */
    /* The lock area is not in the world blob, so tell the newcomer about each
       lock explicitly -- one packetType 15 apiece. */
    for (size_t i = 0; i < w.locks.size(); ++i) {
        const Tile& lt = w.at(w.locks[i].x, w.locks[i].y);
        send_lock(w.name, w.locks[i], lt.fg ? lt.fg : IT_LOCK);
    }
    pl.clothPending = true;
    /* The gem counter RenderBuxComponent draws lives at app+0x130; the pickup
       path adds to it directly, so this is only the starting total. */
    call_fn(peer, { Variant::str("OnSetBux"), Variant::i32(pl.gems) });
    /* BUILDO_TEST_ACTION="/dance" fires one OnAction at the player as soon as
       they are in the world. Synthetic keystrokes do not reach the client's
       chat box, so this is how the emote path gets tested end to end: a name
       the client knows plays the animation, and any other name makes it log
       "Unknown action: <name>" (the format string at 0x4cc354). */
    if (const char* ta = getenv("BUILDO_TEST_ACTION")) pl.pendingAction = ta;

    /* everyone already here, and tell them about us */
    for (std::map<ENetPeer*, Player>::iterator it = g_players.begin();
         it != g_players.end(); ++it) {
        if (it->first == peer) continue;
        if (!it->second.inWorld || it->second.world != wname) continue;
        call_fn(peer, { Variant::str("OnSpawn"),
                        Variant::str(spawn_text(it->second, w, false)) });
        send_clothing(peer, it->second);
        call_fn(it->first, { Variant::str("OnSpawn"),
                             Variant::str(spawn_text(pl, w, false)) });
        send_clothing(it->first, pl);
        call_fn(it->first, { Variant::str("OnConsoleMessage"),
                             Variant::str("`5" + pl.name + "`` entered.") });
    }
    console(peer, "`5World `w" + wname + "`` entered.");
}

/* ==================================================================== */
int main() {
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);
    g_verbose = getenv("BUILDO_VERBOSE") != NULL;

    if (enet_initialize() != 0) { fprintf(stderr,"enet init failed\n"); return 1; }
    ENetAddress addr; addr.host = ENET_HOST_ANY; addr.port = 17091;
    ENetHost* host = enet_host_create(&addr, 32, 2, 0, 0);
    if (!host) { fprintf(stderr,"bind udp/17091 failed\n"); return 1; }

    if (enet_host_compress_with_range_coder(host) != 0) {
        fprintf(stderr, "range coder init failed\n"); return 1;
    }
    host->checksum = enet_crc32;
    printf("[gs] range coder + crc32 checksum enabled\n");
    printf("[gs] Buildo server on udp/17091 (enet %d.%d.%d)\n",
           ENET_VERSION_MAJOR, ENET_VERSION_MINOR, ENET_VERSION_PATCH);
    fflush(stdout);

    /* Resolve items.dat independently of the working directory: $BUILDO_ITEMS,
       else next to this executable, else the cwd. Getting this wrong makes the
       server advertise hash 0, the client demand a refresh, and the user sit
       forever on "Logging on...". */
    std::string itemsPath;
    if (const char* e = getenv("BUILDO_ITEMS")) itemsPath = e;
    else {
        char buf[4096];
        bool haveExe = false;
#ifdef __APPLE__
        uint32_t sz = sizeof buf;
        haveExe = (_NSGetExecutablePath(buf, &sz) == 0);
#elif defined(__linux__)
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n >= 0) { buf[n] = '\0'; haveExe = true; }
#endif
        if (haveExe) {
            std::string exe(buf);
            size_t slash = exe.find_last_of('/');
            if (slash != std::string::npos) itemsPath = exe.substr(0, slash) + "/items.dat";
        }
        if (itemsPath.empty()) itemsPath = "items.dat";
    }
    if (load_items(itemsPath.c_str()) || load_items("items.dat")) {
        int named = 0;
        for (size_t i = 0; i < g_defs.size(); ++i)
            if (g_defs[i].valid && !g_defs[i].name.empty()) ++named;
        printf("[gs] items.dat: %zu bytes, %zu entries (%d named), hash %d / 0x%08x\n",
               g_items.size(), g_defs.size(), named, (int32_t)g_itemHash, g_itemHash);
    } else {
        printf("[gs] FATAL: could not load/parse items.dat (tried '%s').\n",
               itemsPath.c_str());
    }
    fflush(stdout);

    int nextNetID = 1;
    ENetEvent ev;
    time_t lastGrow = 0, lastSave = 0;
    while (!g_stop) {
        time_t nowTick = time(NULL);
        if (nowTick != lastGrow) { lastGrow = nowTick; grow_plants(); }
        if (nowTick - lastSave >= 5) { lastSave = nowTick; save_all_worlds(); }
        while (!g_stop && enet_host_service(host, &ev, 200) > 0) {
            switch (ev.type) {

            case ENET_EVENT_TYPE_CONNECT: {
                char ip[64]; enet_address_get_host_ip(&ev.peer->address, ip, sizeof ip);
                printf("\n[gs] CONNECT %s:%u\n", ip, ev.peer->address.port);
                Player p; p.netID = nextNetID++;
                g_players[ev.peer] = p;
                send_type_only(ev.peer, MSG_SERVER_HELLO);
                enet_host_flush(host);
                fflush(stdout);
                break;
            }

            case ENET_EVENT_TYPE_RECEIVE: {
                const uint8_t* d = ev.packet->data;
                size_t n = ev.packet->dataLength;
                int32_t type = (n >= 4) ? *(const int32_t*)d : -1;
                Player& pl = g_players[ev.peer];

                if ((type == MSG_GENERIC_TEXT || type == MSG_GAME_MESSAGE) && n > 4) {
                    std::string txt((const char*)d + 4, n - 4);
                    while (!txt.empty() && txt[txt.size()-1]=='\0') txt.erase(txt.size()-1);
                    if (g_verbose) printf("\n[gs] text >>>\n%s\n<<<\n", txt.c_str());

                    if (pl.clothPending && pl.inWorld &&
                        txt.find("join_request") == std::string::npos) {
                        pl.clothPending = false; send_clothing(ev.peer, pl);
                        if (!pl.pendingAction.empty()) {
                            call_fn(ev.peer, { Variant::str("OnAction"),
                                               Variant::str(pl.pendingAction) }, pl.netID);
                            printf("[gs] test OnAction '%s' -> netID %d\n",
                                   pl.pendingAction.c_str(), pl.netID);
                            pl.pendingAction.clear();
                        }
                    }
                    if (txt.find("requestedName|") != std::string::npos) {
                        pl.name = get_field(txt, "requestedName");
                        if (pl.name.empty()) pl.name = "Player";
                        give_starter_kit(pl);
                        refresh_clothing(pl);
                        load_player(pl);
                        printf("\n[gs] LOGIN '%s' (netID %d), items hash %d -> client\n",
                               pl.name.c_str(), pl.netID, (int32_t)g_itemHash);
                        console(ev.peer, "`2Local Buildo server`` connected.");
                        call_fn(ev.peer, { Variant::str("OnInitialLogonAccepted"),
                                           Variant::u32(g_itemHash) });
                    }
                    else if (txt.find("action|refresh_item_data") != std::string::npos) {
                        send_item_db(ev.peer);
                    }
                    else if (txt.find("action|enter_game") != std::string::npos) {
                        call_fn(ev.peer, { Variant::str("OnRequestWorldSelectMenu"),
                                           Variant::str(world_list()) });
                    }
                    else if (txt.find("action|join_request") != std::string::npos) {
                        std::string wn = get_field(txt, "name");
                        if (wn.empty()) wn = "START";
                        printf("\n[gs] JOIN '%s' by %s\n", wn.c_str(), pl.name.c_str());
                        join_world(ev.peer, pl, wn);
                    }
                    else if (txt.find("action|quit") != std::string::npos) {
                        leave_world(ev.peer, pl);
                        call_fn(ev.peer, { Variant::str("OnRequestWorldSelectMenu"),
                                           Variant::str(world_list()) });
                    }
                    else if (txt.find("action|respawn") != std::string::npos) {
                        if (pl.inWorld) {
                            WorldData& w = get_world(pl.world);
                            call_fn(ev.peer, { Variant::str("OnSetPos"),
                                               Variant::vec2((float)w.spawnX,
                                                             (float)w.spawnY) },
                                    pl.netID);
                        }
                    }
                    else if (txt.find("action|input") != std::string::npos) {
                        std::string msg = get_field(txt, "text");
                        /* The client knows exactly two chat actions, and it
                           does NOT parse them itself: NetAvatar registers a
                           handler for the server call "OnAction" (name at
                           0x4cc454, handler 0x4471f0) which compares the
                           argument to "/dance" (0x4cc370 -> animation 0x44a500)
                           and "/wave" (0x4cc368 -> 0x44a4e0) and logs
                           "Unknown action: %s" for anything else. So the
                           server is what turns typed text into an emote, and
                           these two are the only ones this build can play. */
                        if (!msg.empty() && msg[0] == '/' && pl.inWorld) {
                            if (msg == "/dance" || msg == "/wave") {
                                broadcast_call(pl.world,
                                    { Variant::str("OnAction"), Variant::str(msg) },
                                    pl.netID);
                                printf("  ** %s %s\n", pl.name.c_str(), msg.c_str());
                            } else {
                                console(ev.peer, "`4Unknown command.`` This build "
                                                 "only knows `w/dance`` and `w/wave``.");
                                printf("  ** %s tried %s\n", pl.name.c_str(), msg.c_str());
                            }
                            fflush(stdout);
                            enet_packet_destroy(ev.packet);
                            break;
                        }
                        if (!msg.empty() && pl.inWorld) {
                            printf("  ** <%s> %s\n", pl.name.c_str(), msg.c_str());
                            broadcast_call(pl.world,
                                { Variant::str("OnConsoleMessage"),
                                  Variant::str("`w" + pl.name + "`` says, `#\"" +
                                               msg + "\"``") });
                            broadcast_call(pl.world,
                                { Variant::str("OnTalkBubble"),
                                  Variant::i32(pl.netID), Variant::str(msg) });
                        }
                    }
                    else if (txt.find("action|setSkin") != std::string::npos) {
                        std::string c = get_field(txt, "color");
                        if (!c.empty()) {
                            pl.skin = (uint32_t)strtoul(c.c_str(), NULL, 10);
                            save_player(pl);
                            printf("  ** %s skin 0x%08x\n", pl.name.c_str(), pl.skin);
                            if (pl.inWorld) {
                                send_clothing(ev.peer, pl);
                                for (std::map<ENetPeer*, Player>::iterator it2 = g_players.begin();
                                     it2 != g_players.end(); ++it2)
                                    if (it2->first != ev.peer && it2->second.inWorld &&
                                        it2->second.world == pl.world)
                                        send_clothing(it2->first, pl);
                            }
                        }
                    }
                    else if (txt.find("action|info") != std::string::npos) {
                        int id = atoi(get_field(txt, "itemID").c_str());
                        if (pl.inWorld) handle_info(ev.peer, pl, id);
                    }
                    else if (txt.find("action|dialog_return") != std::string::npos) {
                        printf("  ** dialog_return from %s:\n%s\n",
                               pl.name.c_str(), txt.c_str());
                        /* The wrench dialog names itself "wrench|X|Y" so the
                           reply carries the tile it belongs to. */
                        std::string dn = get_field(txt, "dialog_name");
                        if (dn.compare(0, 7, "wrench_") == 0 && pl.inWorld) {
                            int wx = 0, wy = 0;
                            if (sscanf(dn.c_str() + 7, "%d_%d", &wx, &wy) == 2) {
                                WorldData& w = get_world(pl.world);
                                if (w.inside(wx, wy)) {
                                    Tile& t = w.at(wx, wy);
                                    std::string v = get_field(txt, "label");
                                    t.extra.s1 = v;
                                    /* packetType 5 pushes the whole tile, extra
                                       included, so the new text shows at once. */
                                    send_tile_update(pl.world, wx, wy, t);
                                    console(ev.peer, std::string("Set to `w") +
                                            (v.empty() ? "(nothing)" : v) + "``.");
                                    printf("  ** %s set %d,%d label to '%s'\n",
                                           pl.name.c_str(), wx, wy, v.c_str());
                                }
                            }
                        }
                    }
                    else if (txt.find("action|drop") != std::string::npos) {
                        int id = atoi(get_field(txt, "itemID").c_str());
                        if (id > 0 && inv_take(pl, (uint16_t)id, 1)) {
                            send_inventory(ev.peer, pl);
                            console(ev.peer, std::string("Dropped a ") + item_name(id) + ".");
                        }
                    }
                    else if (g_verbose) {
                        printf("  (unhandled verb)\n");
                    }
                }
                else if (type == MSG_GAME_PACKET && n >= 4 + sizeof(GameUpdatePacket)) {
                    const GameUpdatePacket* g = (const GameUpdatePacket*)(d + 4);

                    if (g->packetType == 0) {
                        /* Avatar state. The client only sends these when its
                           input changes; relay them so other players move. */
                        ++pl.stateCount;
                        pl.x = g->vecX; pl.y = g->vecY;
                        if (pl.inWorld)
                            check_hazards(ev.peer, pl, get_world(pl.world));
                        if (pl.clothPending) { pl.clothPending = false; send_clothing(ev.peer, pl); }
                        if (!pl.pendingAction.empty()) {
                            call_fn(ev.peer, { Variant::str("OnAction"),
                                               Variant::str(pl.pendingAction) }, pl.netID);
                            printf("[gs] test OnAction '%s' -> netID %d\n",
                                   pl.pendingAction.c_str(), pl.netID);
                            pl.pendingAction.clear();
                        }
                        if (getenv("BUILDO_DUMP_STATE"))
                            printf("      [state#%d] pos=%.1f,%.1f vel=%.1f,%.1f "
                                   "flags=0x%x tile=%d,%d iData=%d\n",
                                   pl.stateCount, g->vecX, g->vecY, g->vec2X, g->vec2Y,
                                   g->flags, g->intX, g->intY, g->intData);
                        if (pl.inWorld) {
                            GameUpdatePacket out = *g;
                            out.netID = pl.netID;
                            broadcast_gup(pl.world, out, ev.peer);
                        }
                        fflush(stdout);
                        enet_packet_destroy(ev.packet);
                        break;
                    }

                    if (g->packetType == 11 && pl.inWorld) {
                        /* "I touched object N" -- see handle_pickup_request. */
                        handle_pickup_request(pl, get_world(pl.world),
                                              (uint32_t)g->intData);
                        fflush(stdout);
                        enet_packet_destroy(ev.packet);
                        break;
                    }

                    if (g->packetType == 10 && pl.inWorld) {
                        /* Item activate, built by 0x430cf0: nothing but the
                           item id in intData. The client sends it from
                           0x43615a, which is reached only when you click an
                           inventory cell that is ALREADY the selected one and
                           the item's material is 14 -- i.e. clicking a garment
                           twice. That is this build's own wear gesture. */
                        handle_wear(ev.peer, pl, g->intData);
                        fflush(stdout);
                        enet_packet_destroy(ev.packet);
                        break;
                    }

                    if (g->packetType == 7 && pl.inWorld) {
                        /* Tile activate -- the client only ever sends this for
                           the door it is standing in (0x4493b0). */
                        WorldData& w = get_world(pl.world);
                        handle_door(ev.peer, pl, w, g->intX, g->intY);
                        fflush(stdout);
                        enet_packet_destroy(ev.packet);
                        break;
                    }

                    if (g->packetType == 3 && pl.inWorld) {
                        /* A tile action: the client sends the tile it aimed at
                           plus the item it is holding. The Fist (material 0)
                           means punch, anything else means place. */
                        WorldData& w = get_world(pl.world);
                        int x = g->intX, y = g->intY;
                        if (!w.inside(x, y)) { enet_packet_destroy(ev.packet); break; }
                        const ItemDef* held = def(g->intData);
                        if (!held) { enet_packet_destroy(ev.packet); break; }
                        /* With BUILDO_TEST_ACTION set, re-arm it on every tile
                           click so the emote can be caught on a screenshot
                           taken right after -- fired once at world entry it is
                           always over before a frame can be grabbed. */
                        if (const char* ta = getenv("BUILDO_TEST_ACTION")) {
                            call_fn(ev.peer, { Variant::str("OnAction"),
                                               Variant::str(ta) }, pl.netID);
                            printf("[gs] test OnAction '%s'\n", ta);
                        }
                        if (held->material == MAT_FIST)
                            handle_punch(ev.peer, pl, w, x, y);
                        else if (held->material == MAT_CLOTHES)
                            handle_wear(ev.peer, pl, g->intData);
                        else if (held->material == MAT_WRENCH)
                            handle_wrench(ev.peer, pl, w, x, y);
                        else
                            handle_place(ev.peer, pl, w, x, y, g->intData,
                                         (g->flags & 0x10) != 0);
                        fflush(stdout);
                        enet_packet_destroy(ev.packet);
                        break;
                    }

                    printf("\n[gs] gup type=%u netID=%d flags=0x%x len=%u "
                           "tile=%d,%d iData=%d\n",
                           g->packetType, g->netID, g->flags, g->dataLength,
                           g->intX, g->intY, g->intData);
                    if (g->flags & 0x08)
                        dump(d + 4 + sizeof(GameUpdatePacket),
                             n - 4 - sizeof(GameUpdatePacket));
                }
                else {
                    printf("\n[gs] RECV ch=%u len=%zu type=%d\n", ev.channelID, n, type);
                    dump(d, n);
                }
                fflush(stdout);
                enet_packet_destroy(ev.packet);
                break;
            }

            case ENET_EVENT_TYPE_DISCONNECT: {
                Player& pl = g_players[ev.peer];
                printf("\n[gs] DISCONNECT %s\n", pl.name.c_str());
                save_player(pl);
                leave_world(ev.peer, pl);
                g_players.erase(ev.peer);
                fflush(stdout);
                break;
            }
            default: break;
            }
        }
    }
    printf("[gs] saving worlds before shutdown\n");
    save_all_worlds();
    save_all_players();
    enet_host_destroy(host);
    enet_deinitialize();
    return 0;
}
