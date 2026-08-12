#pragma once

#include <Windows.h>

#include <array>
#include <cwchar>
#include <filesystem>
#include <string>
#include <utility>

namespace spatch {

// Resolve Windows components from the operating-system directory explicitly.
// A bare LoadLibraryW name would search the game directory and could bind a
// user-provided DLL instead of the system dependency SPatch was built for.
inline HMODULE LoadSystemLibrary(const wchar_t* library_name) noexcept {
    try {
        if (library_name == nullptr || library_name[0] == L'\0' ||
            std::wcschr(library_name, L'\\') != nullptr ||
            std::wcschr(library_name, L'/') != nullptr ||
            std::wcschr(library_name, L':') != nullptr) {
            return nullptr;
        }

        std::array<wchar_t, MAX_PATH> stack_buffer{};
        const UINT initial_length = GetSystemDirectoryW(
            stack_buffer.data(), static_cast<UINT>(stack_buffer.size()));
        if (initial_length == 0) {
            return nullptr;
        }

        std::wstring system_directory;
        if (initial_length < stack_buffer.size()) {
            system_directory.assign(stack_buffer.data(), initial_length);
        } else {
            // GetSystemDirectoryW reports the required size, including the
            // terminator, when the supplied buffer is too small.
            if (initial_length >= 32768) {
                return nullptr;
            }
            std::wstring dynamic_buffer(initial_length + 1, L'\0');
            const UINT actual_length = GetSystemDirectoryW(
                dynamic_buffer.data(), static_cast<UINT>(dynamic_buffer.size()));
            if (actual_length == 0 || actual_length >= dynamic_buffer.size()) {
                return nullptr;
            }
            dynamic_buffer.resize(actual_length);
            system_directory = std::move(dynamic_buffer);
        }

        if (system_directory.empty()) {
            return nullptr;
        }
        const wchar_t tail = system_directory.back();
        if (tail != L'\\' && tail != L'/') {
            system_directory.push_back(L'\\');
        }
        system_directory.append(library_name);
        HMODULE module = LoadLibraryW(system_directory.c_str());
        if (module == nullptr) {
            return nullptr;
        }

        // Windows may reuse an already loaded same-name module. Confirm that
        // the returned handle is the requested system file before exposing it
        // to callers.
        std::wstring loaded_path(32768, L'\0');
        const DWORD loaded_length = GetModuleFileNameW(
            module, loaded_path.data(), static_cast<DWORD>(loaded_path.size()));
        if (loaded_length == 0 || loaded_length >= loaded_path.size()) {
            FreeLibrary(module);
            return nullptr;
        }
        loaded_path.resize(loaded_length);

        std::error_code identity_error;
        const bool system_identity = std::filesystem::equivalent(
            std::filesystem::path(system_directory),
            std::filesystem::path(loaded_path),
            identity_error);
        if (identity_error || !system_identity) {
            FreeLibrary(module);
            return nullptr;
        }
        return module;
    } catch (...) {
        return nullptr;
    }
}

}  // namespace spatch
