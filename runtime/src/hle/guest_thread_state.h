#pragma once

#include <atomic>
#include <cstdint>

#include "isa/ppc_isa_context.h"

// Per-thread state that practically only the host thread running guest code touches, on paths
// the game calls tens of thousands of times a frame (OSRestoreInterrupts, GXBegin). libmain is
// dlopen'd, so each thread_local there costs a TLS descriptor call (tlsdesc_resolver_dynamic).
// The first thread to ask owns a plain member, found by comparing the thread pointer; any other
// thread keeps its own thread_local copy, so the per-thread meaning is unchanged. Each user needs
// its own State type (the fallback is one thread_local per type). Off aarch64: thread_local only.
template <typename State>
class GuestThreadState {
public:
    State& Get() noexcept
    {
        const uintptr_t tp = HostThreadPointer();
        if (tp != 0) {
            uintptr_t owner = m_owner.load(std::memory_order_relaxed);
            if (owner == tp) {
                return m_owned;
            }
            if (owner == 0 && m_owner.compare_exchange_strong(owner, tp, std::memory_order_relaxed)) {
                return m_owned;
            }
        }
        return Fallback();
    }

private:
    static State& Fallback() noexcept
    {
        static thread_local State state{};
        return state;
    }

    std::atomic<uintptr_t> m_owner{0};
    State m_owned{};
};
