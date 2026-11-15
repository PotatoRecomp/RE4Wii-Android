#include "gx_thread.hpp"

#include "fifo.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"

#include <aurora/aurora.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace aurora {
extern std::atomic<AuroraFrameWorkerWaitCallback> g_frameWorkerWaitCallback;
}

namespace aurora::gx::thread {
namespace {
Module Log("aurora::gx::thread");

using Clock = std::chrono::steady_clock;

struct Chunk {
  uint8_t* data = nullptr;
  uint32_t size = 0;
  uint32_t capacity = 0;
};

struct Item {
  Chunk chunk;
  std::function<void()> fn;
  uint64_t fence = 0;
};

struct State {
  std::mutex mutex;
  std::condition_variable cv;     // producer -> GX thread: work queued or stop
  std::condition_variable idleCv; // GX thread -> producer: queue ran empty
  std::deque<Item> queue;
  std::vector<Chunk> freeChunks;
  std::thread thread;
  std::thread::id threadId{};
  bool started = false;
  bool stop = false;
  bool busy = false;
  uint64_t nextFence = 0;
  uint64_t fenceDone = 0;
  // Highest fence in the queue (or already run). A ticket above it is a lazy mark riding on the
  // producer's bytes that are not queued yet (pendingMark).
  uint64_t queuedFence = 0;
  uint64_t pendingMark = 0;
};

State g_state;
// on_gx_thread() runs for every GX call, so it compares thread pointers (TPIDR_EL0 / FS base) with
// two globals instead of reading a thread_local: in libmain.so that went through the dynamic TLS
// resolver (tlsdesc), ~2.5% of the game thread with on_gx_thread itself (Redmi simpleperf, 2026-11-11).
// g_gxThreadTp is the GX thread's; g_forwardingTp is the producer's while it runs forwarded calls itself
// (run_deferred, RE4_GX_THREAD=2), which then own GX state like the GX thread.
std::atomic<uintptr_t> g_gxThreadTp{0};
std::atomic<uintptr_t> g_forwardingTp{0};
#if defined(__aarch64__)
inline uintptr_t current_tp() noexcept { return reinterpret_cast<uintptr_t>(__builtin_thread_pointer()); }
#else
thread_local char t_tpMarker;
inline uintptr_t current_tp() noexcept { return reinterpret_cast<uintptr_t>(&t_tpMarker); }
#endif

std::atomic<uint64_t> g_chunks{0};
std::atomic<uint64_t> g_closures{0};
std::atomic<uint64_t> g_bytes{0};
std::atomic<uint64_t> g_syncs[static_cast<int>(SyncReason::Count)]{};
std::atomic<uint64_t> g_producerWaitNanos{0};
// The same wait, never reset: the VI's per-frame split of late frames (AuroraGxProducerWaitTotalNanos).
std::atomic<uint64_t> g_producerWaitTotalNanos{0};
std::atomic<uint64_t> g_gxBusyNanos{0};
// Stall watchdog (vi.cpp): the GX thread's kernel tid for a stack sample, and what it runs now
// (0 idle, 1 FIFO chunk, 2 closure) with the chunk's size.
std::atomic<int> g_gxThreadTid{0};
std::atomic<int> g_runningKind{0};
std::atomic<uint32_t> g_runningBytes{0};

void run_item(Item& item) {
  if (item.fn) {
    g_runningKind.store(2, std::memory_order_relaxed);
    item.fn();
    item.fn = nullptr;
  } else if (item.chunk.size != 0) {
    g_runningKind.store(1, std::memory_order_relaxed);
    g_runningBytes.store(item.chunk.size, std::memory_order_relaxed);
    fifo::process_chunk(item.chunk.data, item.chunk.size);
  }
  g_runningKind.store(0, std::memory_order_relaxed);
}

void thread_main() {
#if defined(__linux__)
  pthread_setname_np(pthread_self(), "GxThread");
  g_gxThreadTid.store(static_cast<int>(syscall(SYS_gettid)), std::memory_order_relaxed);
#endif
  g_gxThreadTp.store(current_tp(), std::memory_order_relaxed);
  // Busy time is measured per run of items, from waking up to going idle again, not per item: a
  // battle frame is ~430 chunks, and on Cortex-A76 phones every clock read costs (erratum 1188873
  // traps the counter read), which was 7.7% of this thread on a Redmi (Adreno 618).
  Clock::time_point busySince{};
  bool timing = false;
  for (;;) {
    Item item;
    {
      std::unique_lock lock(g_state.mutex);
      if (g_state.queue.empty()) {
        g_state.busy = false;
        if (timing) {
          timing = false;
          g_gxBusyNanos.fetch_add(
              std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - busySince).count(),
              std::memory_order_relaxed);
        }
        g_state.idleCv.notify_all();
        g_state.cv.wait(lock, [] { return g_state.stop || !g_state.queue.empty(); });
      }
      if (g_state.queue.empty() && g_state.stop) {
        g_state.busy = false;
        g_state.idleCv.notify_all();
        return;
      }
      item = std::move(g_state.queue.front());
      g_state.queue.pop_front();
      g_state.busy = true;
    }
    if (!timing) {
      timing = true;
      busySince = Clock::now();
    }
    try {
      run_item(item);
    } catch (const std::exception& e) {
      Log.error("GX thread item failed: {}", e.what());
    }
    if (item.chunk.data != nullptr) {
      item.chunk.size = 0;
      std::lock_guard lock(g_state.mutex);
      g_state.freeChunks.push_back(item.chunk);
    }
    if (item.fence != 0) {
      {
        std::lock_guard lock(g_state.mutex);
        g_state.fenceDone = std::max(g_state.fenceDone, item.fence);
      }
      g_state.idleCv.notify_all();
    }
  }
}

// RE4_GX_THREAD=4 (bisecting): same queue and hand-over points, but no second thread. Items run
// on the producer once RE4_GX_DEFER_ITEMS are queued (or at a sync/fence), so decode is late
// exactly as with the GX thread, yet nothing runs concurrently.
bool deferred() noexcept {
  static const bool on = [] {
    const char* value = std::getenv("RE4_GX_THREAD");
    return value != nullptr && value[0] == '4';
  }();
  return on;
}

size_t defer_items() noexcept {
  static const size_t n = [] {
    const char* value = std::getenv("RE4_GX_DEFER_ITEMS");
    const unsigned long v = value != nullptr ? std::strtoul(value, nullptr, 0) : 0ul;
    return v != 0 ? static_cast<size_t>(v) : size_t{32};
  }();
  return n;
}

bool g_inlineRunning = false; // producer only

// Runs queued items on the calling (producer) thread until the queue is empty or `ticket` is done.
void run_deferred(uint64_t ticket) {
  if (g_inlineRunning) return;
  g_inlineRunning = true;
  for (;;) {
    Item item;
    {
      std::lock_guard lock(g_state.mutex);
      if (g_state.queue.empty() || (ticket != 0 && g_state.fenceDone >= ticket)) break;
      item = std::move(g_state.queue.front());
      g_state.queue.pop_front();
    }
    g_forwardingTp.store(current_tp(), std::memory_order_relaxed);
    try {
      run_item(item);
    } catch (const std::exception& e) {
      Log.error("deferred GX item failed: {}", e.what());
    }
    g_forwardingTp.store(0, std::memory_order_relaxed);
    std::lock_guard lock(g_state.mutex);
    if (item.chunk.data != nullptr) {
      item.chunk.size = 0;
      g_state.freeChunks.push_back(item.chunk);
    }
    if (item.fence != 0) g_state.fenceDone = std::max(g_state.fenceDone, item.fence);
  }
  g_inlineRunning = false;
}

void maybe_run_deferred() {
  if (!deferred()) return;
  size_t queued;
  {
    std::lock_guard lock(g_state.mutex);
    queued = g_state.queue.size();
  }
  if (queued >= defer_items()) run_deferred(0);
}

void ensure_started_locked() {
  if (g_state.started) {
    return;
  }
  g_state.started = true;
  g_state.stop = false;
  g_state.busy = false;
  if (deferred()) {
    Log.info("GX thread deferred mode (no thread), {} items", defer_items());
    return;
  }
  g_state.thread = std::thread(thread_main);
  g_state.threadId = g_state.thread.get_id();
  Log.info("GX thread enabled");
}

void push_item_locked(Item item) {
  g_state.queuedFence = std::max(g_state.queuedFence, item.fence);
  g_state.queue.push_back(std::move(item));
}

// Producer only. Swaps the FIFO write buffer for a free one and queues the full one, carrying the
// pending lazy mark.
void enqueue_pending_bytes_locked() {
  auto& size = fifo::detail::sBufferSize;
  if (size == 0) {
    if (g_state.pendingMark != 0) {
      // Its bytes were dropped (clear_buffer): the mark still has to be reached.
      push_item_locked(Item{{}, nullptr, g_state.pendingMark});
      g_state.pendingMark = 0;
    }
    return;
  }
  Chunk full{fifo::detail::sBufferData, size, fifo::detail::sBufferCapacity};
  Chunk next;
  if (!g_state.freeChunks.empty()) {
    next = g_state.freeChunks.back();
    g_state.freeChunks.pop_back();
  } else {
    next.capacity = full.capacity;
    next.data = static_cast<uint8_t*>(std::malloc(next.capacity));
  }
  fifo::detail::sBufferData = next.data;
  fifo::detail::sBufferCapacity = next.capacity;
  fifo::detail::sBufferSize = 0;
  g_bytes.fetch_add(full.size, std::memory_order_relaxed);
  g_chunks.fetch_add(1, std::memory_order_relaxed);
  push_item_locked(Item{full, nullptr, g_state.pendingMark});
  g_state.pendingMark = 0;
}

// Dawn's GL backend has one EGL context; GX work on a second thread fails with EGL_BAD_ACCESS.
bool gl_backend() noexcept {
  return webgpu::g_backendType == wgpu::BackendType::OpenGLES ||
         webgpu::g_backendType == wgpu::BackendType::OpenGL;
}

} // namespace

bool enabled() noexcept {
  static const bool on = [] {
    const char* value = std::getenv("RE4_GX_THREAD");
    return value != nullptr && (value[0] == '1' || value[0] == '2' || value[0] == '3' || value[0] == '4');
  }();
  if (on && gl_backend()) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      Log.warn("GX thread off: Dawn's OpenGL ES context can only be current on one thread (EGL_BAD_ACCESS)");
    }
    return false;
  }
  return on;
}

namespace {
// RE4_GX_THREAD=3 (bisecting): everything still runs on the GX thread, but the producer waits
// for it after each hand-over, so nothing the game does next can overlap the decode.
bool lockstep() noexcept {
  static const bool on = [] {
    const char* value = std::getenv("RE4_GX_THREAD");
    return value != nullptr && value[0] == '3';
  }();
  return on;
}
} // namespace

bool on_gx_thread() noexcept {
  const uintptr_t tp = current_tp();
  return tp == g_gxThreadTp.load(std::memory_order_relaxed) || tp == g_forwardingTp.load(std::memory_order_relaxed);
}

void submit() {
  if (!offload() || fifo::detail::sBufferSize == 0) {
    return;
  }
  {
    std::lock_guard lock(g_state.mutex);
    ensure_started_locked();
    enqueue_pending_bytes_locked();
  }
  g_state.cv.notify_one();
  if (lockstep()) sync(SyncReason::Other);
  maybe_run_deferred();
}

namespace {
uint64_t post_impl(std::function<void()> fn, const char* name, bool fenced) {
  if (name != nullptr) {
    // First use of each forwarded call site (diagnostics).
    static std::mutex seenMutex;
    static std::vector<const char*> seen;
    std::lock_guard lock(seenMutex);
    if (std::find(seen.begin(), seen.end(), name) == seen.end() && seen.size() < 128) {
      seen.push_back(name);
      Log.info("first forwarded call: {}", name);
    }
  }
  // RE4_GX_THREAD=2 (bisecting): run forwarded calls on the producer after a full sync.
  static const bool inlinePosts = [] {
    const char* value = std::getenv("RE4_GX_THREAD");
    return value != nullptr && value[0] == '2';
  }();
  if (inlinePosts) {
    sync(SyncReason::Other);
    g_forwardingTp.store(current_tp(), std::memory_order_relaxed);
    fn();
    g_forwardingTp.store(0, std::memory_order_relaxed);
    return 0;
  }
  uint64_t ticket = 0;
  {
    std::lock_guard lock(g_state.mutex);
    ensure_started_locked();
    enqueue_pending_bytes_locked();
    if (fenced) ticket = ++g_state.nextFence;
    push_item_locked(Item{{}, std::move(fn), ticket});
    g_closures.fetch_add(1, std::memory_order_relaxed);
  }
  g_state.cv.notify_one();
  if (lockstep()) sync(SyncReason::Other);
  else sync_after(kSyncAfterPost);
  maybe_run_deferred();
  return ticket;
}

unsigned sync_after_mask() noexcept {
  static const unsigned mask = [] {
    const char* value = std::getenv("RE4_GX_SYNC_AFTER");
    return value != nullptr ? static_cast<unsigned>(std::strtoul(value, nullptr, 0)) : 0u;
  }();
  return mask;
}
} // namespace

void sync_after(unsigned bit) {
  if ((sync_after_mask() & bit) != 0 && offload()) sync(SyncReason::Other);
}

void post(std::function<void()> fn, const char* name) { post_impl(std::move(fn), name, false); }

uint64_t post_fenced(std::function<void()> fn, const char* name) {
  if (!offload()) {
    fn();
    return 0;
  }
  return post_impl(std::move(fn), name, true);
}

uint64_t mark_lazy() {
  if (!offload()) {
    return 0;
  }
  // The bisect modes (RE4_GX_THREAD=2/3/4) keep the plain fenced post.
  static const bool plain = [] {
    const char* value = std::getenv("RE4_GX_THREAD");
    return value != nullptr && value[0] == '1';
  }();
  if (!plain) {
    return post_fenced([] {}, nullptr);
  }
  bool wake = false;
  uint64_t ticket = 0;
  {
    std::lock_guard lock(g_state.mutex);
    ensure_started_locked();
    ticket = ++g_state.nextFence;
    if (fifo::detail::sBufferSize != 0) {
      // Reached with the chunk these bytes go out in (an older pending mark is covered by it).
      g_state.pendingMark = ticket;
      return ticket;
    }
    g_state.pendingMark = 0;
    if (!g_state.queue.empty()) {
      // Everything written is queued: reached with the last queued item.
      g_state.queue.back().fence = ticket;
      g_state.queuedFence = ticket;
    } else {
      push_item_locked(Item{{}, nullptr, ticket});
      wake = true;
    }
  }
  if (wake) {
    g_state.cv.notify_one();
  }
  return ticket;
}

bool fence_reached(uint64_t ticket) {
  if (ticket == 0 || !offload()) {
    return true;
  }
  std::lock_guard lock(g_state.mutex);
  return g_state.fenceDone >= ticket;
}

void wait_fence(uint64_t ticket, SyncReason reason) {
  if (ticket == 0 || !offload()) {
    return;
  }
  {
    // A lazy mark still riding on unqueued bytes (only its producer holds such a ticket).
    std::unique_lock lock(g_state.mutex);
    if (ticket > g_state.queuedFence) {
      enqueue_pending_bytes_locked();
      lock.unlock();
      g_state.cv.notify_one();
    }
  }
  if (deferred()) {
    g_syncs[static_cast<int>(reason)].fetch_add(1, std::memory_order_relaxed);
    run_deferred(ticket);
    return;
  }
  std::unique_lock lock(g_state.mutex);
  if (g_state.fenceDone >= ticket) {
    return;
  }
  g_syncs[static_cast<int>(reason)].fetch_add(1, std::memory_order_relaxed);
  g_state.cv.notify_one();
  const auto started = Clock::now();
  while (g_state.fenceDone < ticket) {
    if (g_state.idleCv.wait_for(lock, std::chrono::milliseconds(1), [ticket] { return g_state.fenceDone >= ticket; })) {
      break;
    }
    // Keep the guest's alarm/retrace pump alive while waiting.
    lock.unlock();
    if (const auto callback = g_frameWorkerWaitCallback.load(std::memory_order_acquire)) {
      callback();
    }
    lock.lock();
  }
  {
    const auto waited = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
    g_producerWaitNanos.fetch_add(waited, std::memory_order_relaxed);
    g_producerWaitTotalNanos.fetch_add(waited, std::memory_order_relaxed);
  }
}

void sync(SyncReason reason) {
  if (!offload()) {
    return;
  }
  g_syncs[static_cast<int>(reason)].fetch_add(1, std::memory_order_relaxed);
  std::unique_lock lock(g_state.mutex);
  if (!g_state.started) {
    // Nothing ever ran on the GX thread; whatever is pending is still the producer's to decode.
    return;
  }
  enqueue_pending_bytes_locked();
  if (deferred()) {
    lock.unlock();
    run_deferred(0);
    return;
  }
  if (g_state.queue.empty() && !g_state.busy) {
    return;
  }
  g_state.cv.notify_one();
  const auto started = Clock::now();
  while (!g_state.queue.empty() || g_state.busy) {
    if (g_state.idleCv.wait_for(lock, std::chrono::milliseconds(1),
                                [] { return g_state.queue.empty() && !g_state.busy; })) {
      break;
    }
    // Keep the guest's alarm/retrace pump alive, as a producer waiting on the frame worker does.
    lock.unlock();
    if (const auto callback = g_frameWorkerWaitCallback.load(std::memory_order_acquire)) {
      callback();
    }
    lock.lock();
  }
  {
    const auto waited = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
    g_producerWaitNanos.fetch_add(waited, std::memory_order_relaxed);
    g_producerWaitTotalNanos.fetch_add(waited, std::memory_order_relaxed);
  }
}

Stats take_stats() noexcept {
  Stats s;
  s.chunks = g_chunks.exchange(0, std::memory_order_relaxed);
  s.closures = g_closures.exchange(0, std::memory_order_relaxed);
  s.bytes = g_bytes.exchange(0, std::memory_order_relaxed);
  for (int i = 0; i < static_cast<int>(SyncReason::Count); ++i) {
    s.syncs[i] = g_syncs[i].exchange(0, std::memory_order_relaxed);
  }
  s.producerWaitNanos = g_producerWaitNanos.exchange(0, std::memory_order_relaxed);
  s.gxBusyNanos = g_gxBusyNanos.exchange(0, std::memory_order_relaxed);
  return s;
}

void shutdown() {
  {
    std::lock_guard lock(g_state.mutex);
    if (!g_state.started) {
      return;
    }
    g_state.stop = true;
  }
  g_state.cv.notify_all();
  if (g_state.thread.joinable()) {
    g_state.thread.join();
  }
  std::lock_guard lock(g_state.mutex);
  for (auto& chunk : g_state.freeChunks) {
    std::free(chunk.data);
  }
  g_state.freeChunks.clear();
  g_state.started = false;
}

} // namespace aurora::gx::thread

extern "C" int AuroraGxThreadTid() {
  return aurora::gx::thread::g_gxThreadTid.load(std::memory_order_relaxed);
}

// One line for the stall watchdog. try_lock: a thread stuck while holding the mutex must not take
// the watchdog with it.
extern "C" void AuroraGxThreadDebugState(char* out, size_t size) {
  using namespace aurora::gx::thread;
  const int kind = g_runningKind.load(std::memory_order_relaxed);
  const char* running = kind == 1 ? "chunk" : kind == 2 ? "closure" : "nothing";
  std::unique_lock lock(g_state.mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    std::snprintf(out, size, "mutex held; running %s (%u bytes)", running,
                  g_runningBytes.load(std::memory_order_relaxed));
    return;
  }
  std::snprintf(out, size,
                "queue %zu, busy %d, running %s (%u bytes), fences next %llu done %llu queued %llu pending %llu",
                g_state.queue.size(), g_state.busy ? 1 : 0, running, g_runningBytes.load(std::memory_order_relaxed),
                static_cast<unsigned long long>(g_state.nextFence),
                static_cast<unsigned long long>(g_state.fenceDone),
                static_cast<unsigned long long>(g_state.queuedFence),
                static_cast<unsigned long long>(g_state.pendingMark));
}

extern "C" uint64_t AuroraGxProducerWaitTotalNanos() {
  return aurora::gx::thread::g_producerWaitTotalNanos.load(std::memory_order_relaxed);
}

extern "C" void AuroraTakeGxThreadStats(uint64_t* chunks, uint64_t* closures, uint64_t* bytes, uint64_t* syncs,
                                        uint32_t syncCount, uint64_t* producerWaitNanos, uint64_t* gxBusyNanos) {
  const auto s = aurora::gx::thread::take_stats();
  *chunks = s.chunks;
  *closures = s.closures;
  *bytes = s.bytes;
  for (uint32_t i = 0; i < syncCount && i < static_cast<uint32_t>(aurora::gx::thread::SyncReason::Count); ++i) {
    syncs[i] = s.syncs[i];
  }
  *producerWaitNanos = s.producerWaitNanos;
  *gxBusyNanos = s.gxBusyNanos;
}

extern "C" bool AuroraGxThreadEnabled() { return aurora::gx::thread::enabled(); }

// Fence after everything submitted so far (0 when the GX thread is off: already reached).
extern "C" uint64_t AuroraGxThreadMark() {
  if (!aurora::gx::thread::offload()) return 0;
  return aurora::gx::thread::post_fenced([] {}, nullptr);
}

// The same fence without queuing anything now: it rides on the next chunk of FIFO bytes, so a
// display list per mark no longer goes out as its own chunk and wakes the GX thread (Redmi
// simpleperf: the post and its futex wake were a quarter of GX__CallDisplayList on the game thread).
extern "C" uint64_t AuroraGxThreadMarkLazy() { return aurora::gx::thread::mark_lazy(); }

extern "C" void AuroraGxThreadWaitMark(uint64_t ticket) {
  aurora::gx::thread::wait_fence(ticket, aurora::gx::thread::SyncReason::EndFrame);
}

extern "C" bool AuroraGxThreadMarkReached(uint64_t ticket) {
  return aurora::gx::thread::fence_reached(ticket);
}

// Run fn(arg) on the GX thread after everything submitted so far, or now when it is off.
extern "C" void AuroraGxThreadPostCall(void (*fn)(uint32_t), uint32_t arg) {
  if (!aurora::gx::thread::offload()) {
    fn(arg);
    return;
  }
  aurora::gx::thread::post([fn, arg] { fn(arg); }, nullptr);
}
