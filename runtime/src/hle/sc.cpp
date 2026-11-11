#include "hle_stubs.h"

#include "console_identity.h"
#include "sc_serial_contract.h"
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "memory.h"
#include "runtime_config.h"
#include "aurora_events.h"
#include "runtime_log.h"

namespace {

// Use the SDK's own value tables, including its unknown-region result.
uint32_t LookupProductRegion(uint32_t table, uint32_t stride, uint32_t count,
                             const std::string& value) {
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t entry = table + index * stride;
        if (!Memory::Contains(entry, stride)) {
            break;
        }
        const auto* bytes = static_cast<const uint8_t*>(Memory::GetPointer(entry, stride));
        if (bytes[0] == 0xFF) {
            break;
        }
        if (value.size() < stride - 1 &&
            std::memcmp(bytes + 1, value.c_str(), value.size() + 1) == 0) {
            return bytes[0];
        }
    }
    return 0xFFFFFFFFu;
}

} // namespace

// SCCheckStatus is polled in OSInit's busy loop (while(SCCheckStatus()==1) waits on async SYSCONF
// load via NAND IPC); we have no async IPC callbacks, so return 0 (SUCCESS) immediately.

// 0x801C6830 -> SCCheckStatus() (RE4; status byte r13-0x5FB8, set by SCInit 0x801C6790)
// Returns: 0 = success, 1 = busy, 2 = error. NW4HBM asserts SC_STATUS_OK at Home Menu init.
extern "C" uint32_t SCCheckStatus_HLE()
{
    // Return 0 (success) immediately to avoid infinite busy-wait in OSInit
    return 0;
}
PPC_NATIVE_OVERRIDE(801C6830, SCCheckStatus_HLE, uint32_t, (), ());


// 0x801DD1D0 -> SCGetAspectRatio()
// Returns: 0 = 4:3, 1 = 16:9. The display settings' aspect at boot: 16:9 and Fill both run the
// game in widescreen (Fill then widens its canvas further).
extern "C" uint32_t SCGetAspectRatio_HLE()
{
    return DisplaySettings::GameWidescreen() ? 1u : 0u;
}


// 0x801DD2B0 -> SCGetEuRgb60Mode()
// Returns: 0 = PAL50, 1 = PAL60/RGB60
extern "C" uint32_t SCGetEuRgb60Mode_HLE()
{
    // Default to PAL60 so PAL builds do not fall back to the half-rate PAL50
    // sync path on modern displays. Actual texture/cache correctness is handled
    // elsewhere; this only exposes the intended SYSCONF setting.
    return 1;
}


// 0x801DD320 -> SCGetLanguage()
// The console language (SC_LANG_*: 1 English, 3 French, 4 Spanish; the USA disc has EngUS, FraUS
// and SpaUS). NSMBW picks its message, layout and Home Button files by it. From the Language setting
// ([game] language), read once: a change takes effect the next time the game starts, never halfway.
extern "C" uint32_t SCGetLanguage_HLE()
{
    static const uint32_t language = [] {
        const std::string chosen = RuntimeConfigFile::Language();
        const uint32_t code = chosen == "es" ? 4u : chosen == "fr" ? 3u : 1u;
        RT_LOGF(RT_TAG_HLE, "SCGetLanguage -> %u (%s)\n", code, chosen.c_str());
        return code;
    }();
    return language;
}

PPC_NATIVE_OVERRIDE(801C81C8, SCGetLanguage_HLE, uint32_t, (), ());

// Expose the selected emulated NAND identity through the SDK SC APIs.

extern "C" uint32_t SCGetProductArea_HLE()
{
    return LookupProductRegion(0x802F7C80u, 5, 13,  // SC area table {code, "JPN"...} (Basara SCGetProductArea 803A7368)
                               RuntimeConsoleIdentity::Current().area);
}

PPC_NATIVE_OVERRIDE(801C8678, SCGetProductArea_HLE, uint32_t, (), ());



extern "C" uint32_t SCGetProductSN_HLE(uint32_t serialAddress)
{
    const std::string& serial = RuntimeConsoleIdentity::Current().serial;
    return RuntimeScSerial::Write(serial, serialAddress,
        [](uint32_t address, size_t size) { return Memory::Contains(address, size); },
        [](uint32_t address, uint32_t value) { Memory::Write32(address, value); });
}



// These stubs make the game think all titles are installed; otherwise it checks title ID
// 0x00010004524d4350 ("RMCP", Mario Kart Wii PAL) and reports error code 5.

// 0x801AE4A0 -> OS__IsTitleInstalled(titleIdHi, titleIdLo)
// Returns: 1 = installed, 0 = not installed
extern "C" uint32_t OS__IsTitleInstalled(uint32_t titleIdHi, uint32_t titleIdLo)
{
    RT_LOGF(RT_TAG_HLE, "CINS: OSIsTitleInstalled(0x%08X%08X) -> 1 (stubbed as installed)\n",
            titleIdHi, titleIdLo);
    return 1; // Always report installed
}


// 0x801AD1D4 -> OS__CheckInstall(requiredBlocks, titleIdHi, titleIdLo, outFlagsPtr): returns 0 with
// outFlagsPtr = 0x3 (bit0 has data, bit1 has update; bit2 would be needs-blocks) i.e. fully installed.
extern "C" uint32_t OS__CheckInstall(uint32_t requiredBlocks, uint32_t titleIdHi, 
                                      uint32_t titleIdLo, uint32_t outFlagsPtr)
{
    RT_LOGF(RT_TAG_HLE, "OS__CheckInstall(blocks=%u, 0x%08X%08X) -> success (stubbed)\n",
            requiredBlocks, titleIdHi, titleIdLo);
    if (outFlagsPtr != 0) {
        Memory::Write32(outFlagsPtr, 0x3); // has data + has update = fully installed
    }
    return 0; // Success
}

