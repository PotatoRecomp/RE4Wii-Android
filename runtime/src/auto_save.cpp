#include "auto_save.h"

#include "cheats.h"

#include "det_clock.h"
#include "memory.h"
#include "nand_path.h"
#include "runtime_log.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Layout of an RB4E08 save file (bh4_dataNN, 0x10000 bytes on NAND), read from the Wii's
// cCard::makeSaveData (0x800206A8) and checked against a save the game wrote:
//   0x0000  "resident evil 4", 0x20 "FILE nn", banner and icon (kept from an existing save)
//   0x2000  CRC of the 0x1FC bytes after it; 0x2004 magic 0x309; 0x2008 OSCalendarTime; 0x2030 mode
//   0x2034  pSaveData->pGlobal (0x36F8, pG + 0x4F90 .. ; SaveKind at +0x34F0)
//   0x572C  pSaveData->pItm (0x1204)
//   0x6930  pSaveData->pRm header (16 bytes), 0x6940 its rows (216 bytes each, a stage's range)
//   0xE7D0  pSaveData->pSs (4), 0xE7D4 pSaveData->pMr (0x304)
//   0x203D  = byte 0x5520 (game mode); 0xEAF8 CRC of everything before it
namespace AutoSave {
namespace {

constexpr uint32_t kPSaveDataAddr = 0x8043E0C4u;  // pSaveData (r13 - 0x695C)
constexpr uint32_t kRoomRangeTable = 0x8034D904u;  // u16 rows: +4/+8 count/start, stage 5: +6/+10
constexpr uint32_t kFileSize = 0x10000;
constexpr uint32_t kGameOffset = 0x2034, kGameSize = 0x36F8;
constexpr uint32_t kItemOffset = 0x572C, kItemSize = 0x1204;
constexpr uint32_t kRoomOffset = 0x6930, kRoomRowsOffset = 0x6940, kRoomRowSize = 216;
constexpr uint32_t kSscrnOffset = 0xE7D0, kSscrnSize = 4;
constexpr uint32_t kMerchantOffset = 0xE7D4, kMerchantSize = 0x304;
constexpr uint32_t kCrcOffset = 0xEAF8;
constexpr uint32_t kSaveKindInGame = 0x34F0;  // pG->SaveKind; -1 = the room-entry checkpoint
constexpr uint32_t kMagic = 0x309;

int g_slot = -1;  // 0-based file number, -1 = off
std::atomic<int> g_notice{0};  // FILE number to warn about once (TakeNotice), 0 = none
std::atomic<bool> g_noticeArmed{true};    // the session's first write still has to post the notice
uint32_t g_lastHash = 0;
uint32_t g_frames = 0;

uint32_t Crc(const uint8_t* data, size_t length) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 24;
            for (int j = 0; j < 8; ++j) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0;
    for (size_t i = 0; i < length; ++i) crc = (crc << 8) ^ table[(crc >> 24) ^ data[i]];
    return crc;
}

void Put32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

bool GuestRange(uint32_t addr, uint32_t size) {
    const bool mem1 = addr >= 0x80000000u && addr + size <= 0x81800000u;
    const bool mem2 = addr >= 0x90000000u && addr + size <= 0x94000000u;
    return size != 0 && (mem1 || mem2);
}

bool CopyGuest(uint8_t* dst, uint32_t addr, uint32_t size) {
    if (!GuestRange(addr, size)) return false;
    const uint8_t* src = Memory::GetPointer(addr, size);
    if (src == nullptr) return false;
    std::memcpy(dst, src, size);
    return true;
}

// The save folder: title/00010000/<game id>/data under the NAND root.
const std::filesystem::path& SaveDirectory() {
    static const std::filesystem::path dir = [] {
        char id[9] = {};
        if (const uint8_t* p = Memory::GetPointer(0x80000000u, 4)) {
            std::snprintf(id, sizeof(id), "%02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
        }
        return RuntimeNandPath::DiscoverNandRootPath() / "title" / "00010000" / id / "data";
    }();
    return dir;
}

// Banner, icon and the parts of the image the checkpoint does not carry, from a save the game
// wrote (the auto-save slot's own last image first).
std::optional<std::vector<uint8_t>> Template() {
    std::vector<int> order{g_slot};
    for (int i = 0; i < 20; ++i) {
        if (i != g_slot) order.push_back(i);
    }
    for (const int slot : order) {
        char name[16];
        std::snprintf(name, sizeof(name), "bh4_data%02d", slot);
        std::ifstream in(SaveDirectory() / name, std::ios::binary);
        std::vector<uint8_t> data(kFileSize);
        if (!in.read(reinterpret_cast<char*>(data.data()), kFileSize)) continue;
        const uint32_t magic = (uint32_t(data[0x2004]) << 24) | (uint32_t(data[0x2005]) << 16) |
                               (uint32_t(data[0x2006]) << 8) | data[0x2007];
        if (magic == kMagic) return data;
    }
    return std::nullopt;
}

void Write(std::vector<uint8_t> image, uint16_t room) {
    std::thread([image = std::move(image), room] {
        char name[16];
        std::snprintf(name, sizeof(name), "bh4_data%02d", g_slot);
        const std::filesystem::path path = SaveDirectory() / name;
        const std::filesystem::path temp = SaveDirectory() / (std::string(name) + ".tmp");
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
            if (!out) {
                RT_LOGF(RT_TAG_RUNTIME, "[autosave] could not write %s\n", temp.string().c_str());
                return;
            }
        }
        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
        RT_LOGF(RT_TAG_RUNTIME, "[autosave] room %03X saved to FILE %02d%s\n", room, g_slot + 1,
                ec ? " (rename failed)" : "");
        if (!ec && g_noticeArmed.exchange(false)) {
            g_notice.store(g_slot + 1, std::memory_order_relaxed);
        }
    }).detach();
}

void SaveCheckpoint() {
    uint32_t head = 0, game = 0, item = 0, rooms = 0, sscrn = 0, merchant = 0, base = 0;
    if (!Memory::TryRead32(kPSaveDataAddr, head) || !GuestRange(head, 0x1C) || !Memory::TryRead32(head, base) ||
        base == 0 || !Memory::TryRead32(head + 0x08, game) || !Memory::TryRead32(head + 0x0C, item) ||
        !Memory::TryRead32(head + 0x10, rooms) || !Memory::TryRead32(head + 0x14, sscrn) ||
        !Memory::TryRead32(head + 0x18, merchant) || !GuestRange(game, kGameSize)) {
        return;
    }
    uint32_t saveKind = 0;
    if (!Memory::TryRead32(game + kSaveKindInGame, saveKind) || saveKind != 0xFFFFFFFFu) {
        return;  // not a room-entry checkpoint (a loaded or card-saved image)
    }
    const uint8_t* block = Memory::GetPointer(game, kGameSize);
    if (block == nullptr) return;
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < kGameSize; ++i) hash = (hash ^ block[i]) * 16777619u;
    if (hash == g_lastHash) return;

    auto image = Template();
    if (!image) {
        if (g_lastHash != 1) RT_LOGF(RT_TAG_RUNTIME, "[autosave] no save to copy the banner from yet: save once by hand\n");
        g_lastHash = 1;
        return;
    }
    uint8_t* f = image->data();
    bool ok = CopyGuest(f + kGameOffset, game, kGameSize) && CopyGuest(f + kItemOffset, item, kItemSize) &&
              CopyGuest(f + kRoomOffset, rooms, 16) && CopyGuest(f + kSscrnOffset, sscrn, kSscrnSize) &&
              CopyGuest(f + kMerchantOffset, merchant, kMerchantSize);
    // The stage's range of room rows (stage 5 has its own).
    const uint8_t stage = f[0x2050];
    const uint8_t* table = Memory::GetPointer(kRoomRangeTable, 12);
    if (ok && table != nullptr) {
        const uint32_t at = stage == 5 ? 6 : 4;
        const uint32_t count = (uint32_t(table[at]) << 8) | table[at + 1];
        const uint32_t start = (uint32_t(table[at + 4]) << 8) | table[at + 5];
        const uint32_t bytes = count * kRoomRowSize;
        if (kRoomRowsOffset + bytes <= kSscrnOffset) {
            std::memset(f + kRoomRowsOffset, 0, kSscrnOffset - kRoomRowsOffset);
            ok = CopyGuest(f + kRoomRowsOffset, rooms + 16 + start * kRoomRowSize, bytes);
        } else {
            ok = false;
        }
    }
    if (!ok || table == nullptr) {
        RT_LOGF(RT_TAG_RUNTIME, "[autosave] checkpoint not readable, skipped\n");
        g_lastHash = hash;
        return;
    }
    // A card save stores its mode as SaveKind; 1 = a normal save.
    Put32(f + kGameOffset + kSaveKindInGame, 1);
    char comment[32] = {};
    std::snprintf(comment, sizeof(comment), "FILE %02d", g_slot + 1);
    std::memset(f + 0x20, 0, 0x20);
    std::memcpy(f + 0x20, comment, std::strlen(comment));
    Put32(f + 0x2004, kMagic);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const int32_t calendar[10] = {local.tm_sec, local.tm_min, local.tm_hour, local.tm_mday, local.tm_mon,
                                  local.tm_year + 1900, local.tm_wday, local.tm_yday, 0, 0};
    for (int i = 0; i < 10; ++i) Put32(f + 0x2008 + i * 4, static_cast<uint32_t>(calendar[i]));
    Put32(f + 0x2030, 1);
    f[0x203D] = f[0x5520];
    Put32(f + 0x2000, Crc(f + 0x2004, 0x1FC));
    Put32(f + kCrcOffset, Crc(f, kCrcOffset));
    g_lastHash = hash;
    const uint16_t room = static_cast<uint16_t>((uint32_t(f[0x2050]) << 8) | f[0x2051]);
    Write(std::move(*image), room);
}

} // namespace

void OnRetrace() {
    if (g_slot == -1) {
        // Off unless asked for (Settings > Save Data > Auto Save sets RE4_AUTO_SAVE=1): saving the
        // room on every entry wrote Play As states into the save (1.1 reports).
        const char* env = std::getenv("RE4_AUTO_SAVE");
        if (env == nullptr || std::strcmp(env, "1") != 0) {
            g_slot = -2;
        } else {
            const char* slot = std::getenv("RE4_AUTO_SAVE_SLOT");
            const int n = slot ? std::atoi(slot) : 20;
            g_slot = (n >= 1 && n <= 20) ? n - 1 : 19;
        }
    }
    if (g_slot < 0 || DetClock::Enabled() || ++g_frames % 30 != 0) {
        return;
    }
    // Never while Play As puts another character in Leon's story.
    if (Cheats::PlayAs() != 0) {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            RT_LOGF(RT_TAG_RUNTIME, "[autosave] off while Play As is on\n");
        }
        return;
    }
    SaveCheckpoint();
}

int TakeNotice() {
    return g_notice.exchange(0, std::memory_order_relaxed);
}

} // namespace AutoSave
