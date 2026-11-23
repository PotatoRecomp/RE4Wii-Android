// Mods: a mod folder's sound/quickslot.wav, played once per D-pad quick slot change (quick_slots.cpp).
//
// The same file as CubeEvil4's mod packs ("Weapon Switch Sound"). Searched in every [paths] overlay
// root; the last root that has it wins. PCM 16-bit WAV, mono or stereo, 8..96 kHz, at most kMaxSeconds.
// It is read once at the first change, resampled (linear) to the output rate and added to the game's
// audio (AudioBackend::PushWiiAiSamplesBE16, after the DSP mixed it). Without the file nothing plays.
#include "hle/storage/riivolution.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace ModSound {
namespace {
constexpr uint32_t kMaxSeconds = 4;

std::mutex g_mutex;
bool g_loaded = false;            // the search ran
std::vector<int16_t> g_source;    // the file's samples, interleaved
uint32_t g_sourceRate = 0;
uint32_t g_sourceChannels = 0;
std::vector<int16_t> g_out;       // resampled to g_outRate, stereo
uint32_t g_outRate = 0;
std::atomic<int64_t> g_pos{-1};   // the next output frame to mix, -1 idle

uint32_t Le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

bool ReadWav(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) return false;
    uint32_t channels = 0, rate = 0, bits = 0, format = 0;
    for (size_t at = 12; at + 8 <= d.size();) {
        const uint32_t size = Le32(d.data() + at + 4);
        const uint8_t* body = d.data() + at + 8;
        const size_t avail = std::min<size_t>(size, d.size() - at - 8);
        if (std::memcmp(d.data() + at, "fmt ", 4) == 0 && avail >= 16) {
            format = Le16(body);
            channels = Le16(body + 2);
            rate = Le32(body + 4);
            bits = Le16(body + 14);
        } else if (std::memcmp(d.data() + at, "data", 4) == 0) {
            if (format != 1 || bits != 16 || (channels != 1 && channels != 2) || rate < 8000 || rate > 96000) return false;
            const size_t frames = std::min<size_t>(avail / (2 * channels), size_t(rate) * kMaxSeconds);
            g_source.resize(frames * channels);
            for (size_t i = 0; i < g_source.size(); ++i) g_source[i] = static_cast<int16_t>(Le16(body + 2 * i));
            g_sourceRate = rate;
            g_sourceChannels = channels;
            return !g_source.empty();
        }
        at += 8 + size + (size & 1u);
    }
    return false;
}

void LoadOnce() {
    if (g_loaded) return;
    g_loaded = true;
    std::filesystem::path found;
    for (const auto& overlay : RuntimeRiivolution::Overlays()) {
        std::error_code ec;
        const auto p = overlay.root / "sound" / "quickslot.wav";
        if (std::filesystem::is_regular_file(p, ec)) found = p;
    }
    if (found.empty()) return;
    if (!ReadWav(found)) {
        RT_LOGF(RT_TAG_AUDIO, "mod sound: %s is not a 16-bit PCM WAV this reads\n", found.string().c_str());
        return;
    }
    RT_LOGF(RT_TAG_AUDIO, "mod sound: %s (%u Hz, %u channel(s), %zu frames)\n", found.string().c_str(), g_sourceRate,
            g_sourceChannels, g_source.size() / g_sourceChannels);
}

void Resample(uint32_t rate) {
    g_outRate = rate;
    g_out.clear();
    const size_t inFrames = g_source.size() / std::max<uint32_t>(g_sourceChannels, 1u);
    if (inFrames == 0 || rate == 0) return;
    const size_t outFrames = static_cast<size_t>(uint64_t(inFrames) * rate / g_sourceRate);
    g_out.resize(outFrames * 2);
    for (size_t i = 0; i < outFrames; ++i) {
        const double src = double(i) * g_sourceRate / rate;
        const size_t a = std::min(static_cast<size_t>(src), inFrames - 1), b = std::min(a + 1, inFrames - 1);
        const double t = src - double(a);
        for (uint32_t c = 0; c < 2; ++c) {
            const uint32_t ch = g_sourceChannels == 2 ? c : 0;
            const double v = g_source[a * g_sourceChannels + ch] * (1.0 - t) + g_source[b * g_sourceChannels + ch] * t;
            g_out[i * 2 + c] = static_cast<int16_t>(v);
        }
    }
}
} // namespace

void PlayQuickSlot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadOnce();
    if (!g_source.empty()) g_pos.store(0, std::memory_order_relaxed);
}

// Adds the playing sound to `frames` stereo frames (left, right) at `rate` Hz.
void Mix(int16_t* lr, size_t frames, uint32_t rate) {
    if (g_pos.load(std::memory_order_relaxed) < 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_outRate != rate) Resample(rate);
    int64_t pos = g_pos.load(std::memory_order_relaxed);
    const int64_t total = static_cast<int64_t>(g_out.size() / 2);
    for (size_t i = 0; i < frames && pos >= 0 && pos < total; ++i, ++pos) {
        for (int c = 0; c < 2; ++c) {
            const int v = lr[i * 2 + c] + g_out[size_t(pos) * 2 + c];
            lr[i * 2 + c] = static_cast<int16_t>(std::clamp(v, -32768, 32767));
        }
    }
    g_pos.store(pos >= total ? -1 : pos, std::memory_order_relaxed);
}
} // namespace ModSound
