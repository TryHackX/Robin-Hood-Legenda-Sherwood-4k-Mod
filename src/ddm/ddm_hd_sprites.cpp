#include "ddm_hd_sprites.h"
#include "ddm.h"
#include "ddm_hd.h"
#include "../config.h"
#include "../logger.h"
#include "../timing.h"

#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace DDM::HD::Sprites {
namespace {
    constexpr uintptr_t kAddrDecode        = 0x00504CD0; // bank->Decode(lock*, frame, tone), ret 0xC
    constexpr uintptr_t kAddrDecodeMaskedA = 0x00504EB0; // (lock*, frame, x), ret 0xC
    constexpr uintptr_t kAddrDecodeMaskedB = 0x00504F70; // (lock*, frame, x, y), ret 0x10
    constexpr uintptr_t kAddrShadowColor   = 0x004D2340; // engine->Color(index), ret 4
    constexpr uintptr_t kAddrEngine2       = 0x006C0A88;
    constexpr uintptr_t kAddrSpanMask      = 0x006C11C8;
    constexpr uint16_t  kKey = 0x07C0, kMarker = 0x001F;
    constexpr size_t    kCacheBudget = 128u * 1024 * 1024;

    // The bank file (RobinHood.dic) holds this many frames. Higher numbers are frames the game
    // makes at run time (0x00505980: a picture of the interface resources, cropped to its
    // content and stored as 16-bit row RLE): the portrait above the selected hero, "Z" above a
    // sleeper, markers ... Their numbers depend on the order of loading, so such a frame is
    // identified by its content - FNV-1a 64 over width, height and the row data - and its
    // upscaled version comes from hd\sprites\_icons (tools\hd\upscale_icons.py).
    constexpr uint32_t  kBankFileFrames = 404855;

    // What the bank's decode function gets as its destination (a locked surface).
    struct LockInfo {
        uint32_t unknown0;
        uint16_t w, h;      // +4, +6
        int32_t  pitch;     // +8
        uint16_t bpp;       // +0xC
        uint16_t pad;
        uint8_t* bits;      // +0x10
    };

    // bank frame table entry (20 bytes): +0 w, +2 h, +8 data, +0x10 flag, +0x12 palette (0xFFFF = lossless)
    struct BankFrame {
        uint16_t w, h;
        uint32_t size;
        void*    data;
        uint32_t offset;
        uint8_t  flag;
        uint8_t  pad;
        uint16_t palette;
    };

    struct Decoded {
        uint32_t frame = 0;
        uint32_t tone = 0;
        uint16_t w = 0, h = 0;       // size of the surface it was decoded into
        uint16_t fw = 0, fh = 0;     // size of the frame
        uint8_t  flag = 0;           // lossless frame whose marker pixels were replaced by the shadow colour
        bool     lossless = false;
        bool     masked = false;     // the game cut occluded parts out of the surface after decoding
        int8_t   checked = 0;        // 0 not yet, 1 the surface matches the HD frame, -1 it does not
        uint64_t content = 0;        // run-time frame: key of its content (0 = a frame of the bank file)
        uint32_t prev = 0;           // the frame the same sprite showed in the previous game tick (= frame: none / unknown)
        void*    sprite = nullptr;   // the sprite object that decoded it (nullptr: decoded by other code)
    };

    // ---- in-between frames (HDInBetween): hd\sprites\_between\between.idx / between.dat
    // (tools\hd\make_inbetweens.py). A sprite that went from frame A to frame B in this game tick
    // is drawn, in the pictures of the first part of the tick, with a generated frame half-way
    // between the two - the animation runs at twice the game's 25 steps per second.
    // Which sprite a decode belongs to: the sprite object's own method (0x005BBA00) calls the
    // bank's decoder; the frame each sprite object showed in the previous tick is remembered.
    constexpr uintptr_t kAddrSpriteDecode = 0x005BBA00;  // sprite->DecodeCurrentFrame(arg*), ret 4
    // Pictures drawn before this point of the tick show the in-between frame. A picture stays on
    // screen until the next one is drawn, so the switch happens on average half a picture period
    // after this point; the tick's own picture comes at ~0.04. Measured with draw traces at
    // 144 Hz: 0.6 -> 4 pictures in-between : 2 the frame, 0.5 -> 59 % : 41 %; 0.45 centres it.
    constexpr float     kBetweenPhase = 0.45f;
    using PFN_SpriteDecode = uint32_t(__fastcall*)(void* sprite, void* edx, void* arg);
    PFN_SpriteDecode o_SpriteDecode = nullptr;
    void* g_spriteNow = nullptr;     // the sprite object that is decoding its frame right now

    struct SpriteAnim {
        uint32_t cur = 0, prev = 0;
        uint32_t tick = 0;
    };
    std::unordered_map<void*, SpriteAnim> g_anim;
    uint32_t g_tick = 1;             // counts the game ticks' own frames

    // A store's data file, possibly in parts: a file of a GitHub release must be under 2 GB, so
    // the HD packs (tools\hd\..\make_hd_packs.py) cut a large data file into "<name>",
    // "<name>.2", "<name>.3"... The offsets of the index run over the parts as over one file.
    struct DataFile {
        static constexpr int kMaxParts = 8;
        HANDLE   part[kMaxParts] = {};
        uint64_t start[kMaxParts] = {};
        uint64_t size[kMaxParts] = {};
        int      count = 0;
        bool open() const { return count > 0; }
    };

    void CloseData(DataFile& d) {
        for (int i = 0; i < d.count; ++i) CloseHandle(d.part[i]);
        d = DataFile{};
    }

    bool OpenData(DataFile& d, const std::string& path) {
        CloseData(d);
        const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE; // a generator may be appending
        uint64_t total = 0;
        for (int i = 0; i < DataFile::kMaxParts; ++i) {
            const std::string name = i == 0 ? path : path + "." + std::to_string(i + 1);
            HANDLE h = CreateFileA(name.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) break;
            LARGE_INTEGER sz{};
            if (!GetFileSizeEx(h, &sz)) {
                CloseHandle(h);
                break;
            }
            d.part[d.count] = h;
            d.start[d.count] = total;
            d.size[d.count] = static_cast<uint64_t>(sz.QuadPart);
            total += d.size[d.count];
            ++d.count;
        }
        if (d.count > 1) Logger::Info("HD sprites: %s in %d parts (%.0f MB).", path.c_str(), d.count, static_cast<double>(total) / 1048576.0);
        return d.count > 0;
    }

    // `size` bytes from `offset` over the parts (a frame may cross from one part into the next)
    bool ReadAt(const DataFile& d, uint64_t offset, void* out, uint32_t size) {
        auto* dst = static_cast<uint8_t*>(out);
        for (int i = 0; i < d.count && size > 0; ++i) {
            const int last = d.count - 1;
            if (offset >= d.start[i] + d.size[i] && i < last) continue;
            const uint64_t inPart = offset - d.start[i];
            // the last part takes what lies beyond it too (a generator may be appending to it)
            const uint64_t avail = i < last ? d.size[i] - inPart : static_cast<uint64_t>(size);
            const DWORD n = static_cast<DWORD>((std::min)(static_cast<uint64_t>(size), avail));
            LARGE_INTEGER pos{};
            pos.QuadPart = static_cast<LONGLONG>(inPart);
            DWORD got = 0;
            if (!SetFilePointerEx(d.part[i], pos, nullptr, FILE_BEGIN) || !ReadFile(d.part[i], dst, n, &got, nullptr) || got != n) {
                return false;
            }
            dst += n;
            offset += n;
            size -= n;
        }
        return size == 0;
    }

#pragma pack(push, 1)
    struct BetweenEntry {            // 32 bytes, sorted by (a, b), a < b
        uint32_t a, b;
        uint64_t offset;
        uint32_t size;               // 0 = the generator rejected this pair
        uint16_t w, h;
        int16_t  dxA, dyA, dxB, dyB; // top-left corner of the in-between frame relative to that of frame A / B (HD pixels)
    };
#pragma pack(pop)
    std::vector<BetweenEntry> g_between;
    // Per entry how much the generator's two moved source frames disagreed (colour, 0..254 of 255;
    // 255 = not measured) - hd\sprites\_between\between.q, tools\hd\between_quality.py. The
    // generator accepted up to 32, its own notes call frames up to ~17 clean and 27-36 soft; in
    // the game the soft ones alternated with the sharp HD frames ("now HD, now not", ghosts in
    // fast moves - user report 2026-11-15). Only frames up to HDInBetweenQuality are shown.
    std::vector<uint8_t> g_betweenQ;
    DataFile g_betweenDat;
    bool   g_betweenTried = false;

    uint32_t Crc32(uint32_t crc, const uint8_t* p, size_t n) {     // as zlib.crc32
        static uint32_t table[256];
        static bool ready = false;
        if (!ready) {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            ready = true;
        }
        crc = ~crc;
        while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }
    std::atomic<bool>     g_betweenOn{ false };
    std::atomic<unsigned> g_betweenDrawn{ 0 };
    std::unordered_map<const uint8_t*, Decoded> g_decoded; // main thread only
    void* g_bank = nullptr;          // the bank object, as seen by the decoder

    // run-time frames: content key per frame number (checked against the data it was made from)
    struct RunTimeFrame {
        const void* data;
        uint16_t    w, h;
        uint32_t    bytes;           // length of the row data
        uint64_t    head;            // its first bytes, to notice a frame that was replaced
        uint64_t    key;             // 0 = not usable
    };
    std::unordered_map<uint32_t, RunTimeFrame> g_runTime;

    using PFN_Decode        = int(__fastcall*)(void* bank, void* edx, LockInfo* lock, uint32_t frame, uint32_t tone);
    using PFN_DecodeMaskedA = int(__fastcall*)(void* bank, void* edx, LockInfo* lock, uint32_t frame, uint32_t a);
    using PFN_DecodeMaskedB = int(__fastcall*)(void* bank, void* edx, LockInfo* lock, uint32_t frame, uint32_t a, uint32_t b);
    PFN_Decode        o_Decode = nullptr;
    PFN_DecodeMaskedA o_DecodeMaskedA = nullptr;
    PFN_DecodeMaskedB o_DecodeMaskedB = nullptr;

    // After decoding, the sprite object cuts out of its surface whatever is hidden behind walls,
    // trees ... (0x005BBA00, the part behind `cmp byte [ebp+0x10], 1`): these two routines write
    // the colour key into the hidden pixels. Both get the lock of the surface as 2nd argument.
    constexpr uintptr_t kAddrCutA = 0x005BBF20; // (rect*, lock*, pos*), ret 0xC
    constexpr uintptr_t kAddrCutB = 0x005BC3C0; // (data*, lock*, pos*, word), ret 0x10
    using PFN_CutA = uint32_t(__fastcall*)(void* sprite, void* edx, void* a, LockInfo* lock, void* pos);
    using PFN_CutB = uint32_t(__fastcall*)(void* sprite, void* edx, void* a, LockInfo* lock, void* pos, uint32_t b);
    PFN_CutA o_CutA = nullptr;
    PFN_CutB o_CutB = nullptr;

    // ---- stores: the one named like the background variant, and "_common" (frames made once
    // for all variants: characters and animations)
    struct IndexEntry {
        uint64_t offset;
        uint32_t size;
        uint16_t w, h;
    };
    struct Store {
        std::string             name;
        std::vector<IndexEntry> index;
        DataFile                dat;
        bool                    tried = false;
    };
    Store       g_stores[2];            // [0] variant, [1] common
    std::string g_variant = "off";

    // upscaled run-time frames, by content key: hd\sprites\_icons\icons.idx / icons.dat
    std::unordered_map<uint64_t, IndexEntry> g_icons;
    DataFile g_iconsDat;
    bool   g_iconsTried = false;

    // ---- frames ready to draw
    struct Ready {
        std::vector<uint16_t> px;
        int      w = 0, h = 0;
        uint32_t lastUse = 0;
    };
    std::unordered_map<uint64_t, Ready> g_ready;   // key: frame | shadow colour << 32
    size_t   g_readyBytes = 0;
    uint32_t g_useCounter = 0;
    std::atomic<unsigned> g_drawn{ 0 };
    bool g_cutKnown = false; // the routines that cut occluded parts out of a sprite are hooked

    // A frame is only used if its colours agree with what the game decoded (mean difference of
    // the sampled pixels per channel, of 255). Largest value among accepted frames: "stat" of the harness.
    constexpr double kMaxColourDiff = 36.0;
    double g_worstColourDiff = 0.0;

    // test harness: list of the frames the sprite surfaces hold right now
    std::mutex        g_dumpLock;
    std::string       g_dumpPath;
    std::atomic<bool> g_dumpWanted{ false };
    // test harness: all run-time frames of the bank, as the game holds them -> binary file
    std::string       g_runTimeDumpPath;
    std::atomic<bool> g_runTimeDumpWanted{ false };
    // test harness: what is magnified 2x WITHOUT an upscaled frame, counted over some game ticks
    struct Miss {
        uint32_t frame;      // 0xFFFFFFFF = a surface the decoder did not fill
        uint16_t w, h;
        uint8_t  path;       // 0 DirectDraw stretch, 1 the game's own routine, 2 DirectDraw mirrored
        uint8_t  why;        // 0 unknown surface, 1 no upscaled frame, 2 does not look like it
        uint64_t content;
        bool operator==(const Miss& o) const {
            return frame == o.frame && w == o.w && h == o.h && path == o.path && why == o.why && content == o.content;
        }
    };
    struct MissHash {
        size_t operator()(const Miss& m) const {
            return static_cast<size_t>(m.frame * 2654435761u ^ (m.w << 16) ^ m.h ^ (m.path << 28) ^ (m.why << 24) ^ static_cast<uint32_t>(m.content));
        }
    };
    std::unordered_map<Miss, unsigned, MissHash> g_miss;
    std::string       g_missPath;
    std::atomic<int>  g_missTicks{ 0 };   // > 0: collecting, this many game ticks left
    unsigned          g_missDrawn0 = 0;

    std::string GameDir() {
        char exe[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string path(exe);
        path.erase(path.find_last_of("\\/") + 1);
        return path;
    }

    void CloseStore(Store& st) {
        CloseData(st.dat);
        st.index.clear();
        st.tried = false;
    }

    void DropFrames() {
        g_ready.clear();
        g_readyBytes = 0;
        for (auto& kv : g_decoded) kv.second.checked = 0;
    }

    void OpenStore(Store& st) {
        if (st.tried) return;
        st.tried = true;
        const std::string dir = GameDir() + "hd\\sprites\\" + st.name + "\\";
        const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE; // the upscaler may be appending
        HANDLE idx = CreateFileA((dir + "sprites.idx").c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (idx == INVALID_HANDLE_VALUE) {
            Logger::Info("HD sprites: no store '%s' (hd\\sprites\\%s\\sprites.idx).", st.name.c_str(), st.name.c_str());
            return;
        }
        struct Header { char magic[4]; uint32_t version, count, reserved; } hdr{};
        DWORD got = 0;
        bool ok = ReadFile(idx, &hdr, sizeof(hdr), &got, nullptr) && got == sizeof(hdr) &&
                  std::memcmp(hdr.magic, "HDSI", 4) == 0 && hdr.version == 1 && hdr.count > 0 && hdr.count < 4000000;
        if (ok) {
            st.index.resize(hdr.count);
            const DWORD bytes = static_cast<DWORD>(hdr.count * sizeof(IndexEntry));
            ok = ReadFile(idx, st.index.data(), bytes, &got, nullptr) && got == bytes;
        }
        CloseHandle(idx);
        if (ok) {
            ok = OpenData(st.dat, dir + "sprites.dat");
        }
        if (!ok) {
            st.index.clear();
            Logger::Warn("HD sprites: store '%s' could not be read - ignored.", st.name.c_str());
            return;
        }
        size_t present = 0;
        for (const IndexEntry& e : st.index) present += e.size != 0;
        Logger::Info("HD sprites: store '%s' holds %zu upscaled frames (of %zu in the bank).", st.name.c_str(), present, st.index.size());
    }

    // Reads one frame of a data file; false if it is not there or damaged. Raw or row spans (see
    // upscale_sprites.py).
    bool ReadEntry(const DataFile& dat, const IndexEntry& e, int wantW, int wantH, std::vector<uint16_t>& px) {
        if (!dat.open()) return false;
        if (e.size == 0 || e.w != wantW || e.h != wantH || (e.size & 1) || e.size > 64u * 1024 * 1024) return false;
        const size_t pixels = static_cast<size_t>(e.w) * e.h;
        if (e.size == pixels * 2) {
            px.resize(pixels);
            return ReadAt(dat, e.offset, px.data(), e.size);
        }
        static std::vector<uint16_t> packed;
        packed.resize(e.size / 2);
        if (!ReadAt(dat, e.offset, packed.data(), e.size)) {
            return false;
        }
        px.assign(pixels, kKey);
        size_t p = 0;
        for (int y = 0; y < e.h; ++y) {
            if (p + 2 > packed.size()) return false;
            const uint32_t first = packed[p], count = packed[p + 1];
            p += 2;
            if (first + count > e.w || p + count > packed.size()) return false;
            if (count) std::memcpy(px.data() + static_cast<size_t>(y) * e.w + first, packed.data() + p, count * 2);
            p += count;
        }
        return p == packed.size();
    }

    bool ReadFrame(Store& st, uint32_t frame, int wantW, int wantH, std::vector<uint16_t>& px) {
        OpenStore(st);
        if (!st.dat.open() || frame >= st.index.size()) return false;
        return ReadEntry(st.dat, st.index[frame], wantW, wantH, px);
    }

    // ---- run-time frames (icons)
    void OpenIcons() {
        if (g_iconsTried) return;
        g_iconsTried = true;
        const std::string dir = GameDir() + "hd\\sprites\\_icons\\";
        const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        HANDLE idx = CreateFileA((dir + "icons.idx").c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (idx == INVALID_HANDLE_VALUE) {
            Logger::Info("HD sprites: no icon store (hd\\sprites\\_icons\\icons.idx) - icons and markers are magnified as before.");
            return;
        }
        struct Header { char magic[4]; uint32_t version, count, reserved; } hdr{};
        struct Entry { uint64_t key, offset; uint32_t size; uint16_t w, h; };
        DWORD got = 0;
        std::vector<Entry> entries;
        bool ok = ReadFile(idx, &hdr, sizeof(hdr), &got, nullptr) && got == sizeof(hdr) &&
                  std::memcmp(hdr.magic, "HDIC", 4) == 0 && hdr.version == 1 && hdr.count < 1000000;
        if (ok && hdr.count) {
            entries.resize(hdr.count);
            const DWORD bytes = static_cast<DWORD>(hdr.count * sizeof(Entry));
            ok = ReadFile(idx, entries.data(), bytes, &got, nullptr) && got == bytes;
        }
        CloseHandle(idx);
        if (ok) {
            ok = OpenData(g_iconsDat, dir + "icons.dat");
        }
        if (!ok) {
            Logger::Warn("HD sprites: the icon store could not be read - ignored.");
            return;
        }
        for (const Entry& e : entries) g_icons[e.key] = IndexEntry{ e.offset, e.size, e.w, e.h };
        Logger::Info("HD sprites: icon store holds %zu upscaled run-time frames (icons, markers).", g_icons.size());
    }

    bool ReadIcon(uint64_t key, int wantW, int wantH, std::vector<uint16_t>& px) {
        OpenIcons();
        const auto it = g_icons.find(key);
        return it != g_icons.end() && ReadEntry(g_iconsDat, it->second, wantW, wantH, px);
    }

    // Length in bytes of the bank's 16-bit row RLE for `h` rows of a frame `w` wide ("u16 first,
    // u16 last, last-first+1 pixels"; last = 0xFFFF: empty row). 0 = unreadable or not such data.
    uint32_t RowDataBytes(const void* data, int w, int h) {
        __try {
            const auto* p = static_cast<const uint16_t*>(data);
            for (int y = 0; y < h; ++y) {
                const uint32_t first = p[0], last = p[1];
                p += 2;
                if (last == 0xFFFF) continue;
                if (first > last || last >= static_cast<uint32_t>(w)) return 0;
                p += last - first + 1;
            }
            return static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(p) - static_cast<const uint8_t*>(data));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    uint64_t Fnv1a64(const uint8_t* p, size_t n, uint64_t h) {
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 0x100000001B3ull;
        }
        return h;
    }

    bool ReadTableEntry(void* bank, uint32_t index, BankFrame* out, uint32_t* count, uint32_t* bytes); // below

#pragma pack(push, 1)
    struct DumpRecord { uint32_t frame; uint16_t w, h; uint8_t flag, pad; uint16_t palette; uint32_t size, bytes; };
#pragma pack(pop)

    // A run-time frame that has no upscaled version yet is written, once, to
    //     hd\work\runtime_frames\play_<date>_<time>.bin
    // (the layout of the test command's dump, with count 0xFFFFFFFF = "until the end of the
    // file"). tools\hd\upscale_icons.py makes the missing frames from these files: icons of
    // missions nobody dumped by hand turn up in HD after the next run of the tool.
    std::vector<uint64_t> g_wantedSeen;
    FILE* g_wantedFile = nullptr;
    bool  g_wantedFailed = false;

    void NoteWanted(uint32_t frame, uint64_t content) {
        if (g_wantedFailed || !g_bank || std::find(g_wantedSeen.begin(), g_wantedSeen.end(), content) != g_wantedSeen.end()) return;
        if (g_wantedSeen.size() >= 20000) return;
        g_wantedSeen.push_back(content);
        BankFrame e{};
        uint32_t count = 0, bytes = 0;
        if (!ReadTableEntry(g_bank, frame, &e, &count, &bytes) || bytes == 0) return;
        if (!g_wantedFile) {
            const std::string dir = GameDir() + "hd\\work";
            CreateDirectoryA(dir.c_str(), nullptr);
            CreateDirectoryA((dir + "\\runtime_frames").c_str(), nullptr);
            SYSTEMTIME t{};
            GetLocalTime(&t);
            char name[64];
            std::snprintf(name, sizeof(name), "\\runtime_frames\\play_%04u%02u%02u_%02u%02u%02u.bin", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            g_wantedFile = std::fopen((dir + name).c_str(), "wb");
            if (!g_wantedFile) {
                g_wantedFailed = true;
                return;
            }
            const uint32_t header[4] = { 0x52465452u, 1u, 0xFFFFFFFFu, kBankFileFrames };
            std::fwrite(header, sizeof(header), 1, g_wantedFile);
            Logger::Info("HD sprites: run-time frames without an upscaled version are collected in hd\\work%s "
                         "(tools\\hd\\upscale_icons.py makes them).", name);
        }
        DumpRecord rec{};
        rec.frame = frame;
        rec.w = e.w;
        rec.h = e.h;
        rec.flag = e.flag;
        rec.palette = e.palette;
        rec.size = e.size;
        rec.bytes = bytes;
        std::fwrite(&rec, sizeof(rec), 1, g_wantedFile);
        std::fwrite(e.data, 1, bytes, g_wantedFile);
        std::fflush(g_wantedFile);
    }

    // Content key of a run-time frame (0 = none). Called from the decoder hook: the frame's data
    // has just been read by the game, so it is there.
    uint64_t ContentKey(uint32_t frame, const BankFrame& f) {
        if (f.palette != 0xFFFF || !f.data || f.w == 0 || f.h == 0) return 0;
        uint64_t head = 0;
        std::memcpy(&head, f.data, sizeof(head)); // at least two rows' headers: always 8 bytes
        const auto it = g_runTime.find(frame);
        if (it != g_runTime.end() && it->second.data == f.data && it->second.w == f.w && it->second.h == f.h &&
            it->second.head == head) {
            return it->second.key;
        }
        RunTimeFrame r{ f.data, f.w, f.h, 0, head, 0 };
        r.bytes = RowDataBytes(f.data, f.w, f.h);
        if (r.bytes) {
            const uint16_t dims[2] = { f.w, f.h };
            r.key = Fnv1a64(static_cast<const uint8_t*>(f.data), r.bytes,
                            Fnv1a64(reinterpret_cast<const uint8_t*>(dims), sizeof(dims), 0xCBF29CE484222325ull));
            if (r.key == 0) r.key = 1;
        }
        if (g_runTime.size() > 50000) g_runTime.clear();
        g_runTime[frame] = r;
        return r.key;
    }

    uint16_t ShadowColor() {
        __try {
            void* engine = *reinterpret_cast<void**>(kAddrEngine2);
            if (!engine) return 0;
            return static_cast<uint16_t>(reinterpret_cast<uint32_t(__thiscall*)(void*, int)>(kAddrShadowColor)(engine, 0xF));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    void Evict(size_t needed) {
        while (g_readyBytes + needed > kCacheBudget && !g_ready.empty()) {
            auto oldest = g_ready.begin();
            for (auto it = g_ready.begin(); it != g_ready.end(); ++it) {
                if (it->second.lastUse < oldest->second.lastUse) oldest = it;
            }
            g_readyBytes -= oldest->second.px.size() * 2;
            g_ready.erase(oldest);
        }
    }

    // The HD frame as it has to be drawn: marker pixels replaced by the shadow colour where the
    // game's decoder does that too. nullptr = not available.
    // Night and fog levels. The game mixes every decoded pixel with a colour (0x005046E0, called
    // with tone 1: 50 % and RGB(0,30,60); tone 2: 60 % and white):
    //     channel = channel * percent / 100 + colour channel * (100 - percent) / 100
    // green is treated as 5 bits; the colour key and the shadow colour are left alone.
    void TintFrame(std::vector<uint16_t>& px, uint32_t tone, uint16_t shadowColor, bool hasShadowColor) {
        const int percent = tone == 1 ? 50 : 60;
        const uint32_t colour = tone == 1 ? 0x00E7u : 0xFFFFu; // 0x005EF320(0, 0x1E, 0x3C) -> 0x00E7
        const int keep = 100 - percent;
        const int rAdd = static_cast<int>((colour >> 11) * keep) / 100;
        const int gAdd = static_cast<int>(((colour >> 5) & 0x3E) * keep) / 100;
        const int bAdd = static_cast<int>((colour & 0x1F) * keep) / 100;
        for (uint16_t& p : px) {
            if (p == kKey || p == kMarker || (hasShadowColor && p == shadowColor)) continue;
            const int g = static_cast<int>(((p >> 5) & 0x3E) * percent) / 100 + gAdd;
            const int r = static_cast<int>((p >> 11) * percent) / 100 + rAdd;
            const int b = static_cast<int>((p & 0x1F) * percent) / 100 + bAdd;
            uint16_t out = static_cast<uint16_t>(((r << 11) & 0xF800) | ((g << 5) & 0x07C0) | (b & 0x1F));
            if (out == kKey || out == kMarker) out = static_cast<uint16_t>(out + 1); // must stay a real colour
            p = out;
        }
    }

    const Ready* GetReady(const Decoded& rec, uint16_t shadowColor) {
        const uint64_t state = (static_cast<uint64_t>(rec.flag ? shadowColor : 0xFFFFu) << 32) |
                               (static_cast<uint64_t>(rec.flag) << 48) | (static_cast<uint64_t>(rec.tone & 3) << 49);
        const bool runTime = rec.frame >= kBankFileFrames;
        if (runTime && rec.content == 0) return nullptr;
        // a run-time frame goes by its content (a 64-bit hash mixed with the state; the top bit
        // keeps these keys apart from those of the bank's frames, which fit in 51 bits)
        const uint64_t key = runTime ? ((rec.content * 0x9E3779B97F4A7C15ull) ^ state) | (1ull << 63) : (rec.frame | state);
        auto it = g_ready.find(key);
        if (it != g_ready.end()) {
            it->second.lastUse = ++g_useCounter;
            return it->second.px.empty() ? nullptr : &it->second;
        }
        Ready r;
        r.w = rec.fw * 2;
        r.h = rec.fh * 2;
        const bool found = runTime ? ReadIcon(rec.content, r.w, r.h, r.px)
                                   : (ReadFrame(g_stores[0], rec.frame, r.w, r.h, r.px) || ReadFrame(g_stores[1], rec.frame, r.w, r.h, r.px));
        if (!found) {
            if (runTime) NoteWanted(rec.frame, rec.content);
            // remember that it is missing: no file access on every draw
            if (g_ready.size() > 60000) {
                g_ready.clear();
                g_readyBytes = 0;
            }
            r.px.clear();
            r.lastUse = ++g_useCounter;
            g_ready.emplace(key, std::move(r));
            return nullptr;
        }
        Evict(r.px.size() * 2);
        if (rec.flag) {
            // what 0x005E3919 does at low resolution: marker -> shadow colour, and a real pixel
            // that happens to have that colour is nudged away from it
            for (uint16_t& p : r.px) {
                if (p == kMarker) p = shadowColor;
                else if (p == shadowColor && p != kKey) p = static_cast<uint16_t>(p + 1);
            }
        }
        if (rec.tone == 1 || rec.tone == 2) {
            TintFrame(r.px, rec.tone, shadowColor, rec.flag != 0);
        }
        r.lastUse = ++g_useCounter;
        g_readyBytes += r.px.size() * 2;
        return &g_ready.emplace(key, std::move(r)).first->second;
    }

    void OpenBetween() {
        if (g_betweenTried) return;
        g_betweenTried = true;
        const std::string dir = GameDir() + "hd\\sprites\\_between\\";
        const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE; // the generator may be appending
        HANDLE idx = CreateFileA((dir + "between.idx").c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (idx == INVALID_HANDLE_VALUE) {
            Logger::Info("HD sprites: no in-between frames (hd\\sprites\\_between\\between.idx).");
            return;
        }
        struct Header { char magic[4]; uint32_t version, count, reserved; } hdr{};
        DWORD got = 0;
        bool ok = ReadFile(idx, &hdr, sizeof(hdr), &got, nullptr) && got == sizeof(hdr) &&
                  std::memcmp(hdr.magic, "HDBT", 4) == 0 && hdr.version == 1 && hdr.count < 4000000;
        if (ok && hdr.count) {
            g_between.resize(hdr.count);
            const DWORD bytes = static_cast<DWORD>(hdr.count * sizeof(BetweenEntry));
            ok = ReadFile(idx, g_between.data(), bytes, &got, nullptr) && got == bytes;
        }
        CloseHandle(idx);
        if (ok) {
            // the lookup is a binary search: the order must be right
            ok = std::is_sorted(g_between.begin(), g_between.end(), [](const BetweenEntry& x, const BetweenEntry& y) {
                return x.a != y.a ? x.a < y.a : x.b < y.b;
            });
        }
        if (ok) {
            ok = OpenData(g_betweenDat, dir + "between.dat");
        }
        if (!ok) {
            g_between.clear();
            Logger::Warn("HD sprites: the in-between frame store could not be read - ignored.");
            return;
        }
        size_t usable = 0;
        for (const BetweenEntry& e : g_between) usable += e.size != 0;
        Logger::Info("HD sprites: %zu in-between frames available (%zu frame pairs known).", usable, g_between.size());

        // their quality (see g_betweenQ): only for exactly this index. between.q belongs to the store of the
        // current HD pack, between.v1.q to the first one (soft frames by mixing, 2026-11-14): the mod archive
        // carries both, the one whose CRC matches the installed index is taken.
        g_betweenQ.clear();
        struct QHeader { char magic[4]; uint32_t version, count, crc; } qh{};
        uint32_t crc = Crc32(0, reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr));
        crc = Crc32(crc, reinterpret_cast<const uint8_t*>(g_between.data()), g_between.size() * sizeof(BetweenEntry));
        const char* used = nullptr;
        bool any = false;
        for (const char* name : { "between.q", "between.v1.q" }) {
            HANDLE qf = CreateFileA((dir + name).c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (qf == INVALID_HANDLE_VALUE) continue;
            any = true;
            bool qok = ReadFile(qf, &qh, sizeof(qh), &got, nullptr) && got == sizeof(qh) && std::memcmp(qh.magic, "HDBQ", 4) == 0 &&
                       qh.version == 1 && qh.count == g_between.size() && qh.crc == crc;
            if (qok) {
                g_betweenQ.resize(qh.count);
                qok = ReadFile(qf, g_betweenQ.data(), qh.count, &got, nullptr) && got == qh.count;
            }
            CloseHandle(qf);
            if (qok) {
                used = name;
                break;
            }
            g_betweenQ.clear();
        }
        if (!any) {
            Logger::Info("HD sprites: no quality list for them (hd\\sprites\\_between\\between.q) - all are used.");
            return;
        }
        if (!used) {
            Logger::Warn("HD sprites: no between.q belongs to this in-between store - all frames are used.");
            return;
        }
        Logger::Info("HD sprites: quality list %s.", used);
        const int limit = SherwoodConfig::Get().hdInBetweenQuality;
        size_t kept = 0;
        for (size_t i = 0; i < g_between.size(); ++i) kept += g_between[i].size != 0 && g_betweenQ[i] <= limit;
        Logger::Info("HD sprites: in-between frames up to a difference of %d of 255 (HDInBetweenQuality): %zu of %zu used.",
                     limit, kept, usable);
    }

    // The frame half-way between `prev` and `cur` (either order), and where its top-left corner
    // lies relative to that of `cur`. nullptr = none.
    const Ready* GetBetween(uint32_t prev, uint32_t cur, uint32_t tone, int* dx, int* dy) {
        OpenBetween();
        if (g_between.empty()) return nullptr;
        const uint32_t a = (std::min)(prev, cur), b = (std::max)(prev, cur);
        const auto it = std::lower_bound(g_between.begin(), g_between.end(), std::make_pair(a, b),
                                         [](const BetweenEntry& e, const std::pair<uint32_t, uint32_t>& k) {
                                             return e.a != k.first ? e.a < k.first : e.b < k.second;
                                         });
        if (it == g_between.end() || it->a != a || it->b != b || it->size == 0) return nullptr;
        if (!g_betweenQ.empty() && g_betweenQ[static_cast<size_t>(it - g_between.begin())] > SherwoodConfig::Get().hdInBetweenQuality) {
            return nullptr;         // too soft: the animation as the game shows it
        }
        *dx = cur == it->a ? it->dxA : it->dxB;
        *dy = cur == it->a ? it->dyA : it->dyB;
        const uint64_t key = (1ull << 62) | (static_cast<uint64_t>(tone & 3) << 40) | static_cast<uint64_t>(it - g_between.begin());
        auto hit = g_ready.find(key);
        if (hit != g_ready.end()) {
            hit->second.lastUse = ++g_useCounter;
            return hit->second.px.empty() ? nullptr : &hit->second;
        }
        Ready r;
        r.w = it->w;
        r.h = it->h;
        r.lastUse = ++g_useCounter;
        if (!ReadEntry(g_betweenDat, IndexEntry{ it->offset, it->size, it->w, it->h }, r.w, r.h, r.px)) {
            r.px.clear();
            g_ready.emplace(key, std::move(r));
            return nullptr;
        }
        Evict(r.px.size() * 2);
        if (tone == 1 || tone == 2) TintFrame(r.px, tone, 0, false);
        g_readyBytes += r.px.size() * 2;
        return &g_ready.emplace(key, std::move(r)).first->second;
    }

    // Which frame did this sprite object show in the previous game tick?
    uint32_t TrackSprite(void* sprite, uint32_t frame) {
        if (g_anim.size() > 20000) g_anim.clear();
        SpriteAnim& a = g_anim[sprite];
        if (a.tick != g_tick) {
            a.prev = (a.tick + 1 == g_tick) ? a.cur : frame;
            a.cur = frame;
            a.tick = g_tick;
        } else if (a.cur != frame) {
            a.cur = a.prev = frame; // two frames inside one tick: not an animation step that can be followed
        }
        return a.prev;
    }

    uint32_t __fastcall Hook_SpriteDecode(void* sprite, void* edx, void* arg) {
        g_spriteNow = sprite;
        const uint32_t r = o_SpriteDecode(sprite, edx, arg);
        g_spriteNow = nullptr;
        return r;
    }

    // Does the surface still hold the frame the decoder put there? Compared by transparency at a
    // grid of points: an overdrawn surface, or another frame, does not agree. A surface the game
    // cut occluded parts out of may be transparent where the frame is solid, but not the reverse.
    bool SurfaceMatches(const Buf& src, const Decoded& rec, const Ready& hd, double* colourDiff) {
        int tested = 0, hidden = 0, extra = 0;
        uint32_t diffSum = 0, diffCount = 0;
        const int stepX = (std::max)(1, rec.fw / 12), stepY = (std::max)(1, rec.fh / 12);
        for (int y = stepY / 2; y < rec.fh && y < src.h; y += stepY) {
            const auto* row = reinterpret_cast<const uint16_t*>(src.bits + static_cast<size_t>(y) * src.pitch);
            const uint16_t* h0 = hd.px.data() + static_cast<size_t>(2 * y) * hd.w;
            const uint16_t* h1 = h0 + hd.w;
            for (int x = stepX / 2; x < rec.fw && x < src.w; x += stepX) {
                const uint16_t low = row[x];
                const uint16_t q[4] = { h0[2 * x], h0[2 * x + 1], h1[2 * x], h1[2 * x + 1] };
                const bool lowClear = low == kKey;
                int hdClear = 0, solid = 0, r = 0, g = 0, b = 0;
                for (const uint16_t p : q) {
                    if (p == kKey) { ++hdClear; continue; }
                    if (p == kMarker) continue;
                    ++solid;
                    r += p >> 11; g += (p >> 5) & 0x3F; b += p & 0x1F;
                }
                ++tested;
                if (lowClear && hdClear == 0) ++hidden;
                if (!lowClear && hdClear == 4) ++extra;
                if (!lowClear && solid == 4) {
                    // same colours? (night/fog tint, recoloured palettes ... must agree with what the game decoded)
                    diffSum += static_cast<uint32_t>(std::abs((low >> 11) * 4 - r)) * 2 +       // 5 bits -> 8 bits, mean of 4
                               static_cast<uint32_t>(std::abs(((low >> 5) & 0x3F) * 4 - g)) +
                               static_cast<uint32_t>(std::abs((low & 0x1F) * 4 - b)) * 2;
                    ++diffCount;
                }
            }
        }
        const double diff = diffCount ? static_cast<double>(diffSum) / (3.0 * diffCount) : 0.0; // per channel, of 255
        if (colourDiff) *colourDiff = diff;
        // up to 1 in 8 may disagree in shape (thin outlines move by half a pixel)
        if (tested == 0 || extra * 8 > tested) return false;
        if (diffCount >= 6 && diff > kMaxColourDiff) return false;
        return rec.masked || hidden * 8 <= tested;
    }

    // The record of the surface whose pixels start at `bits`, checked against the HD frame.
    const Ready* Lookup(const Buf& src, const Decoded** recOut) {
        if (g_variant == "off" || !SherwoodConfig::Get().hdSprites) return nullptr;
        auto it = g_decoded.find(src.bits);
        if (it == g_decoded.end()) return nullptr;
        Decoded& rec = it->second;
        if (rec.checked < 0 || rec.tone > 2 || rec.w != src.w || rec.h != src.h || rec.fw > src.w || rec.fh > src.h) return nullptr;
        const uint16_t shadow = rec.flag ? ShadowColor() : 0;
        const Ready* hd = GetReady(rec, shadow);
        if (!hd) return nullptr;
        if (rec.checked == 0) {
            double colourDiff = 0.0;
            rec.checked = SurfaceMatches(src, rec, *hd, &colourDiff) ? 1 : -1;
            if (colourDiff > g_worstColourDiff && rec.checked > 0) g_worstColourDiff = colourDiff;
            if (rec.checked > 0 && colourDiff > 24.0) {
                // accepted, but not by much: worth a look at the upscaled frame (once per frame, a few lines per session)
                static std::vector<uint32_t> s_close;
                if (s_close.size() < 25 && std::find(s_close.begin(), s_close.end(), rec.frame) == s_close.end()) {
                    s_close.push_back(rec.frame);
                    Logger::Info("HD sprites: frame %u (%ux%u, cut=%d, tone=%u) accepted with a large colour difference: %.1f of 255 (limit %.0f).",
                                 rec.frame, rec.fw, rec.fh, rec.masked ? 1 : 0, rec.tone, colourDiff, kMaxColourDiff);
                }
            }
            if (rec.checked < 0) {
                static std::vector<uint32_t> s_logged;
                if (s_logged.size() < 40 && std::find(s_logged.begin(), s_logged.end(), rec.frame) == s_logged.end()) {
                    s_logged.push_back(rec.frame);
                    Logger::Info("HD sprites: frame %u (%ux%u in a %dx%d surface, cut=%d, tone=%u) does not look like its upscaled version "
                                 "(colours differ by %.1f of 255); drawn the old way.",
                                 rec.frame, rec.fw, rec.fh, src.w, src.h, rec.masked ? 1 : 0, rec.tone, colourDiff);
                }
                return nullptr;
            }
        }
        if (recOut) *recOut = &rec;
        return hd;
    }

    void MarkCut(const LockInfo* lock) {
        __try {
            if (!lock || !lock->bits) return;
            auto it = g_decoded.find(lock->bits);
            if (it != g_decoded.end()) {
                it->second.masked = true;
                it->second.checked = 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    uint32_t __fastcall Hook_CutA(void* sprite, void* edx, void* a, LockInfo* lock, void* pos) {
        const uint32_t r = o_CutA(sprite, edx, a, lock, pos);
        MarkCut(lock);
        return r;
    }

    uint32_t __fastcall Hook_CutB(void* sprite, void* edx, void* a, LockInfo* lock, void* pos, uint32_t b) {
        const uint32_t r = o_CutB(sprite, edx, a, lock, pos, b);
        MarkCut(lock);
        return r;
    }

    void Remember(const LockInfo* lock, void* bank, uint32_t frame, uint32_t tone) {
        __try {
            if (!lock || !lock->bits || lock->bpp != 16) return;
            const auto* table = *reinterpret_cast<const BankFrame* const*>(static_cast<const uint8_t*>(bank) + 0xC);
            if (!table) return;
            const BankFrame& f = table[frame];
            Decoded rec;
            rec.frame = frame;
            rec.tone = tone;
            rec.w = lock->w;
            rec.h = lock->h;
            rec.fw = f.w;
            rec.fh = f.h;
            rec.lossless = f.palette == 0xFFFF;
            rec.flag = rec.lossless ? f.flag : 0;
            if (frame >= kBankFileFrames) rec.content = ContentKey(frame, f);
            rec.prev = (g_spriteNow && frame < kBankFileFrames) ? TrackSprite(g_spriteNow, frame) : frame;
            rec.sprite = g_spriteNow;
            g_bank = bank;
            if (g_decoded.size() > 20000) g_decoded.clear();
            g_decoded[lock->bits] = rec;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    void Forget(const LockInfo* lock) {
        __try {
            if (lock && lock->bits) g_decoded.erase(lock->bits);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    int __fastcall Hook_Decode(void* bank, void* edx, LockInfo* lock, uint32_t frame, uint32_t tone) {
        const int r = o_Decode(bank, edx, lock, frame, tone);
        Remember(lock, bank, frame, tone);
        return r;
    }

    // decoded through a mask: the surface holds only part of the frame
    int __fastcall Hook_DecodeMaskedA(void* bank, void* edx, LockInfo* lock, uint32_t frame, uint32_t a) {
        const int r = o_DecodeMaskedA(bank, edx, lock, frame, a);
        Forget(lock);
        return r;
    }

    int __fastcall Hook_DecodeMaskedB(void* bank, void* edx, LockInfo* lock, uint32_t frame, uint32_t a, uint32_t b) {
        const int r = o_DecodeMaskedB(bank, edx, lock, frame, a, b);
        Forget(lock);
        return r;
    }

    bool Hook(uintptr_t addr, const uint8_t* prolog, size_t n, void* detour, void** original) {
        return std::memcmp(reinterpret_cast<const void*>(addr), prolog, n) == 0 &&
               MH_CreateHook(reinterpret_cast<LPVOID>(addr), detour, original) == MH_OK &&
               MH_EnableHook(reinterpret_cast<LPVOID>(addr)) == MH_OK;
    }

    inline uint16_t Darken565(uint32_t px, int32_t mul, const uint16_t* m) {
        const uint32_t b = static_cast<uint32_t>((static_cast<int32_t>((px & 0x1F) << 10) * mul) >> 16) & m[0];
        const uint32_t g = static_cast<uint32_t>((static_cast<int32_t>(((px >> 5) & 0x3F) << 9) * mul) >> 16) & m[1];
        const uint32_t r = static_cast<uint32_t>((static_cast<int32_t>((px >> 11) << 10) * mul) >> 16) & m[2];
        return static_cast<uint16_t>(((b >> 8) & 0xFF) | (g >> 2) | (r << 3));
    }

    // What the game cut out of a sprite surface because walls, trees ... hide it, as a map over
    // the frame's low-resolution pixels: 2 = hidden, 1 = visible. Known where the upscaled frame
    // is solid (there a colour-key pixel in the surface can only be a cut); spread from there to
    // the pixels around, because the in-between frame covers a slightly different area than the
    // frame the game decoded. Returns false if nothing is hidden.
    bool HiddenMap(const Buf& sb, const Decoded& rec, const Ready& hd, std::vector<uint8_t>& state) {
        const int fw = rec.fw, fh = rec.fh;
        state.assign(static_cast<size_t>(fw) * fh, 0);
        bool any = false;
        for (int y = 0; y < fh; ++y) {
            const auto* low = reinterpret_cast<const uint16_t*>(sb.bits + static_cast<size_t>(y) * sb.pitch);
            const uint16_t* h0 = hd.px.data() + static_cast<size_t>(2 * y) * hd.w;
            const uint16_t* h1 = h0 + hd.w;
            uint8_t* s = state.data() + static_cast<size_t>(y) * fw;
            for (int x = 0; x < fw; ++x) {
                if (h0[2 * x] == kKey || h0[2 * x + 1] == kKey || h1[2 * x] == kKey || h1[2 * x + 1] == kKey) continue;
                s[x] = low[x] == kKey ? 2 : 1;
                any |= s[x] == 2;
            }
        }
        if (!any) return false;
        static std::vector<uint8_t> next;
        for (int round = 0; round < 6; ++round) {
            next = state;
            for (int y = 0; y < fh; ++y) {
                for (int x = 0; x < fw; ++x) {
                    const size_t i = static_cast<size_t>(y) * fw + x;
                    if (state[i]) continue;
                    uint8_t v = 0;
                    if (x > 0 && state[i - 1]) v = (std::max)(v, state[i - 1]);
                    if (x + 1 < fw && state[i + 1]) v = (std::max)(v, state[i + 1]);
                    if (y > 0 && state[i - fw]) v = (std::max)(v, state[i - fw]);
                    if (y + 1 < fh && state[i + fw]) v = (std::max)(v, state[i + fw]);
                    next[i] = v;
                }
            }
            state.swap(next);
        }
        return true;
    }

    // An in-between frame, whole, with its top-left corner at (x0, y0) of the destination.
    // `hidden` (may be nullptr): HiddenMap of the frame the game decoded, fw x fh; the in-between
    // frame's pixel (x, y) lies over pixel ((x + ox) / 2, (y + oy) / 2) of that frame.
    void DrawBetween(const Ready& mid, const Buf& db, int x0, int y0, uint32_t factor,
                     const uint8_t* hidden, int fw, int fh, int ox, int oy) {
        const auto* mask = reinterpret_cast<const uint16_t*>(kAddrSpanMask);
        const int32_t mul = static_cast<int16_t>(static_cast<uint16_t>(factor << 6));
        const int xa = (std::max)(0, -x0), xb = (std::min)(mid.w, db.w - x0);
        for (int y = (std::max)(0, -y0); y < mid.h && y0 + y < db.h; ++y) {
            const uint16_t* row = mid.px.data() + static_cast<size_t>(y) * mid.w;
            auto* out = reinterpret_cast<uint16_t*>(db.bits + static_cast<size_t>(y0 + y) * db.pitch) + x0;
            const uint8_t* hrow = hidden ? hidden + static_cast<size_t>((std::min)((std::max)((y + oy) >> 1, 0), fh - 1)) * fw : nullptr;
            for (int x = xa; x < xb; ++x) {
                const uint32_t p = row[x];
                if (p == kKey) continue;
                if (hrow && hrow[(std::min)((std::max)((x + ox) >> 1, 0), fw - 1)] == 2) continue; // behind a wall, a tree ...
                out[x] = p == kMarker ? Darken565(out[x], mul, mask) : static_cast<uint16_t>(p);
            }
        }
    }
}

void InstallHooks() {
    if (!SherwoodConfig::Get().hdBackgrounds || !SherwoodConfig::Get().hdSprites) return;
    static const uint8_t kDecode[]  = { 0x53, 0x55, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x18, 0x8B, 0xF1, 0x57, 0xE8 };
    static const uint8_t kMaskedA[] = { 0x83, 0xEC, 0x08, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x18, 0x8B, 0xF1, 0x57, 0xE8 };
    static const uint8_t kMaskedB[] = { 0x83, 0xEC, 0x08, 0x53, 0x8B, 0x5C, 0x24, 0x14, 0x55, 0x56, 0x57, 0x8B, 0xF1, 0x53, 0xE8 };
    // all three or none: a surface filled by an unhooked decoder would keep a stale record
    const bool known = std::memcmp(reinterpret_cast<const void*>(kAddrDecode), kDecode, sizeof(kDecode)) == 0 &&
                       std::memcmp(reinterpret_cast<const void*>(kAddrDecodeMaskedA), kMaskedA, sizeof(kMaskedA)) == 0 &&
                       std::memcmp(reinterpret_cast<const void*>(kAddrDecodeMaskedB), kMaskedB, sizeof(kMaskedB)) == 0;
    const bool ok = known &&
        Hook(kAddrDecodeMaskedA, kMaskedA, sizeof(kMaskedA), reinterpret_cast<void*>(&Hook_DecodeMaskedA), reinterpret_cast<void**>(&o_DecodeMaskedA)) &&
        Hook(kAddrDecodeMaskedB, kMaskedB, sizeof(kMaskedB), reinterpret_cast<void*>(&Hook_DecodeMaskedB), reinterpret_cast<void**>(&o_DecodeMaskedB)) &&
        Hook(kAddrDecode, kDecode, sizeof(kDecode), reinterpret_cast<void*>(&Hook_Decode), reinterpret_cast<void**>(&o_Decode));
    if (ok) {
        static const uint8_t kCutA[] = { 0x83, 0xEC, 0x3C, 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x54, 0x57, 0x8B, 0x7C, 0x24, 0x50 };
        static const uint8_t kCutB[] = { 0x83, 0xEC, 0x48, 0x53, 0x55, 0x56, 0x8B, 0xD9, 0x8B, 0x0D, 0x88, 0x0A, 0x6C, 0x00 };
        const bool cut = Hook(kAddrCutA, kCutA, sizeof(kCutA), reinterpret_cast<void*>(&Hook_CutA), reinterpret_cast<void**>(&o_CutA)) &&
                         Hook(kAddrCutB, kCutB, sizeof(kCutB), reinterpret_cast<void*>(&Hook_CutB), reinterpret_cast<void**>(&o_CutB));
        g_cutKnown = cut;
        Logger::Info("HD sprites: bank decoder @ 0x%08X hooked - upscaled frames are drawn where a store has them (occlusion cuts followed: %d).",
                     static_cast<uint32_t>(kAddrDecode), cut ? 1 : 0);
        // push -1 / push 0x66F57B / mov eax,fs:[0] / push eax / mov fs:[0],esp / sub esp,0x3C / push ebp / push edi / mov edi,ecx
        static const uint8_t kSpriteDecode[] = { 0x6A, 0xFF, 0x68, 0x7B, 0xF5, 0x66, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50,
                                                 0x64, 0x89, 0x25, 0x00, 0x00, 0x00, 0x00, 0x83, 0xEC, 0x3C, 0x55, 0x57, 0x8B, 0xF9 };
        const bool sprite = Hook(kAddrSpriteDecode, kSpriteDecode, sizeof(kSpriteDecode), reinterpret_cast<void*>(&Hook_SpriteDecode),
                                 reinterpret_cast<void**>(&o_SpriteDecode));
        if (!sprite) o_SpriteDecode = nullptr;
        Logger::Info("HD sprites: sprite objects followed from tick to tick (in-between frames possible): %d.", sprite ? 1 : 0);
    } else {
        Logger::Warn("HD sprites: bank decoder not recognised - sprites are magnified as before.");
    }
}

void SetVariant(const char* name) {
    const std::string wanted = name ? name : "off";
    if (wanted == g_variant && g_stores[0].tried) return;
    CloseStore(g_stores[0]);
    CloseStore(g_stores[1]);
    DropFrames();
    g_variant = wanted;
    g_stores[0].name = wanted;
    g_stores[1].name = "_common";
}

void RequestDump(const char* path) {
    std::lock_guard<std::mutex> lock(g_dumpLock);
    g_dumpPath = path ? path : "";
    g_dumpWanted.store(!g_dumpPath.empty());
}

bool DumpPending() {
    return g_dumpWanted.load();
}

void RequestRunTimeDump(const char* path) {
    std::lock_guard<std::mutex> lock(g_dumpLock);
    g_runTimeDumpPath = path ? path : "";
    g_runTimeDumpWanted.store(!g_runTimeDumpPath.empty());
}

bool RunTimeDumpPending() {
    return g_runTimeDumpWanted.load();
}

void RequestMissReport(const char* path, int ticks) {
    std::lock_guard<std::mutex> lock(g_dumpLock);
    g_missPath = path ? path : "";
    g_missTicks.store(g_missPath.empty() ? 0 : (std::max)(1, ticks) + 1);
}

bool MissReportPending() {
    return g_missTicks.load() > 0;
}

bool MissActive() {
    return g_missTicks.load(std::memory_order_relaxed) > 0;
}

void NoteFallback(const uint8_t* bits, int w, int h, int path) {
    if (g_missTicks.load(std::memory_order_relaxed) <= 0) return;
    Miss m{};
    m.path = static_cast<uint8_t>(path);
    const auto it = bits ? g_decoded.find(bits) : g_decoded.end();
    if (it == g_decoded.end()) {
        m.frame = 0xFFFFFFFFu;
        m.w = static_cast<uint16_t>(w);
        m.h = static_cast<uint16_t>(h);
    } else {
        const Decoded& d = it->second;
        m.frame = d.frame;
        m.w = d.fw;
        m.h = d.fh;
        m.why = d.checked < 0 ? 2 : 1;
        m.content = d.content;
    }
    ++g_miss[m];
}

namespace {
    // one entry of the bank's frame table and the length of its row data, read defensively
    bool ReadTableEntry(void* bank, uint32_t index, BankFrame* out, uint32_t* count, uint32_t* bytes) {
        __try {
            const auto* base = static_cast<const uint8_t*>(bank);
            const auto* first = *reinterpret_cast<const BankFrame* const*>(base + 0xC);
            const auto* last = *reinterpret_cast<const BankFrame* const*>(base + 0x10);
            if (!first || last < first) return false;
            *count = static_cast<uint32_t>(last - first);
            if (index >= *count) return false;
            *out = first[index];
            *bytes = (out->palette == 0xFFFF && out->data && out->w && out->h) ? RowDataBytes(out->data, out->w, out->h) : 0;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // "RTFR", u32 version = 1, u32 number of frames that follow, u32 first run-time frame number;
    // per frame: u32 number, u16 w, u16 h, u8 flag, u8 0, u16 palette, u32 size field, u32 bytes,
    // then `bytes` of row data (0 = none: not 16-bit row RLE, or not loaded).
    void WriteRunTimeFrames(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return;
        uint32_t header[4] = { 0x52465452u, 1u, 0u, kBankFileFrames };
        std::fwrite(header, sizeof(header), 1, f);
        uint32_t written = 0, count = 0;
        for (uint32_t i = kBankFileFrames; g_bank; ++i) {
            BankFrame e{};
            uint32_t bytes = 0;
            if (!ReadTableEntry(g_bank, i, &e, &count, &bytes) || count > kBankFileFrames + 1000000u) break;
            DumpRecord rec{};
            rec.frame = i;
            rec.w = e.w;
            rec.h = e.h;
            rec.flag = e.flag;
            rec.palette = e.palette;
            rec.size = e.size;
            rec.bytes = bytes;
            std::fwrite(&rec, sizeof(rec), 1, f);
            if (bytes) std::fwrite(e.data, 1, bytes, f);
            ++written;
        }
        header[2] = written;
        std::fseek(f, 0, SEEK_SET);
        std::fwrite(header, sizeof(header), 1, f);
        std::fclose(f);
        Logger::Info("HD sprites: %u run-time frames of the bank written to %s (the bank holds %u frames).", written, path.c_str(), count);
    }

    void WriteMissReport(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) return;
        std::vector<std::pair<Miss, unsigned>> rows(g_miss.begin(), g_miss.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        static const char* const kPath[] = { "ddraw", "game", "ddraw-mirrored", "game-blend" };
        static const char* const kWhy[] = { "surface-not-from-decoder", "no-upscaled-frame", "does-not-match" };
        std::fprintf(f, "# drawn with an upscaled frame in this time: %u\n", g_drawn.load() - g_missDrawn0);
        for (const auto& row : rows) {
            const Miss& m = row.first;
            std::fprintf(f, "%u x %d %ux%u %s %s%s key=%016llx\n", row.second, static_cast<int>(m.frame), m.w, m.h,
                         kPath[m.path < 4 ? m.path : 0], kWhy[m.why < 3 ? m.why : 0],
                         m.frame != 0xFFFFFFFFu && m.frame >= kBankFileFrames ? " run-time" : "",
                         static_cast<unsigned long long>(m.content));
        }
        std::fclose(f);
    }
}

// main thread, once per game tick
void OnGameTick() {
    const int missLeft = g_missTicks.load(std::memory_order_relaxed);
    if (missLeft > 0) {
        std::string path;
        {
            std::lock_guard<std::mutex> lock(g_dumpLock);
            path = g_missPath;
        }
        static int s_started = 0;
        if (s_started == 0) {         // first tick of a collection
            g_miss.clear();
            g_missDrawn0 = g_drawn.load();
            s_started = 1;
        }
        if (missLeft == 1) {
            WriteMissReport(path);
            s_started = 0;
        }
        g_missTicks.store(missLeft - 1);
    }
    if (g_runTimeDumpWanted.load(std::memory_order_relaxed)) {
        std::string path;
        {
            std::lock_guard<std::mutex> lock(g_dumpLock);
            path = g_runTimeDumpPath;
        }
        WriteRunTimeFrames(path);
        g_runTimeDumpWanted.store(false);
    }
    if (!g_dumpWanted.load(std::memory_order_relaxed)) return;
    std::string path;
    {
        std::lock_guard<std::mutex> lock(g_dumpLock);
        path = g_dumpPath;
    }
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        // one line per sprite surface: which bank frame it holds
        for (const auto& kv : g_decoded) {
            const Decoded& d = kv.second;
            std::fprintf(f, "%u %ux%u surface %ux%u %s tone=%u flag=%u hd=%d key=%016llx\n", d.frame, d.fw, d.fh, d.w, d.h,
                         d.lossless ? "lossless" : "blocks", d.tone, d.flag, d.checked, static_cast<unsigned long long>(d.content));
        }
        std::fclose(f);
    }
    g_dumpWanted.store(false);
}

bool TryStretch(const Buf& dst, const RECT& d, const RECT& clip, const Buf& src, const RECT& s, uint16_t key) {
    if (!o_Decode || !g_cutKnown || key != kKey) return false;
    const Decoded* rec = nullptr;
    const Ready* hd = Lookup(src, &rec);
    if (!hd) return false;
    // destination pixel (X, Y) shows HD pixel (2*s.left + X - d.left, 2*s.top + Y - d.top) and
    // belongs to pixel (hx/2, hy/2) of the surface; beyond the frame the surface is transparent
    const int offX = 2 * s.left - d.left, offY = 2 * s.top - d.top;
    const bool cut = rec->masked;
    for (int y = clip.top; y < clip.bottom; ++y) {
        const int hy = y + offY;
        if (hy < 0 || hy >= hd->h) continue;
        const uint16_t* row = hd->px.data() + static_cast<size_t>(hy) * hd->w;
        const auto* low = reinterpret_cast<const uint16_t*>(src.bits + static_cast<size_t>(hy >> 1) * src.pitch);
        auto* out = reinterpret_cast<uint16_t*>(dst.bits + static_cast<size_t>(y) * dst.pitch);
        const int x0 = (std::max)(static_cast<int>(clip.left), -offX);
        const int x1 = (std::min)(static_cast<int>(clip.right), hd->w - offX);
        for (int x = x0; x < x1; ++x) {
            const int hx = x + offX;
            uint16_t p = row[hx];
            if (p == kKey) continue;
            if (cut || p == kMarker) {
                // the surface has the last word: hidden behind something (colour key), and for a
                // shadow pixel the colour the game gave it
                const uint16_t l = low[hx >> 1];
                if (l == kKey) continue;
                if (p == kMarker) p = l;
            }
            out[x] = p;
        }
    }
    g_drawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool TryShadowBlit(const uint16_t* src, uint16_t* dst, uint32_t cols, uint32_t rows,
                   int srcPitch, int dstPitch, uint32_t factor, uint32_t marker, bool keyed) {
    if (!o_Decode || !g_cutKnown) return false;
    Buf sb{}, db{};
    if (!FindLockedBuf(src, sb) || !FindLockedBuf(dst, db) || sb.bits == db.bits || sb.pitch != srcPitch || db.pitch != dstPitch) {
        return false;
    }
    const Decoded* rec = nullptr;
    const Ready* hd = Lookup(sb, &rec);
    if (!hd) return false;
    const size_t sOff = static_cast<size_t>(reinterpret_cast<const uint8_t*>(src) - sb.bits);
    const size_t dOff = static_cast<size_t>(reinterpret_cast<const uint8_t*>(dst) - db.bits);
    const int sx = static_cast<int>((sOff % static_cast<size_t>(sb.pitch)) / 2), sy = static_cast<int>(sOff / static_cast<size_t>(sb.pitch));
    const int dx = static_cast<int>((dOff % static_cast<size_t>(db.pitch)) / 2), dy = static_cast<int>(dOff / static_cast<size_t>(db.pitch));
    if (dx + static_cast<int>(cols) * 2 > db.w || dy + static_cast<int>(rows) * 2 > db.h ||
        sx + static_cast<int>(cols) > sb.w || sy + static_cast<int>(rows) > sb.h) {
        return false;
    }

    if (TraceActive()) { // test harness: where every sprite frame is drawn (top-left corner of the whole frame)
        char text[160];
        std::snprintf(text, sizeof(text), "SPR sprite=%p frame=%u prev=%u at=(%d,%d) size=%dx%d cut=%d phase=%.2f",
                      rec->sprite, rec->frame, rec->prev, dx - 2 * sx, dy - 2 * sy, hd->w, hd->h, rec->masked ? 1 : 0, Timing::RenderPhase());
        TraceMark(text);
    }

    // In-between frame: the sprite showed another frame in the previous tick, this picture lies in
    // the first part of the tick, and the store has a frame half-way between the two. What the
    // game cut out of the decoded frame (hidden behind something) is cut out of it as well.
    if (keyed && rec->prev != rec->frame && g_betweenOn.load(std::memory_order_relaxed) &&
        rec->fw <= sb.w && rec->fh <= sb.h && hd->w == rec->fw * 2 && hd->h == rec->fh * 2) {
        float phase = Timing::RenderPhase();
        if (DebugFlags() & 16u) phase = 0.0f; // test harness: every picture of the tick shows the in-between frame
        if (phase >= 0.0f && phase < kBetweenPhase) {
            int ox = 0, oy = 0;
            static std::vector<uint8_t> hidden;
            const bool cutNow = rec->masked && HiddenMap(sb, *rec, *hd, hidden);
            const int fw = rec->fw, fh = rec->fh;
            if (const Ready* mid = GetBetween(rec->prev, rec->frame, rec->tone, &ox, &oy)) {
                DrawBetween(*mid, db, dx - 2 * sx + ox, dy - 2 * sy + oy, factor, cutNow ? hidden.data() : nullptr, fw, fh, ox, oy);
                if (TraceActive()) {
                    char text[120];
                    std::snprintf(text, sizeof(text), "BTW %u->%u at=(%d,%d) size=%dx%d", rec->prev, rec->frame,
                                  dx - 2 * sx + ox, dy - 2 * sy + oy, mid->w, mid->h);
                    TraceMark(text);
                }
                g_betweenDrawn.fetch_add(1, std::memory_order_relaxed);
                g_drawn.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
    }

    // In the upscaled frame the shadow is the bank's marker 0x001F; in the surface the game has
    // replaced it by a colour of its own and passes that colour as `marker` (0x2964 by day).
    const auto* mask = reinterpret_cast<const uint16_t*>(kAddrSpanMask);
    const int32_t mul = static_cast<int16_t>(static_cast<uint16_t>(factor << 6));
    const bool cut = rec->masked;
    for (int y = 0; y < static_cast<int>(rows) * 2; ++y) {
        const int hy = 2 * sy + y;
        if (hy >= hd->h) break;
        const uint16_t* row = hd->px.data() + static_cast<size_t>(hy) * hd->w;
        const auto* low = reinterpret_cast<const uint16_t*>(sb.bits + static_cast<size_t>(sy + (y >> 1)) * sb.pitch) + sx;
        auto* out = reinterpret_cast<uint16_t*>(db.bits + static_cast<size_t>(dy + y) * db.pitch) + dx;
        const int n = (std::min)(static_cast<int>(cols) * 2, hd->w - 2 * sx);
        for (int x = 0; x < n; ++x) {
            const uint32_t p = row[2 * sx + x];
            if (p == kKey) {
                if (keyed) continue;
                out[x] = static_cast<uint16_t>(low[x >> 1]); // not keyed: the surface's own pixel
                continue;
            }
            if (cut && low[x >> 1] == kKey) continue; // hidden behind something
            if (p == kMarker || p == marker) out[x] = Darken565(out[x], mul, mask);
            else out[x] = static_cast<uint16_t>(p);
        }
    }
    g_drawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool TryBlendBlit(const uint16_t* src, uint16_t* dst, uint32_t cols, uint32_t rows, int srcPitch, int dstPitch, uint32_t key) {
    if (!o_Decode || !g_cutKnown) return false;
    Buf sb{}, db{};
    if (!FindLockedBuf(src, sb) || !FindLockedBuf(dst, db) || sb.bits == db.bits || sb.pitch != srcPitch || db.pitch != dstPitch) {
        return false;
    }
    const Decoded* rec = nullptr;
    const Ready* hd = Lookup(sb, &rec);
    if (!hd) return false;
    const size_t sOff = static_cast<size_t>(reinterpret_cast<const uint8_t*>(src) - sb.bits);
    const size_t dOff = static_cast<size_t>(reinterpret_cast<const uint8_t*>(dst) - db.bits);
    const int sx = static_cast<int>((sOff % static_cast<size_t>(sb.pitch)) / 2), sy = static_cast<int>(sOff / static_cast<size_t>(sb.pitch));
    const int dx = static_cast<int>((dOff % static_cast<size_t>(db.pitch)) / 2), dy = static_cast<int>(dOff / static_cast<size_t>(db.pitch));
    if (dx + static_cast<int>(cols) * 2 > db.w || dy + static_cast<int>(rows) * 2 > db.h ||
        sx + static_cast<int>(cols) > sb.w || sy + static_cast<int>(rows) > sb.h) {
        return false;
    }
    // The game's routine (0x005E8B10), per pixel that is not the colour key:
    //     alpha = min(255, upper five bits of green * 16);  out = src * alpha / 256 + dst * (256 - alpha) / 256
    // per channel, with the channel masks of 5-6-5. Same arithmetic here, for every HD pixel.
    const bool cut = rec->masked;
    for (int y = 0; y < static_cast<int>(rows) * 2; ++y) {
        const int hy = 2 * sy + y;
        if (hy >= hd->h) break;
        const uint16_t* row = hd->px.data() + static_cast<size_t>(hy) * hd->w;
        const auto* low = reinterpret_cast<const uint16_t*>(sb.bits + static_cast<size_t>(sy + (y >> 1)) * sb.pitch) + sx;
        auto* out = reinterpret_cast<uint16_t*>(db.bits + static_cast<size_t>(dy + y) * db.pitch) + dx;
        const int n = (std::min)(static_cast<int>(cols) * 2, hd->w - 2 * sx);
        for (int x = 0; x < n; ++x) {
            const uint32_t s = row[2 * sx + x];
            if (s == key || s == kKey) continue;
            if (cut && low[x >> 1] == kKey) continue;
            uint32_t a = (s >> 2) & 0x1F0;
            if (a == 0) continue;
            if (a > 0xFF) a = 0xFF;
            const uint32_t ia = 0x100 - a, d = out[x];
            const uint32_t r = (((((s >> 8) & 0xF8) * a) >> 8) & 0xF8) + (((((d >> 8) & 0xF8) * ia) >> 8) & 0xF8);
            const uint32_t g = (((((s >> 3) & 0xFC) * a) >> 8) & 0xFC) + (((((d >> 3) & 0xFC) * ia) >> 8) & 0xFC);
            const uint32_t b = ((((s & 0x1F) << 3) * a) >> 11) + ((((d & 0x1F) << 3) * ia) >> 11);
            out[x] = static_cast<uint16_t>((r << 8) + (g << 3) + b);
        }
    }
    g_drawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void OnSurfaceWritten(const uint8_t* bits) {
    if (g_decoded.empty()) return;
    const auto it = g_decoded.find(bits);
    if (it != g_decoded.end()) g_decoded.erase(it);
}

unsigned FramesDrawn() {
    return g_drawn.load(std::memory_order_relaxed);
}

void OnTickFrame() {
    ++g_tick;
}

void SetBetween(bool on) {
    g_betweenOn.store(on);
}

bool BetweenOn() {
    return g_betweenOn.load(std::memory_order_relaxed);
}

bool BetweenAvailable() {
    OpenBetween();
    return o_SpriteDecode != nullptr && !g_between.empty();
}

unsigned BetweenDrawn() {
    return g_betweenDrawn.load(std::memory_order_relaxed);
}

double WorstColourDiff() {
    return g_worstColourDiff;
}

} // namespace DDM::HD::Sprites
