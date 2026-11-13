#include "abi_bridge.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"

#include <cstdint>

// HOME Menu sound shutdown: nw4hbm::snd::SoundThread::Shutdown (main.dol 0x8024190C), called by
// SoundSystem::ShutdownSoundSystem (0x80241658) as the HOME Menu closes. The AX callback posts a tick
// (1) to the sound thread's 8-slot queue every audio frame without blocking; when the sound thread falls
// behind, the queue is full, the stop message (2) sent with NOBLOCK fails, Shutdown returns 0 and
// SoundSystem panics (snd_SoundSystem.cpp:141), and the game crashes (Beta 8 report, Infinix X6876; the
// same bug and fix as the Sengoku Basara port's 0x804770F4). Same steps here, but stale ticks are
// dropped to make room for the stop message.
namespace {

constexpr uint32_t kStaticGuard = 0x803FBC68u;            // guard byte of the SoundThread's static init
constexpr uint32_t kSoundThread = kStaticGuard + 0x10u;   // the SoundThread object; OSThread at +0
constexpr uint32_t kQueue = kSoundThread + 0x2338u;       // its OSMessageQueue
constexpr uint32_t kCreatedOffset = 0x2394u;              // u8 "thread created" of the object Shutdown gets
constexpr uint32_t kOSSendMessage = 0x8017C2A8u;
constexpr uint32_t kOSReceiveMessage = 0x8017C370u;
constexpr uint32_t kOSJoinThread = 0x8017F05Cu;

} // namespace

extern "C" void HbmSoundShutdown_HLE_8024190c(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    if (Memory::Read8(self + kCreatedOffset) == 0u) {
        ctx->gpr[3] = 0;
        return;
    }
    if (self != kSoundThread || Memory::Read8(kStaticGuard) == 0u) {
        // Not the object the original code would message either; report a failed shutdown as it would.
        RT_LOGF(RT_TAG_OS, "hbm sound: unexpected shutdown target 0x%08X\n", self);
        ctx->gpr[3] = 0;
        return;
    }
    bool sent = false;
    int dropped = 0;
    for (int attempt = 0; attempt < 16 && !sent; ++attempt) {
        ctx->gpr[3] = kQueue;
        ctx->gpr[4] = 2;  // stop
        ctx->gpr[5] = 0;  // OS_MESSAGE_NOBLOCK
        InvokeIndirectCpu(kOSSendMessage, ctx);
        sent = ctx->gpr[3] != 0;
        if (!sent) {
            ctx->gpr[3] = kQueue;
            ctx->gpr[4] = 0;  // drop the message
            ctx->gpr[5] = 0;  // OS_MESSAGE_NOBLOCK
            InvokeIndirectCpu(kOSReceiveMessage, ctx);
            dropped += ctx->gpr[3] != 0 ? 1 : 0;
        }
    }
    if (dropped != 0) {
        RT_LOGF(RT_TAG_OS, "hbm sound: queue was full, dropped %d tick(s) to send stop (%s)\n", dropped,
                sent ? "sent" : "still full");
    }
    if (!sent) {
        ctx->gpr[3] = 0;
        return;
    }
    ctx->gpr[3] = self;  // OSThread at +0
    ctx->gpr[4] = 0;
    InvokeIndirectCpu(kOSJoinThread, ctx);
    if (ctx->gpr[3] == 0) {
        RT_LOGF(RT_TAG_OS, "hbm sound: OSJoinThread failed\n");
    }
    Memory::Write8(self + kCreatedOffset, 0);
    ctx->gpr[3] = 1;
}
PPC_NATIVE_OVERRIDE_VOID(8024190C, HbmSoundShutdown_HLE_8024190c, (CpuContext* ctx), (ctx));
