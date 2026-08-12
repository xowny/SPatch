#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace spatch::qfile_io {

inline constexpr std::size_t kDeviceOffset = 0x10;
inline constexpr std::size_t kOperationCriticalSectionOffset = 0x1C;
inline constexpr std::size_t kNativeHandleOffset = 0x50;
inline constexpr std::size_t kSeekVtableSlot = 23;
inline constexpr std::size_t kReadVtableSlot = 24;
inline constexpr std::size_t kWriteVtableSlot = 25;
inline constexpr std::uint64_t kOperationFailure = (std::numeric_limits<std::uint64_t>::max)();

using QFileReadyFn = bool (*)(void* file);
using DeviceReadFn = std::uint64_t (*)(void* device, void* file, void* buffer,
                                       std::uint64_t byte_count);
using DeviceSeekFn = bool (*)(void* device, void* file, std::uint32_t origin, std::int64_t offset);
using DeviceWriteFn = std::uint64_t (*)(void* device, void* file, const void* buffer,
                                        std::uint64_t byte_count, bool* disk_full);

namespace detail {

// Engine QFile objects are opaque and can be freed by an asynchronous cancel
// path while one of these detours is still on the stack.  Keep all probing in
// tiny, non-inlined SEH boundaries.  The normal path still performs one plain
// memcpy and does not call VirtualQuery or allocate.
__declspec(noinline) inline bool CopyBytesSafely(std::uintptr_t address,
                                                 void* destination,
                                                 std::size_t size) noexcept {
    if (destination == nullptr || (address == 0 && size != 0)) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    __try {
        std::memcpy(destination, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
__declspec(noinline) inline bool WriteObjectSafely(T* destination,
                                                   const T& value) noexcept {
    if (destination == nullptr) {
        return false;
    }
    __try {
        *destination = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool AddOffset(std::uintptr_t base,
                      std::size_t offset,
                      std::uintptr_t& address) noexcept {
    if (base == 0 || offset > (std::numeric_limits<std::uintptr_t>::max)() - base) {
        address = 0;
        return false;
    }
    address = base + offset;
    return true;
}

template <typename T>
inline bool ReadObjectAt(const void* base, std::size_t offset, T& value) noexcept {
    value = T{};
    std::uintptr_t address = 0;
    return AddOffset(reinterpret_cast<std::uintptr_t>(base), offset, address) &&
           CopyBytesSafely(address, &value, sizeof(value));
}

template <typename T>
inline bool ReadVtableEntry(void** vtable, std::size_t slot, T& value) noexcept {
    value = T{};
    if (slot > (std::numeric_limits<std::size_t>::max)() / sizeof(void*)) {
        return false;
    }
    return ReadObjectAt(vtable, slot * sizeof(void*), value);
}

__declspec(noinline) inline bool EnterCriticalSectionSafely(
    LPCRITICAL_SECTION critical_section) noexcept {
    if (critical_section == nullptr) {
        return false;
    }
    __try {
        EnterCriticalSection(critical_section);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) inline void LeaveCriticalSectionSafely(
    LPCRITICAL_SECTION critical_section) noexcept {
    if (critical_section == nullptr) {
        return;
    }
    __try {
        LeaveCriticalSection(critical_section);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A cancelled QFile may have released its embedded lock.  There is no
        // safe recovery operation here; the detour has already failed closed.
    }
}

__declspec(noinline) inline bool InvokeReadySafely(QFileReadyFn ready,
                                                   void* file) noexcept {
    if (ready == nullptr || file == nullptr) {
        return false;
    }
    __try {
        return ready(file);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) inline bool InvokeSeekSafely(DeviceSeekFn seek,
                                                  void* device,
                                                  void* file,
                                                  std::uint32_t origin,
                                                  std::int64_t offset) noexcept {
    if (seek == nullptr || device == nullptr || file == nullptr) {
        return false;
    }
    __try {
        return seek(device, file, origin, offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) inline std::uint64_t InvokeReadSafely(DeviceReadFn read,
                                                           void* device,
                                                           void* file,
                                                           void* buffer,
                                                           std::uint64_t byte_count) noexcept {
    if (read == nullptr || device == nullptr || file == nullptr || buffer == nullptr) {
        return kOperationFailure;
    }
    __try {
        return read(device, file, buffer, byte_count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kOperationFailure;
    }
}

__declspec(noinline) inline std::uint64_t InvokeWriteSafely(
    DeviceWriteFn write,
    void* device,
    void* file,
    const void* buffer,
    std::uint64_t byte_count,
    bool* disk_full) noexcept {
    if (write == nullptr || device == nullptr || file == nullptr || buffer == nullptr ||
        disk_full == nullptr) {
        return kOperationFailure;
    }
    __try {
        return write(device, file, buffer, byte_count, disk_full);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *disk_full = false;
        return kOperationFailure;
    }
}

}  // namespace detail

inline HANDLE NativeHandle(void* file) noexcept {
    HANDLE handle = nullptr;
    detail::ReadObjectAt(file, kNativeHandleOffset, handle);
    return handle;
}

inline void* FileDevice(void* file) noexcept {
    void* device = nullptr;
    detail::ReadObjectAt(file, kDeviceOffset, device);
    return device;
}

inline bool IsUsableHandle(HANDLE handle) noexcept {
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

inline std::uint64_t ReadFileDevice(void*, void* file, void* buffer,
                                    std::uint64_t byte_count) noexcept {
    const HANDLE handle = NativeHandle(file);
    if (!IsUsableHandle(handle) || buffer == nullptr ||
        byte_count > (std::numeric_limits<DWORD>::max)()) {
        return kOperationFailure;
    }
    if (byte_count == 0) {
        return 0;
    }

    DWORD bytes_read = 0;
    if (ReadFile(handle, buffer, static_cast<DWORD>(byte_count), &bytes_read, nullptr) == FALSE ||
        bytes_read != static_cast<DWORD>(byte_count)) {
        return kOperationFailure;
    }
    return bytes_read;
}

inline bool SeekFileDevice(void*, void* file, std::uint32_t origin, std::int64_t offset) noexcept {
    const HANDLE handle = NativeHandle(file);
    if (!IsUsableHandle(handle)) {
        return false;
    }

    LARGE_INTEGER distance{};
    distance.QuadPart = offset;
    return SetFilePointerEx(handle, distance, nullptr, static_cast<DWORD>(origin)) != FALSE;
}

inline std::uint64_t TellFileDevice(void*, void* file) noexcept {
    const HANDLE handle = NativeHandle(file);
    if (!IsUsableHandle(handle)) {
        return kOperationFailure;
    }

    LARGE_INTEGER distance{};
    LARGE_INTEGER position{};
    if (SetFilePointerEx(handle, distance, &position, FILE_CURRENT) == FALSE) {
        return kOperationFailure;
    }
    return static_cast<std::uint64_t>(position.QuadPart);
}

inline std::uint64_t SizeFileDevice(void*, void* file) noexcept {
    const HANDLE handle = NativeHandle(file);
    if (!IsUsableHandle(handle)) {
        return kOperationFailure;
    }

    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle, &size) == FALSE || size.QuadPart < 0) {
        return kOperationFailure;
    }
    return static_cast<std::uint64_t>(size.QuadPart);
}

class OperationLock final {
public:
    explicit OperationLock(void* file) noexcept {
        std::uintptr_t address = 0;
        if (!detail::AddOffset(reinterpret_cast<std::uintptr_t>(file),
                               kOperationCriticalSectionOffset,
                               address)) {
            return;
        }
        critical_section_ = reinterpret_cast<LPCRITICAL_SECTION>(address);
        acquired_ = detail::EnterCriticalSectionSafely(critical_section_);
        if (!acquired_) {
            critical_section_ = nullptr;
        }
    }

    ~OperationLock() noexcept {
        if (acquired_) {
            detail::LeaveCriticalSectionSafely(critical_section_);
        }
    }

    [[nodiscard]] bool acquired() const noexcept { return acquired_; }

    OperationLock(const OperationLock&) = delete;
    OperationLock& operator=(const OperationLock&) = delete;

private:
    LPCRITICAL_SECTION critical_section_ = nullptr;
    bool acquired_ = false;
};

inline void** DeviceVtable(void* device) noexcept {
    void** vtable = nullptr;
    detail::ReadObjectAt(device, 0, vtable);
    return vtable;
}

inline void* DeviceFunction(void** vtable, std::size_t slot) noexcept {
    void* function = nullptr;
    detail::ReadVtableEntry(vtable, slot, function);
    return function;
}

inline std::uint64_t ReadAt(QFileReadyFn ready, void* file, void* buffer, std::uint64_t byte_count,
                            std::int64_t offset, std::uint32_t origin) noexcept {
    if (!detail::InvokeReadySafely(ready, file) || buffer == nullptr) {
        return kOperationFailure;
    }
    if (byte_count == 0) {
        return 0;
    }

    void* const device = FileDevice(file);
    void** const vtable = DeviceVtable(device);
    const auto seek = reinterpret_cast<DeviceSeekFn>(DeviceFunction(vtable, kSeekVtableSlot));
    const auto read = reinterpret_cast<DeviceReadFn>(DeviceFunction(vtable, kReadVtableSlot));
    if (seek == nullptr || read == nullptr) {
        return kOperationFailure;
    }

    OperationLock lock(file);
    if (!lock.acquired() || !detail::InvokeSeekSafely(seek, device, file, origin, offset)) {
        return kOperationFailure;
    }
    return detail::InvokeReadSafely(read, device, file, buffer, byte_count);
}

inline std::uint64_t WriteAt(QFileReadyFn ready, void* file, const void* buffer,
                             std::uint64_t byte_count, std::int64_t offset, std::uint32_t origin,
                             bool* disk_full) noexcept {
    (void)detail::WriteObjectSafely(disk_full, false);
    if (!detail::InvokeReadySafely(ready, file) || buffer == nullptr) {
        return kOperationFailure;
    }
    if (byte_count == 0) {
        return 0;
    }

    void* const device = FileDevice(file);
    void** const vtable = DeviceVtable(device);
    const auto seek = reinterpret_cast<DeviceSeekFn>(DeviceFunction(vtable, kSeekVtableSlot));
    const auto write = reinterpret_cast<DeviceWriteFn>(DeviceFunction(vtable, kWriteVtableSlot));
    if (seek == nullptr || write == nullptr) {
        return kOperationFailure;
    }

    bool local_disk_full = false;
    std::uint64_t result = kOperationFailure;
    {
        OperationLock lock(file);
        if (lock.acquired() && detail::InvokeSeekSafely(seek, device, file, origin, offset)) {
            result = detail::InvokeWriteSafely(
                write, device, file, buffer, byte_count, &local_disk_full);
        }
    }
    (void)detail::WriteObjectSafely(disk_full, local_disk_full);
    return result;
}

}  // namespace spatch::qfile_io
