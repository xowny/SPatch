#include "HookTargetGuardTests.h"

#include "../src/CharacterHookPolicy.h"
#include "../src/HookTargetGuard.h"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <vector>

namespace spatch::tests {
namespace {

constexpr std::uint64_t kPreferredBase = 0x0000000140000000ULL;

bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: hook target guard: " << message << '\n';
        return false;
    }
    return true;
}

bool RunCharacterHookPlanTests() {
    using character_hooks::BuildHookPlan;

    constexpr character_hooks::HookPlan disabled = BuildHookPlan({});
    if (!Expect(!disabled.install_effects_hooks,
                "disabled character features should not install effects hooks") ||
        !Expect(!disabled.install_health_damage_hook,
                "disabled character features should not install the health hook") ||
        !Expect(!disabled.require_simobject_component,
                "disabled character features should not require component lookup") ||
        !Expect(!disabled.use_sweat_only_health_fast_path,
                "disabled character features should not select a health fast path")) {
        return false;
    }

    constexpr character_hooks::HookPlan sweat_only = BuildHookPlan({
        .restore_sweat = true,
    });
    if (!Expect(sweat_only.install_effects_hooks,
                "sweat-only plan should install character-effects hooks") ||
        !Expect(sweat_only.install_health_damage_hook,
                "sweat-only plan should install the health-damage hook") ||
        !Expect(!sweat_only.install_regression_hooks,
                "sweat-only plan should not install regression hooks") ||
        !Expect(sweat_only.require_simobject_component,
                "sweat-only plan should require component lookup") ||
        !Expect(sweat_only.use_sweat_only_health_fast_path,
                "sweat-only plan should select the minimal health fast path")) {
        return false;
    }

    constexpr character_hooks::HookPlan wetness_only = BuildHookPlan({
        .restore_wetness = true,
    });
    if (!Expect(wetness_only.install_effects_hooks,
                "wetness-only plan should install character-effects hooks") ||
        !Expect(!wetness_only.install_health_damage_hook,
                "wetness-only plan should not install the health-damage hook") ||
        !Expect(wetness_only.require_simobject_component,
                "wetness-only plan should require component lookup") ||
        !Expect(!wetness_only.use_sweat_only_health_fast_path,
                "wetness-only plan should not select a health fast path")) {
        return false;
    }

    // This is the shipped default combination. Sweat still needs the health
    // signal, while wetness shares the character-effects hooks.
    constexpr character_hooks::HookPlan shipped_defaults = BuildHookPlan({
        .restore_wetness = true,
        .restore_sweat = true,
    });
    if (!Expect(shipped_defaults.install_effects_hooks,
                "default plan should install character-effects hooks") ||
        !Expect(shipped_defaults.install_health_damage_hook,
                "default plan should install the health-damage hook") ||
        !Expect(shipped_defaults.require_simobject_component,
                "default plan should require component lookup") ||
        !Expect(shipped_defaults.use_sweat_only_health_fast_path,
                "default plan should use the minimal health path")) {
        return false;
    }

    constexpr character_hooks::HookPlan regression = BuildHookPlan({
        .regression_probe = true,
    });
    return Expect(regression.install_effects_hooks,
                  "regression plan should install character-effects hooks") &&
           Expect(regression.install_health_damage_hook,
                  "regression plan should install the health-damage hook") &&
           Expect(regression.install_regression_hooks,
                  "regression plan should retain the full probe hook set") &&
           Expect(regression.require_simobject_component,
                  "regression plan should require component lookup") &&
           Expect(!regression.use_sweat_only_health_fast_path,
                  "regression plan should use the full diagnostic health path");
}

std::vector<std::uint8_t> MakeRelocatablePe() {
    std::vector<std::uint8_t> file(0x800);

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 2;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->FileHeader.TimeDateStamp = 0x12345678;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.ImageBase = kPreferredBase;
    nt->OptionalHeader.SectionAlignment = 0x1000;
    nt->OptionalHeader.FileAlignment = 0x200;
    nt->OptionalHeader.SizeOfImage = 0x3000;
    nt->OptionalHeader.SizeOfHeaders = 0x400;
    nt->OptionalHeader.AddressOfEntryPoint = 0x1000;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress = 0x2000;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = 12;

    auto* section = IMAGE_FIRST_SECTION(nt);
    std::memcpy(section[0].Name, ".text", 5);
    section[0].Misc.VirtualSize = 0x300;
    section[0].VirtualAddress = 0x1000;
    section[0].SizeOfRawData = 0x200;
    section[0].PointerToRawData = 0x400;
    section[0].Characteristics =
        IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;

    std::memcpy(section[1].Name, ".reloc", 6);
    section[1].Misc.VirtualSize = 0x200;
    section[1].VirtualAddress = 0x2000;
    section[1].SizeOfRawData = 0x200;
    section[1].PointerToRawData = 0x600;
    section[1].Characteristics =
        IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_DISCARDABLE;

    std::fill(file.begin() + 0x400, file.begin() + 0x600, std::uint8_t{0x90});
    const std::uint64_t absolute_value = kPreferredBase + 0x1234;
    std::memcpy(file.data() + 0x420, &absolute_value, sizeof(absolute_value));

    IMAGE_BASE_RELOCATION relocation{};
    relocation.VirtualAddress = 0x1000;
    relocation.SizeOfBlock = 12;
    std::memcpy(file.data() + 0x600, &relocation, sizeof(relocation));
    const std::array<WORD, 2> entries{
        static_cast<WORD>((IMAGE_REL_BASED_DIR64 << 12) | 0x20),
        static_cast<WORD>(IMAGE_REL_BASED_ABSOLUTE << 12),
    };
    std::memcpy(file.data() + 0x608, entries.data(), sizeof(entries));
    return file;
}

__declspec(noinline) int HookGuardProbe(int value) {
    return value * 3 + 7;
}

}  // namespace

bool RunHookTargetGuardTests() {
    using namespace hook_guard;

    if (!RunCharacterHookPlanTests()) {
        return false;
    }

    if (!Expect(IsContainedRange(4, 4, 8), "normal contained range") ||
        !Expect(IsContainedRange(8, 0, 8), "empty range at end") ||
        !Expect(!IsContainedRange(7, 2, 8), "range extending past end") ||
        !Expect(!IsContainedRange((std::numeric_limits<std::uint64_t>::max)() - 1, 4,
                                  (std::numeric_limits<std::uint64_t>::max)()),
                "overflowing range")) {
        return false;
    }

    std::vector<std::uint8_t> file = MakeRelocatablePe();
    PeLayout layout;
    std::vector<PeSection> sections;
    if (!Expect(ParsePeImage(file, layout, sections) == PeStatus::Ok,
                "synthetic PE should parse") ||
        !Expect(layout.preferred_image_base == kPreferredBase,
                "preferred image base should parse") ||
        !Expect(sections.size() == 2, "section table should parse")) {
        return false;
    }

    const RvaMapping header =
        ResolveRvaRange(0x100, 16, layout, sections, file.size(), false);
    const RvaMapping text =
        ResolveRvaRange(0x1010, 16, layout, sections, file.size(), true);
    const RvaMapping non_executable =
        ResolveRvaRange(0x2000, 8, layout, sections, file.size(), true);
    const RvaMapping virtual_tail =
        ResolveRvaRange(0x1250, 8, layout, sections, file.size(), false);
    const RvaMapping crossing_raw_end =
        ResolveRvaRange(0x11F8, 16, layout, sections, file.size(), false);
    if (!Expect(header.status == PeStatus::Ok && header.file_offset == 0x100,
                "header RVA should map directly") ||
        !Expect(text.status == PeStatus::Ok && text.file_offset == 0x410,
                "executable RVA should map through its section") ||
        !Expect(non_executable.status == PeStatus::RvaNotExecutable,
                "non-code section should be rejected for hooks") ||
        !Expect(virtual_tail.status == PeStatus::RvaNotFileBacked,
                "zero-fill section tail should not be trusted as pristine bytes") ||
        !Expect(crossing_raw_end.status == PeStatus::RvaNotFileBacked,
                "mapping must not cross a raw section boundary")) {
        return false;
    }

    std::array<std::uint8_t, 12> expected{};
    constexpr std::uint64_t kLoadedBase = kPreferredBase + 0x00200000;
    if (!Expect(BuildExpectedImageBytes(file,
                                        layout,
                                        sections,
                                        0x101E,
                                        kLoadedBase,
                                        expected) == PeStatus::Ok,
                "relocated expected bytes should build")) {
        return false;
    }
    std::uint64_t relocated_value = 0;
    std::memcpy(&relocated_value, expected.data() + 2, sizeof(relocated_value));
    if (!Expect(relocated_value == kLoadedBase + 0x1234,
                "DIR64 relocation should be applied even when partially overlapping range")) {
        return false;
    }
    constexpr std::uint64_t kLoadedBaseBelowPreferred = kPreferredBase - 0x00200000;
    if (!Expect(BuildExpectedImageBytes(file,
                                        layout,
                                        sections,
                                        0x101E,
                                        kLoadedBaseBelowPreferred,
                                        expected) == PeStatus::Ok,
                "relocations should support ASLR below the preferred image base")) {
        return false;
    }
    std::memcpy(&relocated_value, expected.data() + 2, sizeof(relocated_value));
    if (!Expect(relocated_value == kLoadedBaseBelowPreferred + 0x1234,
                "below-base DIR64 relocation should preserve the signed delta")) {
        return false;
    }

    std::vector<std::uint8_t> malformed = file;
    auto* malformed_block =
        reinterpret_cast<IMAGE_BASE_RELOCATION*>(malformed.data() + 0x600);
    malformed_block->SizeOfBlock = 6;
    if (!Expect(BuildExpectedImageBytes(malformed,
                                        layout,
                                        sections,
                                        0x101E,
                                        kLoadedBase,
                                        expected) == PeStatus::InvalidRelocations,
                "malformed relocation block should fail closed")) {
        return false;
    }

    std::vector<std::uint8_t> unsupported = file;
    const WORD unsupported_entry = static_cast<WORD>((7U << 12) | 0x20);
    std::memcpy(unsupported.data() + 0x608, &unsupported_entry, sizeof(unsupported_entry));
    if (!Expect(BuildExpectedImageBytes(unsupported,
                                        layout,
                                        sections,
                                        0x101E,
                                        kLoadedBase,
                                        expected) == PeStatus::UnsupportedRelocation,
                "unknown relocation type should fail closed")) {
        return false;
    }

    std::vector<std::uint8_t> truncated = file;
    truncated.resize(0x680);
    if (!Expect(ParsePeImage(truncated, layout, sections) == PeStatus::InvalidSectionTable,
                "truncated raw section should fail parsing")) {
        return false;
    }

    Guard guard;
    Result initialize_result;
    if (!Expect(guard.Initialize(nullptr, &initialize_result),
                "running test executable should map as its pristine image")) {
        std::cerr << "  status=" << StatusName(initialize_result.status)
                  << " pe_status=" << PeStatusName(initialize_result.pe_status)
                  << " win32_error=" << initialize_result.win32_error << '\n';
        return false;
    }

    Guard address_guard;
    Result address_initialize_result;
    if (!Expect(address_guard.InitializeForAddress(
                    reinterpret_cast<const void*>(&HookGuardProbe),
                    &address_initialize_result),
                "address-based guard should resolve the owning test image") ||
        !Expect(address_guard.Verify(reinterpret_cast<const void*>(&HookGuardProbe),
                                     kMinHookTargetVerificationBytes)
                    .verified(),
                "address-based guard should verify the owning image prologue")) {
        std::cerr << "  address status=" << StatusName(address_initialize_result.status)
                  << " pe_status=" << PeStatusName(address_initialize_result.pe_status)
                  << " win32_error=" << address_initialize_result.win32_error << '\n';
        return false;
    }
    address_guard.Reset();
    Result invalid_address_result;
    if (!Expect(!address_guard.InitializeForAddress(nullptr, &invalid_address_result) &&
                    invalid_address_result.status == Status::InvalidArgument,
                "address-based guard should reject a null target")) {
        return false;
    }

    volatile int probe_result = HookGuardProbe(5);
    (void)probe_result;
    const Result verified =
        guard.Verify(reinterpret_cast<const void*>(&HookGuardProbe),
                     kMinHookTargetVerificationBytes);
    int stack_value = 0;
    const Result outside = guard.Verify(&stack_value, 8);
    const Result invalid = guard.Verify(reinterpret_cast<const void*>(&HookGuardProbe), 0);
    if (!Expect(verified.verified(), "unmodified live prologue should match its disk image") ||
        !Expect(outside.status == Status::TargetOutsideImage,
                "non-image address should be rejected") ||
        !Expect(invalid.status == Status::InvalidArgument,
                "empty prologue should be rejected")) {
        if (!verified.verified()) {
            std::cerr << "  live status=" << StatusName(verified.status)
                      << " pe_status=" << PeStatusName(verified.pe_status)
                      << " rva=0x" << std::hex << verified.target_rva << std::dec
                      << " mismatch=" << verified.mismatch_offset << '\n';
        }
        return false;
    }

    // Exercise the span that protects MinHook's decoder window, not only the
    // first few bytes that happen to form the visible function signature.
    auto* probe_bytes = reinterpret_cast<std::uint8_t*>(
        reinterpret_cast<void*>(&HookGuardProbe));
    DWORD old_protection = 0;
    if (!Expect(VirtualProtect(probe_bytes,
                               kMinHookTargetVerificationBytes,
                               PAGE_EXECUTE_READWRITE,
                               &old_protection) != FALSE,
                "probe prologue should be temporarily writable")) {
        return false;
    }
    const std::uint8_t saved_byte = probe_bytes[20];
    probe_bytes[20] = static_cast<std::uint8_t>(saved_byte ^ 0x5AU);
    FlushInstructionCache(GetCurrentProcess(), probe_bytes, kMinHookTargetVerificationBytes);
    const Result modified =
        guard.Verify(reinterpret_cast<const void*>(&HookGuardProbe),
                     kMinHookTargetVerificationBytes);
    probe_bytes[20] = saved_byte;
    FlushInstructionCache(GetCurrentProcess(), probe_bytes, kMinHookTargetVerificationBytes);
    DWORD ignored_protection = 0;
    VirtualProtect(probe_bytes,
                   kMinHookTargetVerificationBytes,
                   old_protection,
                   &ignored_protection);
    if (!Expect(modified.status == Status::Modified && modified.mismatch_offset == 20,
                "a modification beyond the minimum jump span should be rejected") ||
        !Expect(guard.Verify(reinterpret_cast<const void*>(&HookGuardProbe),
                             kMinHookTargetVerificationBytes)
                    .verified(),
                "restored prologue should verify again")) {
        return false;
    }

    guard.Reset();
    return Expect(!guard.initialized(), "reset should release the transaction snapshot");
}

}  // namespace spatch::tests
