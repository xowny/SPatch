#include "RuntimePatch.h"

#include "Logger.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

namespace spatch::runtime_patch {
namespace {

bool SafeCopyFromProcess(std::uintptr_t address, void* destination, std::size_t size) {
    __try {
        std::memcpy(destination, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeCopyToProcess(std::uintptr_t address, const void* source, std::size_t size) {
    __try {
        std::memcpy(reinterpret_cast<void*>(address), source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeBytesEqual(std::uintptr_t address, std::span<const std::uint8_t> expected) {
    __try {
        return std::memcmp(
                   reinterpret_cast<const void*>(address), expected.data(), expected.size()) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

constexpr unsigned int kResumeAttempts = 8;
constexpr std::size_t kMaximumDeferredResumes = 4096;
constexpr std::size_t kMaximumEmergencyResumes = 4096;
constexpr std::size_t kMaximumOverflowResumes = 1024;
constexpr std::size_t kMaximumFrozenThreads = 1024;
// Executable writes must never run two independent suspend/write/resume
// transactions at once. Besides preventing overlapping patch races, this
// guarantees that the deferred-resume queue can contain at most one freeze's
// bounded 1024-thread set before the next mutation is allowed to begin.
std::mutex g_patch_transaction_mutex;

enum class ResumeResult {
    Resumed,
    Exited,
    Failed,
};

ResumeResult TryResumeHandle(HANDLE thread, DWORD& last_error) noexcept {
    last_error = ERROR_SUCCESS;
    if (thread == nullptr) {
        last_error = ERROR_INVALID_HANDLE;
        return ResumeResult::Failed;
    }

    for (unsigned int attempt = 0; attempt < kResumeAttempts; ++attempt) {
        if (ResumeThread(thread) != static_cast<DWORD>(-1)) {
            return ResumeResult::Resumed;
        }
        last_error = GetLastError();
        if (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0) {
            return ResumeResult::Exited;
        }
        if (attempt + 1 < kResumeAttempts) {
            SwitchToThread();
        }
    }
    return ResumeResult::Failed;
}

struct DeferredResume {
    HANDLE handle = nullptr;
    DWORD last_error = ERROR_SUCCESS;
};

class DeferredResumeQueue {
public:
    ~DeferredResumeQueue() noexcept {
        CloseEntries(entries_, count_);
        CloseEntries(emergency_entries_, emergency_count_);
        CloseEntries(overflow_entries_, overflow_count_);
    }

    bool Retry() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        // Do not short-circuit the emergency tranche when a primary entry is
        // still unresolved; every retained thread must get a retry attempt.
        const bool primary_complete = RetryEntries(entries_, count_);
        const bool emergency_complete = RetryEntries(emergency_entries_, emergency_count_);
        const bool overflow_complete = RetryEntries(overflow_entries_, overflow_count_);
        return primary_complete && emergency_complete && overflow_complete;
    }

    bool Reserve(std::size_t count) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t free_primary = entries_.size() - count_;
        const std::size_t free_emergency = emergency_entries_.size() - emergency_count_;
        const std::size_t free_overflow = overflow_entries_.size() - overflow_count_;
        const std::size_t available = free_primary + free_emergency + free_overflow;
        if (reserved_ > available || count > available - reserved_) {
            return false;
        }
        reserved_ += count;
        return true;
    }

    void ReleaseReservation(std::size_t count) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        reserved_ = count <= reserved_ ? reserved_ - count : 0;
    }

    bool EnqueueReserved(HANDLE handle, DWORD last_error) noexcept {
        if (handle == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (reserved_ == 0) {
            return false;
        }
        if (count_ < entries_.size()) {
            entries_[count_++] = DeferredResume{handle, last_error};
            --reserved_;
            return true;
        }
        // Keep a bounded emergency tranche for the one-transaction maximum
        // (1024 threads). This makes queue exhaustion a genuine process-wide
        // invariant violation rather than silently orphaning a suspended
        // handle after a few consecutive failed transactions.
        if (emergency_count_ >= emergency_entries_.size()) {
            if (overflow_count_ >= overflow_entries_.size()) {
                return false;
            }
            overflow_entries_[overflow_count_++] = DeferredResume{handle, last_error};
            --reserved_;
            return true;
        }
        emergency_entries_[emergency_count_++] = DeferredResume{handle, last_error};
        --reserved_;
        return true;
    }

    bool empty() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_ == 0 && emergency_count_ == 0 && overflow_count_ == 0;
    }

private:
    template <std::size_t Size>
    static void CloseEntries(std::array<DeferredResume, Size>& entries,
                             std::size_t count) noexcept {
        for (std::size_t index = 0; index < count; ++index) {
            if (entries[index].handle != nullptr) {
                CloseHandle(entries[index].handle);
                entries[index].handle = nullptr;
            }
        }
    }

    template <std::size_t Size>
    static bool RetryEntries(std::array<DeferredResume, Size>& entries,
                             std::size_t& count) noexcept {
        bool complete = true;
        for (std::size_t index = 0; index < count;) {
            DeferredResume& entry = entries[index];
            const ResumeResult result = TryResumeHandle(entry.handle, entry.last_error);
            if (result == ResumeResult::Failed) {
                complete = false;
                ++index;
                continue;
            }
            CloseHandle(entry.handle);
            --count;
            if (index != count) {
                entry = entries[count];
            }
        }
        return complete;
    }

    std::mutex mutex_;
    std::array<DeferredResume, kMaximumDeferredResumes> entries_{};
    std::array<DeferredResume, kMaximumEmergencyResumes> emergency_entries_{};
    std::array<DeferredResume, kMaximumOverflowResumes> overflow_entries_{};
    std::size_t count_ = 0;
    std::size_t emergency_count_ = 0;
    std::size_t overflow_count_ = 0;
    std::size_t reserved_ = 0;
};

DeferredResumeQueue& PendingResumes() noexcept {
    static DeferredResumeQueue queue;
    return queue;
}

std::atomic<bool> g_deferred_resume_worker_running = false;

DWORD WINAPI DeferredResumeWorker(void*) noexcept {
    unsigned int retry_count = 0;
    for (;;) {
        const bool complete = PendingResumes().Retry();
        if (complete && PendingResumes().empty()) {
            // Close the enqueue-vs-exit race: publish idle, then recheck.  A
            // concurrent handoff either starts a new worker or is observed and
            // reclaimed by this one, so no suspended handle is left waiting
            // for a hypothetical later patch transaction.
            g_deferred_resume_worker_running.store(false, std::memory_order_release);
            if (PendingResumes().empty()) {
                return 0;
            }
            bool expected = false;
            if (!g_deferred_resume_worker_running.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return 0;
            }
            retry_count = 0;
            continue;
        }

        ++retry_count;
        Sleep(retry_count < 128 ? 1 : 10);
    }
}

void EnsureDeferredResumeWorker() noexcept {
    bool expected = false;
    if (!g_deferred_resume_worker_running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }

    HANDLE worker = CreateThread(nullptr, 0, &DeferredResumeWorker, nullptr, 0, nullptr);
    if (worker == nullptr) {
        OutputDebugStringA(
            "SPatch runtime_patch: deferred resume worker creation failed; retrying synchronously\n");
        // Returning would orphan a suspended thread when this was the final
        // patch transaction.  The current patch thread is the safe fallback
        // owner and cannot continue until every handed-off thread is resumed.
        (void)DeferredResumeWorker(nullptr);
        return;
    }
    CloseHandle(worker);
}

class OtherThreadFreeze {
public:
    explicit OtherThreadFreeze(bool report_failures = true) noexcept
        : report_failures_(report_failures) {}

    ~OtherThreadFreeze() {
        bool thawed = Thaw();
        // ResumeThread normally cannot fail for a live handle opened with
        // THREAD_SUSPEND_RESUME, but a thread can exit or its handle can race
        // teardown while a patch transaction is unwinding. Give the kernel a
        // few more chances before releasing ownership, and make any residual
        // failure explicit instead of silently dropping a suspended thread.
        for (unsigned int attempt = 0; !thawed && attempt < 4; ++attempt) {
            Sleep(1);
            thawed = Thaw();
        }
        if (!thawed) {
            std::size_t deferred = 0;
            for (std::size_t index = 0; index < thread_count_; ++index) {
                if (!suspended_[index]) {
                    continue;
                }
                if (PendingResumes().EnqueueReserved(threads_[index], thaw_errors_[index])) {
                    threads_[index] = nullptr;
                    suspended_[index] = false;
                    --resume_reservation_;
                    ++deferred;
                } else {
                    // Freeze reserves enough queue capacity before suspending
                    // any thread, so this is an internal invariant failure.
                    // Never discard the only owned handle: keep retrying the
                    // resume/handoff until one succeeds rather than returning
                    // to the game with a permanently suspended thread.
                    if (report_failures_) {
                        OutputDebugStringA(
                            "SPatch runtime_patch: reserved thread thaw handoff failed\n");
                    }
                    for (;;) {
                        const ResumeResult result =
                            TryResumeHandle(threads_[index], thaw_errors_[index]);
                        if (result != ResumeResult::Failed) {
                            suspended_[index] = false;
                            break;
                        }
                        (void)PendingResumes().Retry();
                        if (PendingResumes().EnqueueReserved(
                                threads_[index], thaw_errors_[index])) {
                            threads_[index] = nullptr;
                            suspended_[index] = false;
                            --resume_reservation_;
                            ++deferred;
                            break;
                        }
                        Sleep(1);
                    }
                }
            }
            if (deferred != 0) {
                EnsureDeferredResumeWorker();
            }
            if (report_failures_) {
                (void)deferred;
                OutputDebugStringA(
                    "SPatch runtime_patch: thread thaw incomplete after final retry\n");
            }
        }
        if (resume_reservation_ != 0) {
            PendingResumes().ReleaseReservation(resume_reservation_);
            resume_reservation_ = 0;
        }
        for (std::size_t index = 0; index < thread_count_; ++index) {
            if (threads_[index] != nullptr) {
                CloseHandle(threads_[index]);
                threads_[index] = nullptr;
            }
        }
    }

    bool Freeze() {
        if (!PendingResumes().Retry()) {
            if (report_failures_) {
                OutputDebugStringA(
                    "SPatch runtime_patch: pending thread thaw unresolved; refusing mutation\n");
            }
            return false;
        }
        if (!PendingResumes().Reserve(kMaximumFrozenThreads)) {
            if (report_failures_) {
                OutputDebugStringA(
                    "SPatch runtime_patch: unable to reserve thread thaw capacity\n");
            }
            return false;
        }
        resume_reservation_ = kMaximumFrozenThreads;
        const DWORD process_id = GetCurrentProcessId();
        const DWORD current_thread_id = GetCurrentThreadId();

        // Repeat snapshots until every non-current thread is already held.
        // An entry that exits or has its ID recycled before suspension may
        // have created a successor after this fixed snapshot was captured, so
        // that race also forces another pass even when no handle was added.
        for (std::size_t pass = 0; pass < kMaximumSnapshotPasses; ++pass) {
            PurgeExitedThreads();
            bool added_thread = false;
            bool observed_exit_or_recycle_race = false;
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE) {
                return false;
            }

            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            if (!Thread32First(snapshot, &entry)) {
                CloseHandle(snapshot);
                return false;
            }

            do {
                if (entry.th32OwnerProcessID == process_id &&
                    entry.th32ThreadID != current_thread_id &&
                    !ContainsThread(entry.th32ThreadID)) {
                    if (thread_count_ >= kMaximumThreads) {
                        CloseHandle(snapshot);
                        return false;
                    }

                    HANDLE thread = OpenThread(
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                            THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                        FALSE,
                        entry.th32ThreadID);
                    if (thread == nullptr) {
                        if (GetLastError() == ERROR_INVALID_PARAMETER) {
                            observed_exit_or_recycle_race = true;
                            entry.dwSize = sizeof(entry);
                            continue;  // Exited after the snapshot.
                        }
                        CloseHandle(snapshot);
                        return false;
                    }

                    // A recycled thread ID must never let us suspend a thread
                    // owned by another process.
                    const DWORD thread_process_id = GetProcessIdOfThread(thread);
                    if (thread_process_id == 0) {
                        CloseHandle(thread);
                        CloseHandle(snapshot);
                        return false;
                    }
                    if (thread_process_id != process_id) {
                        CloseHandle(thread);
                        observed_exit_or_recycle_race = true;
                        entry.dwSize = sizeof(entry);
                        continue;
                    }

                    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
                        if (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0) {
                            CloseHandle(thread);
                            observed_exit_or_recycle_race = true;
                            entry.dwSize = sizeof(entry);
                            continue;
                        }
                        CloseHandle(thread);
                        CloseHandle(snapshot);
                        return false;
                    }

                    thread_ids_[thread_count_] = entry.th32ThreadID;
                    threads_[thread_count_] = thread;
                    suspended_[thread_count_] = true;
                    ++thread_count_;
                    added_thread = true;
                }
                entry.dwSize = sizeof(entry);
            } while (Thread32Next(snapshot, &entry));

            const DWORD enumeration_error = GetLastError();
            CloseHandle(snapshot);
            if (enumeration_error != ERROR_NO_MORE_FILES) {
                return false;
            }
            if (!ThreadSnapshotPassNeedsRetry(
                    added_thread, observed_exit_or_recycle_race)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool Thaw() {
        bool complete = true;
        for (std::size_t index = thread_count_; index > 0; --index) {
            const std::size_t slot = index - 1;
            if (!suspended_[slot]) {
                continue;
            }
            const ResumeResult result = TryResumeHandle(threads_[slot], thaw_errors_[slot]);
            if (result != ResumeResult::Failed) {
                suspended_[slot] = false;
            } else {
                complete = false;
            }
        }
        return complete;
    }

    [[nodiscard]] bool IsRangeUnoccupied(std::uintptr_t address, std::size_t size) const {
        for (std::size_t index = 0; index < thread_count_; ++index) {
            if (!suspended_[index]) {
                continue;
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(threads_[index], &context)) {
                if (WaitForSingleObject(threads_[index], 0) == WAIT_OBJECT_0) {
                    continue;
                }
                return false;
            }
            if (IsAddressInPatchRange(
                    static_cast<std::uintptr_t>(context.Rip), address, size)) {
                return false;
            }
        }
        return true;
    }

private:
    static constexpr std::size_t kMaximumThreads = kMaximumFrozenThreads;
    static constexpr std::size_t kMaximumSnapshotPasses = 8;

    [[nodiscard]] bool ContainsThread(DWORD thread_id) const {
        return std::any_of(thread_ids_.begin(),
                           thread_ids_.begin() + static_cast<std::ptrdiff_t>(thread_count_),
                           [thread_id](DWORD candidate) { return candidate == thread_id; });
    }

    void PurgeExitedThreads() {
        for (std::size_t index = 0; index < thread_count_;) {
            if (WaitForSingleObject(threads_[index], 0) != WAIT_OBJECT_0) {
                ++index;
                continue;
            }
            CloseHandle(threads_[index]);
            --thread_count_;
            if (index != thread_count_) {
                threads_[index] = threads_[thread_count_];
                thread_ids_[index] = thread_ids_[thread_count_];
                suspended_[index] = suspended_[thread_count_];
                thaw_errors_[index] = thaw_errors_[thread_count_];
            }
        }
    }

    std::array<HANDLE, kMaximumThreads> threads_{};
    std::array<DWORD, kMaximumThreads> thread_ids_{};
    std::array<bool, kMaximumThreads> suspended_{};
    std::array<DWORD, kMaximumThreads> thaw_errors_{};
    std::size_t thread_count_ = 0;
    std::size_t resume_reservation_ = 0;
    bool report_failures_ = true;
};

enum class WriteOutcome {
    Succeeded,
    FailedNoMutation,
    FailedMutationPossible,
};

struct WriteResult {
    WriteOutcome outcome = WriteOutcome::FailedNoMutation;
    bool protection_restore_pending = false;
    DWORD original_protection = 0;
};

constexpr unsigned int kProtectionRestoreAttempts = 8;

bool QueryRangeProtection(std::uintptr_t address,
                          std::size_t size,
                          DWORD& protection) noexcept {
    protection = 0;
    if (address == 0 || size == 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) !=
            sizeof(region) ||
        region.State != MEM_COMMIT || region.BaseAddress == nullptr) {
        return false;
    }
    const std::uintptr_t region_base =
        reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (address < region_base) {
        return false;
    }
    const std::size_t offset = static_cast<std::size_t>(address - region_base);
    if (offset > region.RegionSize || size > region.RegionSize - offset) {
        return false;
    }
    protection = region.Protect;
    return true;
}

// g_patch_transaction_mutex must be held.  Restore only the RWX state that
// this transaction installed.  If another writer changed the protection, do
// not overwrite its state; retain the uncertainty record for a later retry.
bool TryRestoreOwnedPageProtectionLocked(std::uintptr_t address,
                                         std::size_t size,
                                         DWORD original_protection) noexcept {
    if (original_protection == 0) {
        return false;
    }
    for (unsigned int attempt = 0; attempt < kProtectionRestoreAttempts; ++attempt) {
        DWORD current_protection = 0;
        if (!QueryRangeProtection(address, size, current_protection)) {
            return false;
        }
        if (current_protection == original_protection) {
            return true;
        }
        if (current_protection != PAGE_EXECUTE_READWRITE) {
            return false;
        }

        DWORD previous_protection = 0;
        if (VirtualProtect(reinterpret_cast<void*>(address),
                           size,
                           original_protection,
                           &previous_protection)) {
            if (previous_protection == PAGE_EXECUTE_READWRITE ||
                previous_protection == original_protection) {
                return true;
            }

            // An external protection write raced the query. Put its actual
            // state back and keep our record rather than claiming ownership of
            // a transition that is no longer identifiable as ours.
            DWORD ignored = 0;
            (void)VirtualProtect(reinterpret_cast<void*>(address),
                                 size,
                                 previous_protection,
                                 &ignored);
            return false;
        }
        if (attempt + 1 < kProtectionRestoreAttempts) {
            SwitchToThread();
        }
    }
    return false;
}

bool RetryOwnedPageProtection(std::uintptr_t address,
                              std::size_t size,
                              DWORD original_protection) noexcept {
    std::lock_guard<std::mutex> transaction_lock(g_patch_transaction_mutex);
    return TryRestoreOwnedPageProtectionLocked(address, size, original_protection);
}

WriteResult WriteProcessBytes(std::uintptr_t address,
                              std::span<const std::uint8_t> bytes,
                              std::span<const std::uint8_t> rollback_bytes) {
    if (address == 0 || bytes.empty() || bytes.size() != rollback_bytes.size()) {
        return {WriteOutcome::FailedNoMutation};
    }

    std::lock_guard<std::mutex> transaction_lock(g_patch_transaction_mutex);

    // The freeze object owns four 1024-entry bookkeeping arrays. Keep that
    // state off the patch caller's stack (and preserve deterministic capacity)
    // instead of replacing it with vectors that could allocate after threads
    // have already been suspended.
    std::unique_ptr<OtherThreadFreeze> freeze(new (std::nothrow) OtherThreadFreeze());
    if (!freeze) {
        log::Error("runtime_patch thread freeze allocation failed");
        return {WriteOutcome::FailedNoMutation};
    }
    if (!freeze->Freeze()) {
        // Freeze can fail after suspending a subset of the process.  Surface
        // an incomplete thaw as an owned transaction failure instead of
        // letting Registry::Apply discard its rollback record.
        const bool resumed_all_threads = freeze->Thaw();
        return {resumed_all_threads ? WriteOutcome::FailedNoMutation
                                    : WriteOutcome::FailedMutationPossible};
    }
    // A stable suspension alone is insufficient: resuming a thread whose RIP
    // was inside the overwritten instruction span would continue in the middle
    // of old/new bytes. Abort without mutation and let a later startup/retry
    // attempt run after that instruction has retired.
    // A suspended thread may have its RIP on an instruction that starts just
    // before the patch and crosses into it. Guard one full maximum x86-64
    // instruction behind the site as well as the bytes being replaced; only
    // checking RIP inside the patch span leaves that crossing instruction
    // vulnerable to a torn resume.
    constexpr std::size_t kMaximumInstructionLength = 15;
    const std::uintptr_t occupied_start =
        address >= kMaximumInstructionLength ? address - kMaximumInstructionLength : 0;
    const std::size_t occupied_prefix = address - occupied_start;
    if (bytes.size() > (std::numeric_limits<std::size_t>::max)() - occupied_prefix ||
        !freeze->IsRangeUnoccupied(occupied_start, occupied_prefix + bytes.size())) {
        const bool resumed_all_threads = freeze->Thaw();
        return {resumed_all_threads ? WriteOutcome::FailedNoMutation
                                    : WriteOutcome::FailedMutationPossible};
    }

    bool write_succeeded = false;
    bool mutation_attempted = false;
    bool rollback_verified = true;
    bool protection_restored = true;
    DWORD old_protection = 0;
    bool protection_changed = false;

    // Revalidate after the stable freeze. The earlier classification is only
    // a fast fail; this comparison owns the actual write decision and prevents
    // a concurrent mod from being overwritten between check and commit.
    if (SafeBytesEqual(address, rollback_bytes) &&
        VirtualProtect(reinterpret_cast<void*>(address),
                       bytes.size(),
                       PAGE_EXECUTE_READWRITE,
                       &old_protection)) {
        protection_changed = true;
        protection_restored = false;
        mutation_attempted = true;
        if (SafeCopyToProcess(address, bytes.data(), bytes.size()) &&
            FlushInstructionCache(
                GetCurrentProcess(), reinterpret_cast<void*>(address), bytes.size())) {
            if (TryRestoreOwnedPageProtectionLocked(
                    address, bytes.size(), old_protection)) {
                protection_changed = false;
                protection_restored = true;
                write_succeeded = true;
            }
        }
    }

    if (!write_succeeded && protection_changed) {
        // SafeCopyToProcess converts any access violation into a normal failure
        // so the freeze object always gets a chance to resume every thread.
        const bool rollback_written =
            SafeCopyToProcess(address, rollback_bytes.data(), rollback_bytes.size());
        const bool rollback_flushed =
            rollback_written &&
            FlushInstructionCache(
                GetCurrentProcess(), reinterpret_cast<void*>(address), rollback_bytes.size());
        protection_restored = TryRestoreOwnedPageProtectionLocked(
            address, bytes.size(), old_protection);
        if (protection_restored) {
            protection_changed = false;
        }
        rollback_verified =
            rollback_flushed && SafeBytesEqual(address, rollback_bytes);
    }

    const bool resumed_all_threads = freeze->Thaw();
    if (!resumed_all_threads) {
        OutputDebugStringA(
            "SPatch runtime_patch: thread thaw incomplete; patch ownership retained\n");
    }
    if (write_succeeded) {
        return {resumed_all_threads ? WriteOutcome::Succeeded
                                    : WriteOutcome::FailedMutationPossible};
    }
    if (resumed_all_threads && (!mutation_attempted ||
                                (rollback_verified && protection_restored))) {
        return {WriteOutcome::FailedNoMutation};
    }
    return {WriteOutcome::FailedMutationPossible,
            protection_changed && !protection_restored,
            protection_changed && !protection_restored ? old_protection : 0};
}

}  // namespace

bool IsAddressInPatchRange(std::uintptr_t instruction_pointer,
                           std::uintptr_t patch_address,
                           std::size_t patch_size) noexcept {
    return patch_size != 0 && instruction_pointer >= patch_address &&
           (instruction_pointer - patch_address) < patch_size;
}

ByteState ClassifyBytes(std::span<const std::uint8_t> current,
                        std::span<const std::uint8_t> expected,
                        std::span<const std::uint8_t> replacement) {
    if (current.size() != expected.size() || current.size() != replacement.size()) {
        return ByteState::Unexpected;
    }
    if (std::equal(current.begin(), current.end(), expected.begin())) {
        return ByteState::Expected;
    }
    if (std::equal(current.begin(), current.end(), replacement.begin())) {
        return ByteState::Replacement;
    }
    return ByteState::Unexpected;
}

const char* ApplyResultName(ApplyResult result) {
    switch (result) {
        case ApplyResult::Applied:
            return "applied";
        case ApplyResult::AlreadyApplied:
            return "already_applied";
        case ApplyResult::InvalidRequest:
            return "invalid_request";
        case ApplyResult::AllocationFailed:
            return "allocation_failed";
        case ApplyResult::ReadFailed:
            return "read_failed";
        case ApplyResult::UnexpectedBytes:
            return "unexpected_bytes";
        case ApplyResult::WriteFailed:
            return "write_failed";
    }
    return "unknown";
}

ApplyResult Registry::Apply(std::string name,
                            std::uintptr_t address,
                            std::span<const std::uint8_t> expected,
                            std::span<const std::uint8_t> replacement) {
    if (name.empty() || address == 0 || expected.empty() ||
        expected.size() != replacement.size()) {
        return ApplyResult::InvalidRequest;
    }

    std::vector<std::uint8_t> current;
    try {
        current.resize(expected.size());
    } catch (const std::bad_alloc&) {
        return ApplyResult::AllocationFailed;
    }
    if (!SafeCopyFromProcess(address, current.data(), current.size())) {
        return ApplyResult::ReadFailed;
    }

    switch (ClassifyBytes(current, expected, replacement)) {
        case ByteState::Replacement:
            return ApplyResult::AlreadyApplied;
        case ByteState::Unexpected:
            return ApplyResult::UnexpectedBytes;
        case ByteState::Expected:
            break;
    }

    AppliedPatch pending;
    try {
        pending = AppliedPatch{std::move(name),
                               address,
                               std::vector<std::uint8_t>(expected.begin(), expected.end()),
                               std::vector<std::uint8_t>(replacement.begin(), replacement.end())};
        applied_.reserve(applied_.size() + 1);
    } catch (const std::bad_alloc&) {
        return ApplyResult::AllocationFailed;
    }
    static_assert(std::is_nothrow_move_constructible_v<AppliedPatch>);

    // Commit ownership before mutation. A rare write/resume failure therefore
    // leaves a conservative rollback record instead of unowned modified code.
    applied_.push_back(std::move(pending));
    const WriteResult write_result = WriteProcessBytes(address, replacement, expected);
    if (write_result.outcome == WriteOutcome::FailedNoMutation) {
        applied_.pop_back();
        return ApplyResult::WriteFailed;
    }
    if (write_result.outcome == WriteOutcome::FailedMutationPossible) {
        applied_.back().mutation_uncertain = true;
        applied_.back().protection_restore_pending =
            write_result.protection_restore_pending;
        applied_.back().original_protection = write_result.original_protection;
        return ApplyResult::WriteFailed;
    }

    return ApplyResult::Applied;
}

std::size_t Registry::checkpoint() const noexcept {
    return applied_.size();
}

bool Registry::RestoreTo(std::size_t checkpoint) {
    if (checkpoint > applied_.size()) {
        return false;
    }
    // A previous write may have left a thread handle in the deferred-resume
    // queue. Retry before treating an already-stock byte range as fully
    // restored; otherwise shutdown could erase the rollback record while the
    // process still owns a suspended thread.
    bool pending_resumes_complete = PendingResumes().Retry();
    for (std::size_t index = applied_.size(); index > checkpoint; --index) {
        AppliedPatch& patch = applied_[index - 1];
        if (patch.protection_restore_pending) {
            if (!RetryOwnedPageProtection(patch.address,
                                          patch.replacement.size(),
                                          static_cast<DWORD>(patch.original_protection))) {
                log::WarnF("runtime_patch restore deferred name=%s "
                           "reason=page_protection_pending target=0x%p",
                           patch.name.c_str(),
                           reinterpret_cast<void*>(patch.address));
                continue;
            }
            patch.protection_restore_pending = false;
            patch.original_protection = 0;
        }
        std::vector<std::uint8_t> current;
        try {
            current.resize(patch.replacement.size());
        } catch (const std::bad_alloc&) {
            log::WarnF("runtime_patch restore allocation failed name=%s target=0x%p",
                       patch.name.c_str(),
                       reinterpret_cast<void*>(patch.address));
            continue;
        }
        if (!SafeCopyFromProcess(patch.address, current.data(), current.size())) {
            log::WarnF("runtime_patch restore read failed name=%s target=0x%p",
                       patch.name.c_str(),
                       reinterpret_cast<void*>(patch.address));
            continue;
        }

        if (std::equal(current.begin(), current.end(), patch.expected.begin())) {
            if (!pending_resumes_complete || !PendingResumes().empty()) {
                pending_resumes_complete = PendingResumes().Retry();
            }
            if (!pending_resumes_complete || !PendingResumes().empty()) {
                log::WarnF("runtime_patch restore deferred name=%s reason=thread_thaw_pending "
                           "target=0x%p",
                           patch.name.c_str(),
                           reinterpret_cast<void*>(patch.address));
                continue;
            }
            applied_.erase(applied_.begin() + static_cast<std::ptrdiff_t>(index - 1));
            continue;
        }
        if (!std::equal(current.begin(), current.end(), patch.replacement.begin())) {
            log::WarnF("runtime_patch restore skipped name=%s reason=bytes_changed target=0x%p",
                       patch.name.c_str(),
                       reinterpret_cast<void*>(patch.address));
            if (!patch.mutation_uncertain) {
                // Another writer owns the live bytes now. Relinquish our
                // rollback record rather than later restoring over that mod.
                applied_.erase(applied_.begin() + static_cast<std::ptrdiff_t>(index - 1));
            }
            continue;
        }

        const WriteResult restore_result =
            WriteProcessBytes(patch.address, patch.expected, patch.replacement);
        if (restore_result.outcome != WriteOutcome::Succeeded) {
            if (restore_result.outcome == WriteOutcome::FailedMutationPossible) {
                patch.mutation_uncertain = true;
                if (restore_result.protection_restore_pending) {
                    patch.protection_restore_pending = true;
                    patch.original_protection = restore_result.original_protection;
                }
            }
            log::WarnF("runtime_patch restore failed name=%s target=0x%p",
                       patch.name.c_str(),
                       reinterpret_cast<void*>(patch.address));
            continue;
        }
        applied_.erase(applied_.begin() + static_cast<std::ptrdiff_t>(index - 1));
    }
    pending_resumes_complete = PendingResumes().Retry();
    return applied_.size() == checkpoint && pending_resumes_complete &&
           PendingResumes().empty();
}

bool Registry::RestoreAll() {
    return RestoreTo(0);
}

bool Registry::empty() const {
    return applied_.empty();
}

bool MatchesBytes(std::uintptr_t address, std::span<const std::uint8_t> expected) {
    if (address == 0 || expected.empty()) {
        return false;
    }
    std::vector<std::uint8_t> current;
    try {
        current.resize(expected.size());
    } catch (const std::bad_alloc&) {
        return false;
    }
    return SafeCopyFromProcess(address, current.data(), current.size()) &&
           std::equal(current.begin(), current.end(), expected.begin());
}

}  // namespace spatch::runtime_patch
