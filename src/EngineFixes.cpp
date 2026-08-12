#include "EngineFixes.h"

#include "DisplaySettings.h"
#include "InputPolicy.h"
#include "Logger.h"
#include "RuntimePatch.h"
#include "TextureFilteringPolicy.h"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <span>

namespace spatch::engine_fixes {
namespace {

constexpr std::uintptr_t kLegacySphericalReflectionSetupRva = 0x000442EF;
constexpr std::uintptr_t kLatestSphericalReflectionSetupRva = 0x0004458F;
constexpr std::uintptr_t kLegacyPresentFunctionRva = 0x006A0FF0;
constexpr std::uintptr_t kLatestPresentFunctionRva = 0x006A0FC0;
constexpr std::uintptr_t kLegacyHidden120FpsWaitBranchRva = 0x006A13F1;
constexpr std::uintptr_t kLatestHidden120FpsWaitBranchRva = 0x006A13C1;
constexpr std::uintptr_t kLegacySaveHeaderLoadRva = 0x004AD798;
constexpr std::uintptr_t kLatestSaveHeaderLoadRva = 0x004AD8C8;
constexpr std::uintptr_t kLegacySaveHeaderCallRva = 0x004AD96C;
constexpr std::uintptr_t kLatestSaveHeaderCallRva = 0x004ADA9C;
constexpr std::uintptr_t kLegacySaveParserCall1Rva = 0x004AD9B5;
constexpr std::uintptr_t kLatestSaveParserCall1Rva = 0x004ADAE5;
constexpr std::uintptr_t kLegacySaveParserCall2Rva = 0x004B6FD8;
constexpr std::uintptr_t kLatestSaveParserCall2Rva = 0x004B70A8;
constexpr std::uintptr_t kLegacySaveHeaderFunctionRva = 0x00496C70;
constexpr std::uintptr_t kLatestSaveHeaderFunctionRva = 0x00496EF0;
constexpr std::uintptr_t kLegacySaveParserFunctionRva = 0x0049B900;
constexpr std::uintptr_t kLatestSaveParserFunctionRva = 0x0049BA40;
constexpr std::uintptr_t kLegacyTaskCreateThreadCallRva = 0x00A38C02;
constexpr std::uintptr_t kLatestTaskCreateThreadCallRva = 0x00A38BA2;
constexpr std::uintptr_t kLegacyGenericCreateThreadCallRva = 0x00A3950F;
constexpr std::uintptr_t kLatestGenericCreateThreadCallRva = 0x00A394AF;
constexpr std::uintptr_t kLegacyIoCreateThreadCallRva = 0x00AA8494;
constexpr std::uintptr_t kLatestIoCreateThreadCallRva = 0x00AA8594;
constexpr std::uintptr_t kLegacyBankManagerCreateThreadCallRva = 0x00A610D2;
constexpr std::uintptr_t kLatestBankManagerCreateThreadCallRva = 0x00A60FA2;
constexpr std::uintptr_t kBankShutdownFenceCreateEventCallRva = 0x00A5CCBE;
constexpr std::uintptr_t kLegacyWwiseBlockingCompletionRva = 0x00AA85B0;
constexpr std::uintptr_t kLatestWwiseBlockingCompletionRva = 0x00AA86B0;
constexpr std::uintptr_t kLegacyWwiseBlockingWaitRva = 0x00AA86F0;
constexpr std::uintptr_t kLatestWwiseBlockingWaitRva = 0x00AA87F0;
constexpr std::uintptr_t kLegacyFirstRunResolutionRva = 0x01788130;
constexpr std::uintptr_t kLatestFirstRunResolutionRva = 0x01788250;
constexpr std::uintptr_t kLegacyScaleformQpcClockRva = 0x0098AC44;
constexpr std::uintptr_t kLatestScaleformQpcClockRva = 0x0098AFE4;
constexpr std::uintptr_t kLegacyFileTimestampOpenModeRva = 0x00A3938A;
constexpr std::uintptr_t kLatestFileTimestampOpenModeRva = 0x00A3932A;
constexpr std::uintptr_t kLegacyAudioFileHandleTestRva = 0x00A34B7B;
constexpr std::uintptr_t kLatestAudioFileHandleTestRva = 0x00A34ABB;
constexpr std::uintptr_t kLegacyAudioFileMappingArgumentRva = 0x00A34B97;
constexpr std::uintptr_t kLatestAudioFileMappingArgumentRva = 0x00A34AD7;
constexpr std::uintptr_t kLegacyFileSizeCombineRva = 0x0128E04B;
constexpr std::uintptr_t kLatestFileSizeCombineRva = 0x0128DCBB;
constexpr std::uintptr_t kLegacyContactImageFormatCallRva = 0x005D1B76;
constexpr std::uintptr_t kLatestContactImageFormatCallRva = 0x005D1C46;
constexpr std::uintptr_t kLegacyBenchmarkVramReadRva = 0x00A4161E;
constexpr std::uintptr_t kLatestBenchmarkVramReadRva = 0x00A4159E;
constexpr std::uintptr_t kAdapterCountRva = 0x0243A3A0;
constexpr std::uintptr_t kAdapterArrayRva = 0x0243A3A8;
constexpr std::uintptr_t kSelectedAdapterInterfaceRva = 0x02439AF8;
constexpr std::uintptr_t kTruncatedDedicatedVideoMemoryRva = 0x02439BB0;
constexpr std::uintptr_t kLegacyVramPoolFinalValidationCallRva = 0x0016E33D;
constexpr std::uintptr_t kLatestVramPoolFinalValidationCallRva = 0x0016E38D;
constexpr std::uintptr_t kLegacyVramPoolValidationFunctionRva = 0x001676D0;
constexpr std::uintptr_t kLatestVramPoolValidationFunctionRva = 0x00167700;
constexpr std::uintptr_t kLegacyCharacterSurfaceCopyCallRva = 0x000039CC;
constexpr std::uintptr_t kLatestCharacterSurfaceCopyCallRva = 0x00003ADC;
constexpr std::uintptr_t kLegacyLoadedChunkErrorLogCallRva = 0x0017B388;
constexpr std::uintptr_t kLatestLoadedChunkErrorLogCallRva = 0x0017B408;
constexpr std::uintptr_t kLegacyLoadedChunkFileErrorLogCallRva = 0x0017B562;
constexpr std::uintptr_t kLatestLoadedChunkFileErrorLogCallRva = 0x0017B5E2;
constexpr std::uintptr_t kLegacyLoadedChunkFileSizeCleanupCallRva = 0x0017B797;
constexpr std::uintptr_t kLatestLoadedChunkFileSizeCleanupCallRva = 0x0017B817;
constexpr std::uintptr_t kLegacySynchronousResourceFinalizeCallRva = 0x00174EFD;
constexpr std::uintptr_t kLatestSynchronousResourceFinalizeCallRva = 0x00174F9D;
constexpr std::uintptr_t kLegacySynchronousLooseOpenFailureBranchRva =
    0x00177134;
constexpr std::uintptr_t kLatestSynchronousLooseOpenFailureBranchRva =
    0x001771D4;
constexpr std::uintptr_t kLegacySynchronousLooseFinalizeCallRva = 0x0017724A;
constexpr std::uintptr_t kLatestSynchronousLooseFinalizeCallRva = 0x001772EA;
constexpr std::uintptr_t kLegacySynchronousLooseInvalidSizeStateRva =
    0x00177295;
constexpr std::uintptr_t kLatestSynchronousLooseInvalidSizeStateRva =
    0x00177335;
constexpr std::uintptr_t kLegacySynchronousLooseOpenFailureEpilogueRva =
    0x00177264;
constexpr std::uintptr_t kLatestSynchronousLooseOpenFailureEpilogueRva =
    0x00177304;
constexpr std::uintptr_t kLegacyLoadedChunkErrorLoggerRva = 0x00001FF0;
constexpr std::uintptr_t kLatestLoadedChunkErrorLoggerRva = 0x00002110;
constexpr std::uintptr_t kLegacyLoadedChunkFileErrorLoggerRva = 0x000C67A0;
constexpr std::uintptr_t kLatestLoadedChunkFileErrorLoggerRva = 0x000C6700;
constexpr std::uintptr_t kLegacyReleaseResourceWaitersRva = 0x001781F0;
constexpr std::uintptr_t kLatestReleaseResourceWaitersRva = 0x00178290;
constexpr std::uintptr_t kLegacyLoadedChunkFileSizeEpilogueRva = 0x0017B7D9;
constexpr std::uintptr_t kLatestLoadedChunkFileSizeEpilogueRva = 0x0017B859;
constexpr std::uintptr_t kLegacyResourceFinalizeRva = 0x0017FC80;
constexpr std::uintptr_t kLatestResourceFinalizeRva = 0x0017FD00;
constexpr std::uintptr_t kLegacyQcmpFailureCopyCallRva = 0x00189934;
constexpr std::uintptr_t kLatestQcmpFailureCopyCallRva = 0x001899E4;
constexpr std::uintptr_t kLegacyBufferCopyRva = 0x00A3A310;
constexpr std::uintptr_t kLatestBufferCopyRva = 0x00A3A290;
constexpr std::uintptr_t kLegacyCompressedXmlAllocationCallRva = 0x0008A61D;
constexpr std::uintptr_t kLatestCompressedXmlAllocationCallRva = 0x0008A9BD;
constexpr std::uintptr_t kLegacyCompressedXmlFinalizeRva = 0x0008A643;
constexpr std::uintptr_t kLatestCompressedXmlFinalizeRva = 0x0008A9E3;
constexpr std::uintptr_t kLegacyCompressedXmlCleanupRva = 0x0008A64B;
constexpr std::uintptr_t kLatestCompressedXmlCleanupRva = 0x0008A9EB;
constexpr std::uintptr_t kLegacyResourceAllocatorRva = 0x00187BE0;
constexpr std::uintptr_t kLatestResourceAllocatorRva = 0x00187C90;
constexpr std::uintptr_t kLegacyResourceFreeRva = 0x0016E720;
constexpr std::uintptr_t kLatestResourceFreeRva = 0x0016E770;
constexpr std::uintptr_t kResourceAllocatorInstanceRva = 0x02258190;
constexpr std::uintptr_t kResourceWorkPendingRva = 0x0225A64B;
constexpr std::uint32_t kMinimumSavePayloadSize = 0x84;
constexpr std::uint32_t kMinimumSaveFileSize = 0xB8;
constexpr unsigned int kFeatureApplyAttempts = 3;

// UserOptions already owns a hidden PCMouseInputRaw switch. Force only the
// value loaded from the options snapshot, leaving registration, WM_INPUT,
// cursor capture, and UI input routing on their stock paths.
constexpr std::array<std::uint8_t, 14> kRawMouseOptionSignature{
    0x48, 0x8B, 0x87, 0x10, 0x18, 0x00, 0x00,
    0x0F, 0xB6, 0x88, 0xBE, 0x02, 0x00, 0x00};
constexpr std::size_t kRawMouseOptionReadOffset = 7;
constexpr std::array<std::uint8_t, 7> kRawMouseOptionRead{
    0x0F, 0xB6, 0x88, 0xBE, 0x02, 0x00, 0x00};
constexpr std::array<std::uint8_t, 7> kForceRawMouseOption{
    0xB9, 0x01, 0x00, 0x00, 0x00, 0x90, 0x90};
constexpr std::size_t kRawMouseStateStoreOffset =
    kRawMouseOptionSignature.size();
constexpr std::array<std::uint8_t, 2> kRawMouseStateStoreOpcode{0x88, 0x0D};
constexpr std::array<std::uint8_t, 1> kRawMouseDisabled{0x00};
constexpr std::array<std::uint8_t, 1> kRawMouseEnabled{0x01};

// Gamepad axes are sampled directly through XInput or DirectInput. This is the
// complete, unique stock response table surrounding their radial deadzones.
// Patching the whole table transactionally keeps each deadzone and reciprocal
// scale paired, so a failed write cannot leave a distorted response curve.
constexpr std::array<std::uint8_t, 52> kControllerResponseSignature{
    0xFF, 0xFF, 0xF9, 0x3C, 0x5C, 0x8F, 0x42, 0x3E, 0x5C, 0xEC, 0xAC,
    0x3E, 0xF5, 0x6D, 0xBF, 0x3E, 0x6E, 0xDB, 0xB6, 0x3F, 0x1D, 0x47,
    0xC1, 0x3F, 0x80, 0x6F, 0xCC, 0x3F, 0xCA, 0x6B, 0xA8, 0x40, 0x00,
    0x00, 0x00, 0xB8, 0xFF, 0xFF, 0xF9, 0xBC, 0x5C, 0x8F, 0x42, 0xBE,
    0x5C, 0x8F, 0xC2, 0xBE, 0xCA, 0x6B, 0xA8, 0xC0};
constexpr std::size_t kLeftStickDeadzoneOffset = 8;
constexpr std::size_t kRightStickDeadzoneOffset = 12;
constexpr std::size_t kLeftStickScaleOffset = 20;
constexpr std::size_t kRightStickScaleOffset = 24;

// FollowCamera accumulates mouse deltas and drains them through qApproach at
// rate * frame_delta before applying yaw/pitch. The surrounding values make a
// unique data signature; only the two mouse drain rates are changed.
constexpr std::array<std::uint8_t, 48> kMouseCameraSignature{
    0x0A, 0xD7, 0x23, 0x3D, 0x0A, 0xD7, 0xA3, 0x3D, 0x0A, 0xD7, 0x23, 0x3D,
    0x0A, 0xD7, 0xA3, 0x3D, 0x00, 0x00, 0x20, 0x41, 0x00, 0x00, 0x20, 0x41,
    0x00, 0x00, 0x48, 0x42, 0x00, 0x00, 0x48, 0x42, 0x9A, 0x99, 0x99, 0x3E,
    0x8F, 0xC2, 0x75, 0x3D, 0x00, 0x00, 0x80, 0x40, 0x66, 0x66, 0x66, 0x3F};
constexpr std::size_t kHorizontalMouseDrainRateOffset = 16;
constexpr std::size_t kVerticalMouseDrainRateOffset = 20;
constexpr float kImmediateMouseDrainRate = 1.0e9f;

constexpr std::array<std::uint8_t, 14> kStockSphericalReflectionSetup{
    0xC7, 0x45, 0xA8, 0x00, 0x05, 0x00, 0x00,
    0xC7, 0x45, 0xAC, 0x80, 0x02, 0x00, 0x00};

constexpr std::array<std::uint8_t, 32> kPresentFunctionSignature{
    0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x60,
    0x48, 0xC7, 0x44, 0x24, 0x30, 0xFE, 0xFF, 0xFF, 0xFF, 0x0F, 0x29,
    0x74, 0x24, 0x50, 0x0F, 0x29, 0x7C, 0x24, 0x40, 0x0F, 0x28};
constexpr std::array<std::uint8_t, 6> kHidden120FpsWaitBranch{0x0F, 0x85, 0x86,
                                                              0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 6> kSkipHidden120FpsWait{0xE9, 0x87, 0x00,
                                                            0x00, 0x00, 0x90};
constexpr std::array<std::uint8_t, 4> kSaveHeaderLoadSignature{0x45, 0x8B, 0x7E,
                                                               0x68};
constexpr std::array<std::uint8_t, 35> kSaveParserSignature{
    0x48, 0x8B, 0xC4, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55, 0x56,
    0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8,
    0xB8, 0xFB, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x10, 0x05, 0x00, 0x00};
constexpr std::array<std::uint8_t, 5> kLegacySaveHeaderCall{0xE8, 0xFF, 0x92,
                                                            0xFE, 0xFF};
constexpr std::array<std::uint8_t, 5> kLatestSaveHeaderCall{0xE8, 0x4F, 0x94,
                                                            0xFE, 0xFF};
constexpr std::array<std::uint8_t, 5> kLegacySaveParserCall1{0xE8, 0x46, 0xDF,
                                                             0xFE, 0xFF};
constexpr std::array<std::uint8_t, 5> kLatestSaveParserCall1{0xE8, 0x56, 0xDF,
                                                             0xFE, 0xFF};
constexpr std::array<std::uint8_t, 5> kLegacySaveParserCall2{0xE8, 0x23, 0x49,
                                                             0xFE, 0xFF};
constexpr std::array<std::uint8_t, 5> kLatestSaveParserCall2{0xE8, 0x93, 0x49,
                                                             0xFE, 0xFF};
constexpr std::array<std::uint8_t, 6> kLegacyTaskCreateThreadCall{
    0xFF, 0x15, 0x60, 0x25, 0xBF, 0x00};
constexpr std::array<std::uint8_t, 6> kLatestTaskCreateThreadCall{
    0xFF, 0x15, 0xC0, 0x25, 0xBF, 0x00};
constexpr std::array<std::uint8_t, 6> kLegacyGenericCreateThreadCall{
    0xFF, 0x15, 0x53, 0x1C, 0xBF, 0x00};
constexpr std::array<std::uint8_t, 6> kLatestGenericCreateThreadCall{
    0xFF, 0x15, 0xB3, 0x1C, 0xBF, 0x00};
constexpr std::array<std::uint8_t, 6> kLegacyIoCreateThreadCall{
    0xFF, 0x15, 0xCE, 0x2C, 0xB8, 0x00};
constexpr std::array<std::uint8_t, 6> kLatestIoCreateThreadCall{
    0xFF, 0x15, 0xCE, 0x2B, 0xB8, 0x00};
constexpr std::array<std::uint8_t, 6> kLegacyBankManagerCreateThreadCall{
    0xFF, 0x15, 0x90, 0xA0, 0xBC, 0x00};
constexpr std::array<std::uint8_t, 6> kLatestBankManagerCreateThreadCall{
    0xFF, 0x15, 0xC0, 0xA1, 0xBC, 0x00};
constexpr std::array<std::uint8_t, 6> kBankShutdownFenceCreateEventCall{
    0xFF, 0x15, 0x64, 0xE7, 0xBC, 0x00};
constexpr std::array<std::uint8_t, 15> kLegacyWwiseBlockingCompletion{
    0x48, 0x8B, 0x4A, 0x08, 0xC6, 0x42, 0x10, 0x00,
    0x48, 0xFF, 0x25, 0x91, 0x2C, 0xB8, 0x00};
constexpr std::array<std::uint8_t, 15> kLatestWwiseBlockingCompletion{
    0x48, 0x8B, 0x4A, 0x08, 0xC6, 0x42, 0x10, 0x00,
    0x48, 0xFF, 0x25, 0x91, 0x2B, 0xB8, 0x00};
constexpr std::array<std::uint8_t, 14> kLegacyWwiseBlockingWait{
    0x48, 0x8B, 0x4A, 0x08, 0x83, 0xCA, 0xFF,
    0x48, 0xFF, 0x25, 0x4A, 0x2A, 0xB8, 0x00};
constexpr std::array<std::uint8_t, 14> kLatestWwiseBlockingWait{
    0x48, 0x8B, 0x4A, 0x08, 0x83, 0xCA, 0xFF,
    0x48, 0xFF, 0x25, 0x4A, 0x29, 0xB8, 0x00};
// UserOptions.xml defaults Resolution to "1920x1880", but the missing-
// DisplaySettings path immediately below it recognizes only 1920x1080 and
// 1440x1080.  The display-mode constructor zeroes width and height first, so
// the typo otherwise reaches device creation as a 0x0 mode on a clean profile.
// Both strings have the same length, keeping this correction data-only.
constexpr std::array<std::uint8_t, 9> kInvalidFirstRunResolution{
    0x31, 0x39, 0x32, 0x30, 0x78, 0x31, 0x38, 0x38, 0x30};
constexpr std::array<std::uint8_t, 9> kValidFirstRunResolution{
    0x31, 0x39, 0x32, 0x30, 0x78, 0x31, 0x30, 0x38, 0x30};
// Scaleform converts QueryPerformanceCounter ticks to microseconds by first
// multiplying the 64-bit counter by 1,000,000. The stock IMUL keeps only the
// low half of that product, making the clock jump backwards whenever the
// product crosses 2^64. Use MUL so the existing DIV consumes RDX:RAX instead.
// The replacement is the same size and produces identical results until the
// first stock overflow.
constexpr std::array<std::uint8_t, 15> kTruncatedScaleformQpcConversion{
    0x48, 0x69, 0xDB, 0x40, 0x42, 0x0F, 0x00,  // imul rbx, rbx, 1000000
    0x33, 0xD2,                                // xor edx, edx
    0x48, 0x8B, 0xC3,                          // mov rax, rbx
    0x49, 0xF7, 0xF0};                         // div r8
constexpr std::array<std::uint8_t, 15> kFullScaleformQpcConversion{
    0x48, 0x8B, 0xC3,              // mov rax, rbx
    0xB9, 0x40, 0x42, 0x0F, 0x00,  // mov ecx, 1000000
    0x48, 0xF7, 0xE1,              // mul rcx
    0x49, 0xF7, 0xF0,              // div r8
    0x90};                         // nop
// The timestamp helper tries OPEN_EXISTING, reads GetLastError even on success,
// and retries with CREATE_NEW when that stale value happens to be
// ERROR_FILE_NOT_FOUND. OPEN_ALWAYS is the intended one-call operation and,
// unlike OPEN_EXISTING, defines LastError on a successful open or create.
constexpr std::array<std::uint8_t, 8> kTimestampOpenExisting{
    0xC7, 0x44, 0x24, 0x20, 0x03, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 8> kTimestampOpenAlways{
    0xC7, 0x44, 0x24, 0x20, 0x04, 0x00, 0x00, 0x00};
// CreateFile returns INVALID_HANDLE_VALUE, not NULL. The parser already saves
// the returned handle in RBX, so RAX can be incremented for an exact three-byte
// sentinel test as long as CreateFileMapping receives the saved RBX value.
constexpr std::array<std::uint8_t, 3> kNullFileHandleTest{0x48, 0x85, 0xC0};
constexpr std::array<std::uint8_t, 3> kInvalidFileHandleTest{0x48, 0xFF, 0xC0};
constexpr std::array<std::uint8_t, 3> kMappingArgumentFromRax{0x48, 0x8B, 0xC8};
constexpr std::array<std::uint8_t, 3> kMappingArgumentFromRbx{0x48, 0x8B, 0xCB};
// BY_HANDLE_FILE_INFORMATION stores nFileSizeHigh immediately before
// nFileSizeLow. Stock code adds those DWORDs and passes the 32-bit sum to a
// 64-bit setter. Load the pair, swap its DWORD halves, and store the complete
// high:low value directly. The 14-byte replacement leaves the following
// control-flow instruction untouched.
constexpr std::array<std::uint8_t, 14> kTruncatedFileSizeCombine{
    0x8B, 0x55, 0x08, 0x03, 0x55, 0x0C, 0x49,
    0x8B, 0xCE, 0xE8, 0x77, 0xDB, 0xFF, 0xFF};
constexpr std::array<std::uint8_t, 14> kFullFileSizeCombine{
    0x48, 0x8B, 0x55, 0x08, 0x48, 0xC1, 0xCA,
    0x20, 0x49, 0x89, 0x56, 0x08, 0x66, 0x90};
// ContactList_AddContact formats contact_name + "_img32" into a 64-byte
// stack buffer, then never reads any byte of that buffer. Long contact names
// can therefore overwrite the frame for work with no observable consumer.
// Remove only the dead call and leave all three Scaleform arguments unchanged.
constexpr std::array<std::uint8_t, 5> kLegacyContactImageFormatCall{
    0xE8, 0x21, 0xF0, 0xCD, 0x00};
constexpr std::array<std::uint8_t, 5> kLatestContactImageFormatCall{
    0xE8, 0xD1, 0xEB, 0xCD, 0x00};
constexpr std::array<std::uint8_t, 5> kSkipContactImageFormatCall{
    0x90, 0x90, 0x90, 0x90, 0x90};
// DXGI_ADAPTER_DESC1::DedicatedVideoMemory is SIZE_T, but the benchmark path
// first truncates it to a DWORD and only then converts bytes to MiB. On GPUs
// with at least 4 GiB this reports the low 32-bit remainder. Redirect the
// load/shift pair through a relay that finds the selected adapter record and
// performs the conversion on the complete 64-bit field.
constexpr std::array<std::uint8_t, 9> kLegacyTruncatedBenchmarkVramRead{
    0x8B, 0x05, 0x8C, 0x85, 0x9F, 0x01, 0xC1, 0xE8, 0x14};
constexpr std::array<std::uint8_t, 9> kLatestTruncatedBenchmarkVramRead{
    0x8B, 0x05, 0x0C, 0x86, 0x9F, 0x01, 0xC1, 0xE8, 0x14};
constexpr std::array<std::uint8_t, 5> kLegacyVramPoolFinalValidationCall{
    0xE8, 0x8E, 0x93, 0xFF, 0xFF};
constexpr std::array<std::uint8_t, 5> kLatestVramPoolFinalValidationCall{
    0xE8, 0x6E, 0x93, 0xFF, 0xFF};
// CharacterLook still produces wetness and sweat at floats 9/10 (+0x24/+0x28)
// of its 64-byte render block. The original material consumer reads the
// wetness value from float 14 (+0x38), while DE leaves the adjacent +0x3C
// per-draw slot occupied by an unrelated material value. Redirect only this
// verified staging copy and mirror wetness after copying; sweat is applied at
// the CharacterLook component and must never clobber the unrelated slot.
constexpr std::array<std::uint8_t, 5> kLegacyCharacterSurfaceCopyCall{
    0xE8, 0x3F, 0x69, 0xA3, 0x00};
constexpr std::array<std::uint8_t, 5> kLatestCharacterSurfaceCopyCall{
    0xE8, 0xAF, 0x67, 0xA3, 0x00};
// Async resource callbacks already release their waiting load tables, but a
// read/decompression error leaves the resource in LOADING forever. The file-
// size callback is worse: after releasing the same waiters it frees the object
// even though duplicate load requests may have raised its reference count.
// Redirect only the three failure-only calls through process-lifetime relays.
constexpr std::array<std::uint8_t, 5> kLegacyLoadedChunkErrorLogCall{
    0xE8, 0x63, 0x6C, 0xE8, 0xFF};
constexpr std::array<std::uint8_t, 5> kLatestLoadedChunkErrorLogCall{
    0xE8, 0x03, 0x6D, 0xE8, 0xFF};
constexpr std::array<std::uint8_t, 5> kLegacyLoadedChunkFileErrorLogCall{
    0xE8, 0x39, 0xB2, 0xF4, 0xFF};
constexpr std::array<std::uint8_t, 5> kLatestLoadedChunkFileErrorLogCall{
    0xE8, 0x19, 0xB1, 0xF4, 0xFF};
constexpr std::array<std::uint8_t, 5> kLegacyLoadedChunkFileSizeCleanupCall{
    0xE8, 0x54, 0xCA, 0xFF, 0xFF};
constexpr std::array<std::uint8_t, 5> kLatestLoadedChunkFileSizeCleanupCall{
    0xE8, 0x74, 0xCA, 0xFF, 0xFF};
constexpr std::array<std::uint8_t, 5> kLegacySynchronousResourceFinalizeCall{
    0xE8, 0x7E, 0xAD, 0x00, 0x00};
constexpr std::array<std::uint8_t, 5> kLatestSynchronousResourceFinalizeCall{
    0xE8, 0x5E, 0xAD, 0x00, 0x00};
constexpr std::array<std::uint8_t, 6> kSynchronousLooseOpenFailureBranch{
    0x0F, 0x84, 0x2A, 0x01, 0x00, 0x00};
constexpr std::array<std::uint8_t, 5> kLegacySynchronousLooseFinalizeCall{
    0xE8, 0x31, 0x8A, 0x00, 0x00};
constexpr std::array<std::uint8_t, 5> kLatestSynchronousLooseFinalizeCall{
    0xE8, 0x11, 0x8A, 0x00, 0x00};
constexpr std::array<std::uint8_t, 10> kSynchronousLooseInvalidSizeState{
    0xC7, 0x87, 0xB0, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 5> kLegacyQcmpFailureCopyCall{
    0xE8, 0xD7, 0x09, 0x8B, 0x00};
constexpr std::array<std::uint8_t, 5> kLatestQcmpFailureCopyCall{
    0xE8, 0xA7, 0x08, 0x8B, 0x00};
constexpr std::array<std::uint8_t, 5> kLegacyCompressedXmlAllocationCall{
    0xE8, 0xBE, 0xD5, 0x0F, 0x00};
constexpr std::array<std::uint8_t, 5> kLatestCompressedXmlAllocationCall{
    0xE8, 0xCE, 0xD2, 0x0F, 0x00};
constexpr std::array<std::uint8_t, 6> kCompressedXmlFinalize{
    0xC6, 0x04, 0x3B, 0x00, 0xEB, 0x02};

using DeserializeSaveFn = bool(__fastcall*)(void*,
                                            std::uint32_t,
                                            std::uint32_t,
                                            const void*,
                                            std::uint32_t);

struct RelayLayout {
    std::uint8_t* page = nullptr;
    std::uintptr_t save_parser = 0;
    std::uintptr_t save_header = 0;
    std::uintptr_t create_thread = 0;
    std::uintptr_t create_io_thread = 0;
    std::uintptr_t create_task_thread = 0;
    std::uintptr_t create_bank_manager_thread = 0;
    std::uintptr_t create_bank_shutdown_fence_event = 0;
    std::uintptr_t complete_wwise_blocking_operation = 0;
    std::uintptr_t wait_wwise_blocking_operation = 0;
    bool latest_steam = false;
    bool ready = false;
};

struct VramUnlockRelay {
    std::uint8_t* page = nullptr;
    std::uintptr_t entry = 0;
    std::uintptr_t module_base = 0;
    bool latest_steam = false;
    bool ready = false;
};

struct VramCapacityRelay {
    std::uint8_t* page = nullptr;
    std::uintptr_t entry = 0;
    std::uintptr_t module_base = 0;
    bool latest_steam = false;
    bool ready = false;
};

struct CharacterSurfaceRelay {
    std::uint8_t* page = nullptr;
    std::uintptr_t entry = 0;
    std::uintptr_t module_base = 0;
    bool latest_steam = false;
    bool bridge_wetness = false;
    bool ready = false;
};

struct ResourceFailureRelay {
    std::uint8_t* page = nullptr;
    std::uintptr_t loaded_chunk_error = 0;
    std::uintptr_t loaded_chunk_file_error = 0;
    std::uintptr_t loaded_chunk_file_size_error = 0;
    std::uintptr_t synchronous_indexed_read_error = 0;
    std::uintptr_t synchronous_loose_read_error = 0;
    std::uintptr_t synchronous_loose_open_error = 0;
    std::uintptr_t synchronous_loose_size_error = 0;
    std::uintptr_t qcmp_failure_copy_guard = 0;
    std::uintptr_t compressed_xml_allocation_guard = 0;
    std::uintptr_t compressed_xml_finalize_guard = 0;
    std::uintptr_t module_base = 0;
    bool latest_steam = false;
    bool ready = false;
};

runtime_patch::Registry g_static_patches;
// The public initialize/shutdown entry points are normally serialized by
// Hooks, but they are also callable directly. Protect the registry and all
// non-atomic feature-resolution state with one real exclusion boundary.
std::mutex g_lifecycle_mutex;
std::atomic<bool> g_initialized = false;
// Keep the initialization gate separate from the committed state.  The old
// code set g_initialized before the first write, so a transient allocation,
// protection, or signature failure could permanently suppress a later retry
// even though no patch was actually owned by the registry.
std::atomic<bool> g_initialization_in_progress = false;
std::atomic<bool> g_initialization_complete = false;
std::uint32_t g_requested_feature_mask = 0;
bool g_requested_feature_mask_valid = false;
struct StaticPatchRequest {
    std::uint32_t feature_mask = 0;
    std::uintptr_t module_base = 0;
    bool latest_steam_layout = false;
    int spherical_reflection_width = 0;
    int left_stick_deadzone = input::kStockDeadzone;
    int right_stick_deadzone = input::kStockDeadzone;

    bool operator==(const StaticPatchRequest&) const = default;
};
std::optional<StaticPatchRequest> g_active_request;
bool g_spherical_reflection_resolved = false;
bool g_force_anisotropic_filtering_resolved = false;
bool g_hidden_cap_resolved = false;
bool g_save_guard_resolved = false;
bool g_thread_guard_resolved = false;
bool g_first_run_resolution_resolved = false;
bool g_scaleform_qpc_clock_resolved = false;
bool g_file_timestamp_open_mode_resolved = false;
bool g_audio_file_open_resolved = false;
bool g_file_size_resolved = false;
bool g_contact_list_overflow_resolved = false;
bool g_vram_capacity_resolved = false;
bool g_vram_pool_lock_resolved = false;
bool g_character_surface_resolved = false;
bool g_raw_mouse_input_resolved = false;
bool g_camera_smoothing_resolved = false;
bool g_controller_deadzone_resolved = false;
bool g_resource_failure_recovery_resolved = false;
// A parser callsite can still be executing the relay after its bytes have been
// restored (and an incomplete rollback deliberately retains that relay).  The
// game parser lives in the process-lifetime main module, so publish it once and
// never clear it while the process is alive.
std::atomic<DeserializeSaveFn> g_deserialize_save_original{nullptr};
RelayLayout g_relay_layout{};
// FUN_140A5CB60 publishes a stack callback context and then waits on a newly
// created auto-reset event. Reserve one matching handle before the callsite is
// patched so a transient kernel allocation failure cannot turn that wait into
// WAIT_FAILED and let the callback write through an expired stack pointer.
std::atomic<HANDLE> g_bank_shutdown_fence_event_reserve{nullptr};
// Relay code can remain on another thread's return stack after its call site
// is restored, so these bounded, layout-specific pages live until process
// exit. There are only two supported executable builds.
std::array<VramUnlockRelay, 2> g_vram_unlock_relays{};
std::array<VramCapacityRelay, 2> g_vram_capacity_relays{};
std::array<CharacterSurfaceRelay, 8> g_character_surface_relays{};
std::array<ResourceFailureRelay, 8> g_resource_failure_relays{};

constexpr std::uint32_t kFeatureSphericalReflection = 1u << 0;
constexpr std::uint32_t kFeatureForceAnisotropicFiltering = 1u << 1;
constexpr std::uint32_t kFeatureHiddenCap = 1u << 2;
constexpr std::uint32_t kFeatureSaveGuard = 1u << 3;
constexpr std::uint32_t kFeatureThreadGuard = 1u << 4;
constexpr std::uint32_t kFeatureFirstRunResolution = 1u << 5;
constexpr std::uint32_t kFeatureScaleformQpcClock = 1u << 6;
constexpr std::uint32_t kFeatureFileTimestampOpenMode = 1u << 7;
constexpr std::uint32_t kFeatureAudioFileOpen = 1u << 8;
constexpr std::uint32_t kFeatureFileSize = 1u << 9;
constexpr std::uint32_t kFeatureVramPoolLock = 1u << 10;
constexpr std::uint32_t kFeatureVramCapacity = 1u << 11;
constexpr std::uint32_t kFeatureCharacterSurface = 1u << 12;
constexpr std::uint32_t kFeatureRawMouseInput = 1u << 13;
constexpr std::uint32_t kFeatureCameraSmoothing = 1u << 14;
constexpr std::uint32_t kFeatureControllerDeadzone = 1u << 15;
constexpr std::uint32_t kFeatureResourceFailureRecovery = 1u << 16;
constexpr std::uint32_t kFeatureContactListOverflow = 1u << 17;

struct WindowSearchState {
    DWORD process_id = 0;
    int best_width = 0;
    long long best_area = 0;
};

BOOL CALLBACK InspectProcessWindow(HWND window, LPARAM user_data) {
    auto* state = reinterpret_cast<WindowSearchState*>(user_data);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (state == nullptr || process_id != state->process_id ||
        !IsWindowVisible(window)) {
        return TRUE;
    }

    RECT client{};
    if (!GetClientRect(window, &client)) {
        return TRUE;
    }
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const long long area =
        static_cast<long long>(width) * static_cast<long long>(height);
    if (width > 0 && height > 0 && area > state->best_area) {
        state->best_width = width;
        state->best_area = area;
    }
    return TRUE;
}

template <std::size_t Size>
void StoreLittleEndian32(std::array<std::uint8_t, Size>& bytes,
                         std::size_t offset,
                         std::uint32_t value) {
    static_assert(Size >= sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template <std::size_t Size>
void StoreFloat(std::array<std::uint8_t, Size>& bytes,
                std::size_t offset,
                float value) {
    static_assert(Size >= sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

bool IsReadableProtection(DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    switch (protection & 0xFFu) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

bool IsExecutableProtection(DWORD protection) noexcept {
    switch (protection & 0xFFu) {
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

enum class PatternMemoryClass {
    Executable,
    NonExecutable,
};

#if defined(_MSC_VER)
__declspec(noinline)
#endif
bool CopyBytesSafely(void* destination,
                     std::size_t destination_size,
                     const void* source,
                     std::size_t source_size,
                     std::size_t source_offset,
                     std::size_t byte_count) noexcept {
    if (destination == nullptr || source == nullptr || byte_count == 0 ||
        byte_count > destination_size || source_offset > source_size ||
        byte_count > source_size - source_offset) {
        return false;
    }

    const std::uintptr_t source_address =
        reinterpret_cast<std::uintptr_t>(source);
    if (source_address >
        (std::numeric_limits<std::uintptr_t>::max)() - source_offset) {
        return false;
    }

#if defined(_MSC_VER)
    __try {
        std::memcpy(destination,
                    reinterpret_cast<const void*>(source_address +
                                                  source_offset),
                    byte_count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(destination,
                reinterpret_cast<const void*>(source_address + source_offset),
                byte_count);
#endif
    return true;
}

bool ReadPointerFieldSafely(const void* source,
                            std::size_t source_size,
                            std::size_t source_offset,
                            std::uintptr_t& value) noexcept {
    return CopyBytesSafely(&value, sizeof(value), source, source_size,
                           source_offset, sizeof(value));
}

struct PatternSearchResult {
    std::uintptr_t address = 0;
    std::size_t matches = 0;
};

PatternSearchResult FindUniquePattern(
    std::uintptr_t module_base,
    std::span<const std::uint8_t> pattern,
    PatternMemoryClass memory_class) {
    PatternSearchResult result{};
    if (module_base == 0 || pattern.empty()) {
        return result;
    }

    std::uintptr_t cursor = module_base;
    for (;;) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory,
                         sizeof(memory)) != sizeof(memory) ||
            reinterpret_cast<std::uintptr_t>(memory.AllocationBase) !=
                module_base) {
            break;
        }

        const std::uintptr_t region_base =
            reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (memory.RegionSize == 0 ||
            region_base > (std::numeric_limits<std::uintptr_t>::max)() -
                              memory.RegionSize) {
            break;
        }
        const std::uintptr_t region_end = region_base + memory.RegionSize;
        const std::uintptr_t begin_address = (std::max)(cursor, region_base);
        const bool is_executable = IsExecutableProtection(memory.Protect);
        const bool matches_memory_class =
            memory_class == PatternMemoryClass::Executable ? is_executable
                                                           : !is_executable;
        if (memory.State == MEM_COMMIT &&
            IsReadableProtection(memory.Protect) && matches_memory_class &&
            begin_address < region_end) {
            const std::size_t readable_size =
                static_cast<std::size_t>(region_end - begin_address);
            if (readable_size >= pattern.size()) {
                const auto* begin =
                    reinterpret_cast<const std::uint8_t*>(begin_address);
                const std::size_t last = readable_size - pattern.size();
                for (std::size_t offset = 0; offset <= last; ++offset) {
                    if (std::memcmp(begin + offset, pattern.data(),
                                    pattern.size()) != 0) {
                        continue;
                    }
                    result.address = begin_address + offset;
                    ++result.matches;
                    if (result.matches > 1) {
                        return result;
                    }
                }
            }
        }
        cursor = region_end;
    }
    return result;
}

template <std::size_t Size>
std::optional<std::uintptr_t> ResolveUniquePattern(
    std::uintptr_t module_base,
    const char* name,
    const std::array<std::uint8_t, Size>& expected,
    const std::array<std::uint8_t, Size>& replacement,
    PatternMemoryClass memory_class) {
    PatternSearchResult result =
        FindUniquePattern(module_base, expected, memory_class);
    if (result.matches == 1) {
        return result.address;
    }
    if (result.matches == 0 && expected != replacement) {
        result = FindUniquePattern(module_base, replacement, memory_class);
        if (result.matches == 1) {
            return result.address;
        }
    }
    log::WarnF(
        "engine_patch name=%s result=signature_mismatch matches=%zu disabled=1",
        name, result.matches);
    return std::nullopt;
}

void LogPatchResult(const char* name,
                    std::uintptr_t address,
                    runtime_patch::ApplyResult result) {
    if (result == runtime_patch::ApplyResult::Applied ||
        result == runtime_patch::ApplyResult::AlreadyApplied) {
        log::InfoF("engine_patch name=%s result=%s target=0x%p", name,
                   runtime_patch::ApplyResultName(result),
                   reinterpret_cast<void*>(address));
    } else {
        log::WarnF("engine_patch name=%s result=%s target=0x%p disabled=1",
                   name, runtime_patch::ApplyResultName(result),
                   reinterpret_cast<void*>(address));
    }
}

bool __fastcall SafeDeserializeSave(void* snapshot,
                                    std::uint32_t filter,
                                    std::uint32_t required_mask,
                                    const void* payload,
                                    std::uint32_t payload_size) {
    const DeserializeSaveFn original =
        g_deserialize_save_original.load(std::memory_order_acquire);
    if (original == nullptr ||
        !IsSafeSavePayload(reinterpret_cast<std::uintptr_t>(payload),
                           payload_size)) {
        return false;
    }
    return original(snapshot, filter, required_mask, payload, payload_size);
}

HANDLE WINAPI CreateThreadSentinelAdapter(LPSECURITY_ATTRIBUTES attributes,
                                          SIZE_T stack_size,
                                          LPTHREAD_START_ROUTINE start_routine,
                                          LPVOID parameter,
                                          DWORD creation_flags,
                                          LPDWORD thread_id) {
    const HANDLE handle = CreateThread(attributes, stack_size, start_routine,
                                       parameter, creation_flags, thread_id);
    return reinterpret_cast<HANDLE>(
        NormalizeThreadHandle(reinterpret_cast<std::uintptr_t>(handle)));
}

HANDLE WINAPI
CreateTaskThreadWithEventGuard(LPSECURITY_ATTRIBUTES attributes,
                               SIZE_T stack_size,
                               LPTHREAD_START_ROUTINE start_routine,
                               LPVOID parameter,
                               DWORD creation_flags,
                               LPDWORD thread_id) {
    if (parameter == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    // qTaskManagerData creates these four events before starting any of its
    // workers, but stock checks none of them.  The worker waits on close/add,
    // while queue submission and completion use sync/all-done.  Starting with
    // any missing handle leaves the manager unable to wake, drain, or shut
    // down.  Its destructor safely closes partial initialization and its
    // caller already expects INVALID_HANDLE_VALUE for thread creation failure.
    constexpr std::size_t kManagerViewSize = 0xC0;
    std::uintptr_t sync_event = 0;
    std::uintptr_t close_event = 0;
    std::uintptr_t add_event = 0;
    std::uintptr_t all_done_event = 0;
    if (!ReadPointerFieldSafely(parameter, kManagerViewSize, 0x68,
                                sync_event) ||
        !ReadPointerFieldSafely(parameter, kManagerViewSize, 0x88,
                                close_event) ||
        !ReadPointerFieldSafely(parameter, kManagerViewSize, 0xA0,
                                add_event) ||
        !ReadPointerFieldSafely(parameter, kManagerViewSize, 0xB8,
                                all_done_event)) {
        SetLastError(ERROR_NOACCESS);
        return INVALID_HANDLE_VALUE;
    }
    if (!AreTaskManagerEventsReady(sync_event, close_event, add_event,
                                   all_done_event)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return INVALID_HANDLE_VALUE;
    }

    const HANDLE handle = CreateThread(attributes, stack_size, start_routine,
                                       parameter, creation_flags, thread_id);
    return reinterpret_cast<HANDLE>(
        NormalizeThreadHandle(reinterpret_cast<std::uintptr_t>(handle)));
}

HANDLE WINAPI CreateIoThreadWithEventGuard(LPSECURITY_ATTRIBUTES attributes,
                                           SIZE_T stack_size,
                                           LPTHREAD_START_ROUTINE start_routine,
                                           LPVOID parameter,
                                           DWORD creation_flags,
                                           LPDWORD thread_id) {
    if (parameter == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    // AK::IOThread creates four event handles before this call, but the stock
    // success check omits the shutdown event at +0x48.  Refuse to start a
    // worker that would immediately pass a NULL handle to its wait set; the
    // existing caller already treats a NULL thread handle as initialization
    // failure and the owner destructor closes every successfully-created event.
    constexpr std::size_t kOwnerViewSize = 0x60;
    std::uintptr_t wake_event = 0;
    std::uintptr_t shutdown_event = 0;
    std::uintptr_t work_event = 0;
    std::uintptr_t idle_event = 0;
    if (!ReadPointerFieldSafely(parameter, kOwnerViewSize, 0x40, wake_event) ||
        !ReadPointerFieldSafely(parameter, kOwnerViewSize, 0x48,
                                shutdown_event) ||
        !ReadPointerFieldSafely(parameter, kOwnerViewSize, 0x50, work_event) ||
        !ReadPointerFieldSafely(parameter, kOwnerViewSize, 0x58, idle_event)) {
        SetLastError(ERROR_NOACCESS);
        return nullptr;
    }
    if (!AreIoThreadBootstrapEventsReady(wake_event, shutdown_event, work_event,
                                         idle_event)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }

    return CreateThread(attributes, stack_size, start_routine, parameter,
                        creation_flags, thread_id);
}

HANDLE WINAPI
CreateBankManagerThreadWithEventGuard(LPSECURITY_ATTRIBUTES attributes,
                                      SIZE_T stack_size,
                                      LPTHREAD_START_ROUTINE start_routine,
                                      LPVOID parameter,
                                      DWORD creation_flags,
                                      LPDWORD thread_id) {
    if (parameter == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    // AK::BankManager's outer wake event is checked by stock code, but the
    // manual-reset callback fence constructed at owner+0x938 is not.  A null
    // fence makes ResetEvent/SetEvent fail and turns callback-removal waits
    // into immediate WAIT_FAILED returns, so callbacks can outlive the object
    // that unregisters them.  The initializer tail-jumps into the bootstrap
    // and propagates its NULL-thread failure result to the top-level AK init.
    constexpr std::size_t kOwnerViewSize = 0x940;
    std::uintptr_t wake_event = 0;
    std::uintptr_t callback_fence_event = 0;
    if (!ReadPointerFieldSafely(parameter, kOwnerViewSize, 0x40, wake_event) ||
        !ReadPointerFieldSafely(parameter, kOwnerViewSize, 0x938,
                                callback_fence_event)) {
        SetLastError(ERROR_NOACCESS);
        return nullptr;
    }
    if (!AreBankManagerEventsReady(wake_event, callback_fence_event)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }

    return CreateThread(attributes, stack_size, start_routine, parameter,
                        creation_flags, thread_id);
}

bool PrepareBankShutdownFenceEventReserve() noexcept {
    if (g_bank_shutdown_fence_event_reserve.load(std::memory_order_acquire) !=
        nullptr) {
        return true;
    }

    HANDLE created = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (created == nullptr) {
        return false;
    }
    HANDLE expected = nullptr;
    if (!g_bank_shutdown_fence_event_reserve.compare_exchange_strong(
            expected, created, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        CloseHandle(created);
    }
    return true;
}

void ReleaseUnusedBankShutdownFenceEventReserve() noexcept {
    HANDLE reserved = g_bank_shutdown_fence_event_reserve.exchange(
        nullptr, std::memory_order_acq_rel);
    if (reserved != nullptr) {
        CloseHandle(reserved);
    }
}

HANDLE WINAPI
CreateBankShutdownFenceEventWithReserve(LPSECURITY_ATTRIBUTES attributes,
                                        BOOL manual_reset,
                                        BOOL initial_state,
                                        LPCWSTR name) {
    HANDLE created =
        CreateEventW(attributes, manual_reset, initial_state, name);
    if (created != nullptr) {
        return created;
    }

    // This callsite requests an unnamed, initially-unsignaled auto-reset
    // event. The shutdown loop is serialized on the sole BankManager worker
    // and closes each event before its next iteration. Transferring one
    // pre-created matching handle therefore preserves the stock ownership and
    // wait contract while also freeing a kernel slot for a following iteration.
    const HANDLE reserved = g_bank_shutdown_fence_event_reserve.exchange(
        nullptr, std::memory_order_acq_rel);
    return reinterpret_cast<HANDLE>(SelectCreatedOrReservedEvent(
        reinterpret_cast<std::uintptr_t>(created),
        reinterpret_cast<std::uintptr_t>(reserved)));
}

void __fastcall CompleteWwiseBlockingOperation(void*, void* event_owner) {
    if (event_owner == nullptr) {
        return;
    }

    auto* const owner = static_cast<std::uint8_t*>(event_owner);
    std::uintptr_t event_value = 0;
    std::memcpy(&event_value, owner + 0x08, sizeof(event_value));
    if (event_value != 0) {
        (void)SetEvent(reinterpret_cast<HANDLE>(event_value));
    }

    // Stock clears this byte before SetEvent. With a null handle that lets the
    // waiter observe completion while this callback can still read its owner.
    // Make the flag the final release operation so both the kernel-event and
    // allocation-failure paths share one lifetime boundary.
    (void)_InterlockedExchange8(reinterpret_cast<volatile char*>(owner + 0x10),
                                0);
}

DWORD __fastcall WaitForWwiseBlockingOperation(void*, void* event_owner) {
    if (event_owner == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }

    auto* const owner = static_cast<std::uint8_t*>(event_owner);
    std::uintptr_t event_value = 0;
    std::memcpy(&event_value, owner + 0x08, sizeof(event_value));
    DWORD wait_result = WAIT_OBJECT_0;
    if (event_value != 0) {
        wait_result = WaitForSingleObject(reinterpret_cast<HANDLE>(event_value),
                                          INFINITE);
    }

    // Every synchronous caller sets +0x10 before queueing work, and all five
    // completion paths converge on CompleteWwiseBlockingOperation. Poll only
    // when necessary; a valid event normally leaves at most the tiny interval
    // between SetEvent and the final flag store. If event allocation failed,
    // this retains stock's infinite-wait contract without waiting on NULL.
    unsigned int yields = 0;
    while (IsWwiseBlockingOperationPending(
        static_cast<std::uint8_t>(_InterlockedCompareExchange8(
            reinterpret_cast<volatile char*>(owner + 0x10), 0, 0)))) {
        if (yields < 64) {
            ++yields;
            SwitchToThread();
        } else {
            Sleep(1);
        }
    }
    return wait_result;
}

std::uint8_t* AllocateRelayPageNear(std::uintptr_t module_base) {
    constexpr std::uintptr_t kFirstDistance = 0x04000000;
    constexpr std::uintptr_t kDistanceStep = 0x01000000;
    constexpr std::uintptr_t kMaximumDistance = 0x60000000;
    constexpr std::size_t kRelayPageSize = 4096;
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const std::uintptr_t granularity = system_info.dwAllocationGranularity;
    if (module_base == 0 || granularity == 0) {
        return nullptr;
    }

    const auto align_down = [granularity](std::uintptr_t value) {
        return value - (value % granularity);
    };
    for (std::uintptr_t distance = kFirstDistance; distance <= kMaximumDistance;
         distance += kDistanceStep) {
        const std::array<std::uintptr_t, 2> candidates{
            module_base <=
                    (std::numeric_limits<std::uintptr_t>::max)() - distance
                ? align_down(module_base + distance)
                : 0,
            module_base > distance ? align_down(module_base - distance) : 0};
        for (const std::uintptr_t candidate : candidates) {
            if (candidate == 0) {
                continue;
            }
            void* const page =
                VirtualAlloc(reinterpret_cast<void*>(candidate), kRelayPageSize,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (page != nullptr) {
                return static_cast<std::uint8_t*>(page);
            }
        }
    }
    return nullptr;
}

std::optional<std::uintptr_t> BuildVramUnlockRelay(std::uintptr_t module_base,
                                                   bool latest_steam_layout) {
    for (const VramUnlockRelay& relay : g_vram_unlock_relays) {
        if (relay.ready && relay.module_base == module_base &&
            relay.latest_steam == latest_steam_layout) {
            return relay.entry;
        }
    }

    VramUnlockRelay* free_slot = nullptr;
    for (VramUnlockRelay& relay : g_vram_unlock_relays) {
        if (!relay.ready) {
            free_slot = &relay;
            break;
        }
    }
    if (free_slot == nullptr) {
        return std::nullopt;
    }

    std::uint8_t* const page = AllocateRelayPageNear(module_base);
    if (page == nullptr) {
        return std::nullopt;
    }

    // The replaced call receives the pool in RCX and keeps it in nonvolatile
    // RBX. Preserve the stock final validation, then release exactly the one
    // recursive CRITICAL_SECTION acquisition made by ForceEmptyPool.
    constexpr std::array<std::uint8_t, 37> relay_template{
        0x48, 0x83, 0xEC, 0x28,                          // sub rsp, 28h
        0x48, 0xB8,                                      // mov rax, validation
        0,    0,    0,    0,    0, 0, 0, 0, 0xFF, 0xD0,  // call rax
        0x48, 0x8D, 0x4B, 0x10,                          // lea rcx, [rbx+10h]
        0x48, 0xB8,  // mov rax, LeaveCriticalSection
        0,    0,    0,    0,    0, 0, 0, 0, 0xFF, 0xD0,  // call rax
        0x48, 0x83, 0xC4, 0x28,                          // add rsp, 28h
        0xC3};                                           // ret
    auto relay_bytes = relay_template;
    const std::uintptr_t validation =
        module_base + (latest_steam_layout
                           ? kLatestVramPoolValidationFunctionRva
                           : kLegacyVramPoolValidationFunctionRva);
    const std::uintptr_t leave_critical_section =
        reinterpret_cast<std::uintptr_t>(&LeaveCriticalSection);
    std::memcpy(relay_bytes.data() + 6, &validation, sizeof(validation));
    std::memcpy(relay_bytes.data() + 22, &leave_critical_section,
                sizeof(leave_critical_section));
    std::memcpy(page, relay_bytes.data(), relay_bytes.size());

    DWORD old_protection = 0;
    if (!VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old_protection) ||
        !FlushInstructionCache(GetCurrentProcess(), page, relay_bytes.size())) {
        VirtualFree(page, 0, MEM_RELEASE);
        return std::nullopt;
    }

    *free_slot = VramUnlockRelay{page, reinterpret_cast<std::uintptr_t>(page),
                                 module_base, latest_steam_layout, true};
    return free_slot->entry;
}

std::optional<std::uintptr_t> BuildVramCapacityRelay(std::uintptr_t module_base,
                                                     bool latest_steam_layout) {
    for (const VramCapacityRelay& relay : g_vram_capacity_relays) {
        if (relay.ready && relay.module_base == module_base &&
            relay.latest_steam == latest_steam_layout) {
            return relay.entry;
        }
    }

    VramCapacityRelay* free_slot = nullptr;
    for (VramCapacityRelay& relay : g_vram_capacity_relays) {
        if (!relay.ready) {
            free_slot = &relay;
            break;
        }
    }
    if (free_slot == nullptr) {
        return std::nullopt;
    }

    std::uint8_t* const page = AllocateRelayPageNear(module_base);
    if (page == nullptr) {
        return std::nullopt;
    }

    // Preserve RCX and RDX because the replaced MOV/SHR pair changed only RAX
    // and flags. Search the adapter records by the selected IDXGIAdapter1
    // pointer, then convert the complete SIZE_T byte count to MiB. If display
    // initialization is not complete, retain the stock low-DWORD fallback.
    constexpr std::array<std::uint8_t, 100> relay_template{
        0x51,        // push rcx
        0x52,        // push rdx
        0x48, 0xB8,  // mov rax, adapter-array address
        0,    0,    0,    0,    0,    0,    0,
        0,    0x48, 0x8B,
        0x00,        // mov rax, [rax]
        0x48, 0xBA,  // mov rdx, adapter-count address
        0,    0,    0,    0,    0,    0,    0,
        0,    0x8B, 0x12,  // mov edx, [rdx]
        0x48, 0xB9,        // mov rcx, selected-adapter address
        0,    0,    0,    0,    0,    0,    0,
        0,    0x48, 0x8B,
        0x09,                                      // mov rcx, [rcx]
        0x48, 0x85, 0xC0,                          // test rax, rax
        0x74, 0x17,                                // jz fallback
        0x85, 0xD2,                                // test edx, edx
        0x74, 0x13,                                // jz fallback
        0x48, 0x39, 0x88, 0x38, 0x01, 0x00, 0x00,  // cmp [rax+138h], rcx
        0x74, 0x1C,                                // je found
        0x48, 0x05, 0x50, 0x01, 0x00, 0x00,        // add rax, 150h
        0xFF, 0xCA,                                // dec edx
        0x75, 0xED,                                // jnz search
        0x48, 0xB8,  // fallback: mov rax, low-DWORD address
        0,    0,    0,    0,    0,    0,    0,
        0,    0x8B, 0x00,                          // mov eax, [rax]
        0xC1, 0xE8, 0x14,                          // shr eax, 20
        0x5A,                                      // pop rdx
        0x59,                                      // pop rcx
        0xC3,                                      // ret
        0x48, 0x8B, 0x80, 0x10, 0x01, 0x00, 0x00,  // found: mov rax, [rax+110h]
        0x48, 0xC1, 0xE8, 0x14,                    // shr rax, 20
        0x5A,                                      // pop rdx
        0x59,                                      // pop rcx
        0xC3};                                     // ret
    auto relay_bytes = relay_template;
    const std::uintptr_t adapter_array = module_base + kAdapterArrayRva;
    const std::uintptr_t adapter_count = module_base + kAdapterCountRva;
    const std::uintptr_t selected_adapter =
        module_base + kSelectedAdapterInterfaceRva;
    const std::uintptr_t truncated_capacity =
        module_base + kTruncatedDedicatedVideoMemoryRva;
    std::memcpy(relay_bytes.data() + 4, &adapter_array, sizeof(adapter_array));
    std::memcpy(relay_bytes.data() + 17, &adapter_count, sizeof(adapter_count));
    std::memcpy(relay_bytes.data() + 29, &selected_adapter,
                sizeof(selected_adapter));
    std::memcpy(relay_bytes.data() + 70, &truncated_capacity,
                sizeof(truncated_capacity));
    std::memcpy(page, relay_bytes.data(), relay_bytes.size());

    DWORD old_protection = 0;
    if (!VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old_protection) ||
        !FlushInstructionCache(GetCurrentProcess(), page, relay_bytes.size())) {
        VirtualFree(page, 0, MEM_RELEASE);
        return std::nullopt;
    }

    *free_slot = VramCapacityRelay{page, reinterpret_cast<std::uintptr_t>(page),
                                   module_base, latest_steam_layout, true};
    return free_slot->entry;
}

std::optional<std::uintptr_t> BuildCharacterSurfaceRelay(
    std::uintptr_t module_base,
    bool latest_steam_layout,
    bool bridge_wetness) {
    for (const CharacterSurfaceRelay& relay : g_character_surface_relays) {
        if (relay.ready && relay.module_base == module_base &&
            relay.latest_steam == latest_steam_layout &&
            relay.bridge_wetness == bridge_wetness) {
            return relay.entry;
        }
    }

    CharacterSurfaceRelay* free_slot = nullptr;
    for (CharacterSurfaceRelay& relay : g_character_surface_relays) {
        if (!relay.ready) {
            free_slot = &relay;
            break;
        }
    }
    if (free_slot == nullptr) {
        return std::nullopt;
    }

    std::uint8_t* const page = AllocateRelayPageNear(module_base);
    if (page == nullptr) {
        return std::nullopt;
    }

    // This replaces one verified memcpy(destination, source, 0x40) call. XMM0
    // through XMM3 are volatile in the Windows x64 ABI, and the caller does
    // not consume the other volatile arguments after the call.
    std::array<std::uint8_t, 40> relay_bytes{
        0x0F, 0x10, 0x02,                    // movups xmm0, [rdx]
        0x0F, 0x10, 0x4A, 0x10,              // movups xmm1, [rdx+10h]
        0x0F, 0x10, 0x52, 0x20,              // movups xmm2, [rdx+20h]
        0x0F, 0x10, 0x5A, 0x30,              // movups xmm3, [rdx+30h]
        0x0F, 0x11, 0x01,                    // movups [rcx], xmm0
        0x0F, 0x11, 0x49, 0x10,              // movups [rcx+10h], xmm1
        0x0F, 0x11, 0x51, 0x20,              // movups [rcx+20h], xmm2
        0x0F, 0x11, 0x59, 0x30,              // movups [rcx+30h], xmm3
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90,  // optional wetness bridge
        0x48, 0x8B, 0xC1,                    // mov rax, rcx
        0xC3};                               // ret
    constexpr std::array<std::uint8_t, 6> wetness_bridge{
        0x8B, 0x41, 0x24, 0x89, 0x41, 0x38};  // [rcx+24h] -> [rcx+38h]
    if (bridge_wetness) {
        std::copy(wetness_bridge.begin(), wetness_bridge.end(),
                  relay_bytes.begin() + 30);
    }
    std::memcpy(page, relay_bytes.data(), relay_bytes.size());

    DWORD old_protection = 0;
    if (!VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old_protection) ||
        !FlushInstructionCache(GetCurrentProcess(), page, relay_bytes.size())) {
        VirtualFree(page, 0, MEM_RELEASE);
        return std::nullopt;
    }

    *free_slot = CharacterSurfaceRelay{
        page,           reinterpret_cast<std::uintptr_t>(page),
        module_base,    latest_steam_layout,
        bridge_wetness, true};
    return free_slot->entry;
}

ResourceFailureRelay* BuildResourceFailureRelays(std::uintptr_t module_base,
                                                 bool latest_steam_layout) {
    for (ResourceFailureRelay& relay : g_resource_failure_relays) {
        if (relay.ready && relay.module_base == module_base &&
            relay.latest_steam == latest_steam_layout) {
            return &relay;
        }
    }

    ResourceFailureRelay* free_slot = nullptr;
    for (ResourceFailureRelay& relay : g_resource_failure_relays) {
        if (!relay.ready) {
            free_slot = &relay;
            break;
        }
    }
    if (free_slot == nullptr) {
        return nullptr;
    }

    std::uint8_t* const page = AllocateRelayPageNear(module_base);
    if (page == nullptr) {
        return nullptr;
    }

    constexpr std::size_t kLoadedChunkErrorOffset = 0;
    constexpr std::size_t kLoadedChunkFileErrorOffset = 64;
    constexpr std::size_t kLoadedChunkFileSizeErrorOffset = 128;
    constexpr std::size_t kSynchronousIndexedReadErrorOffset = 192;
    constexpr std::size_t kSynchronousLooseReadErrorOffset = 256;
    constexpr std::size_t kSynchronousLooseOpenErrorOffset = 320;
    constexpr std::size_t kSynchronousLooseSizeErrorOffset = 384;
    constexpr std::size_t kQcmpFailureCopyGuardOffset = 448;
    constexpr std::size_t kCompressedXmlAllocationGuardOffset = 512;
    constexpr std::size_t kCompressedXmlFinalizeGuardOffset = 576;
    const std::uintptr_t loaded_chunk_error =
        reinterpret_cast<std::uintptr_t>(page + kLoadedChunkErrorOffset);
    const std::uintptr_t loaded_chunk_file_error =
        reinterpret_cast<std::uintptr_t>(page + kLoadedChunkFileErrorOffset);
    const std::uintptr_t loaded_chunk_file_size_error =
        reinterpret_cast<std::uintptr_t>(page +
                                         kLoadedChunkFileSizeErrorOffset);
    const std::uintptr_t synchronous_indexed_read_error =
        reinterpret_cast<std::uintptr_t>(page +
                                         kSynchronousIndexedReadErrorOffset);
    const std::uintptr_t synchronous_loose_read_error =
        reinterpret_cast<std::uintptr_t>(page +
                                         kSynchronousLooseReadErrorOffset);
    const std::uintptr_t synchronous_loose_open_error =
        reinterpret_cast<std::uintptr_t>(page +
                                         kSynchronousLooseOpenErrorOffset);
    const std::uintptr_t synchronous_loose_size_error =
        reinterpret_cast<std::uintptr_t>(page +
                                         kSynchronousLooseSizeErrorOffset);
    const std::uintptr_t qcmp_failure_copy_guard =
        reinterpret_cast<std::uintptr_t>(page + kQcmpFailureCopyGuardOffset);
    const std::uintptr_t compressed_xml_allocation_guard =
        reinterpret_cast<std::uintptr_t>(
            page + kCompressedXmlAllocationGuardOffset);
    const std::uintptr_t compressed_xml_finalize_guard =
        reinterpret_cast<std::uintptr_t>(page +
                                         kCompressedXmlFinalizeGuardOffset);
    const std::uintptr_t work_pending = module_base + kResourceWorkPendingRva;

    // The two error-log calls are reached only after the callback has consumed
    // its operation and released all waiters when the outstanding count reaches
    // zero. Preserve the original logger arguments by tail-jumping to it. Only
    // LOADING resources with no operation left are made retryable; unexpected
    // late callbacks must not demote an already-loaded resource.
    const auto emit_log_relay = [&](std::size_t offset,
                                    std::uintptr_t original_logger) -> bool {
        std::array<std::uint8_t, 40> bytes{
            0x83, 0xBB, 0xB0, 0x00, 0x00, 0x00, 0x01,  // cmp [rbx+B0h], 1
            0x75, 0x13,                                // jne publish
            0x83, 0xBB, 0xAC, 0x00, 0x00, 0x00, 0x00,  // cmp [rbx+ACh], 0
            0x75, 0x0A,                                // jne publish
            0xC7, 0x83, 0xB0, 0x00, 0x00, 0x00,        // mov [rbx+B0h], 0
            0x00, 0x00, 0x00, 0x00, 0xC6, 0x05, 0x00,
            0x00, 0x00, 0x00, 0x01,         // mov byte [work], 1
            0xE9, 0x00, 0x00, 0x00, 0x00};  // jmp original logger
        const std::uintptr_t entry =
            reinterpret_cast<std::uintptr_t>(page + offset);
        const auto pending_displacement =
            ComputeRelativeBranchDisplacement(entry + 28, 7, work_pending);
        const auto logger_displacement =
            ComputeRelativeBranchDisplacement(entry + 35, 5, original_logger);
        if (!pending_displacement.has_value() ||
            !logger_displacement.has_value()) {
            return false;
        }
        std::memcpy(bytes.data() + 30, &*pending_displacement,
                    sizeof(*pending_displacement));
        std::memcpy(bytes.data() + 36, &*logger_displacement,
                    sizeof(*logger_displacement));
        std::memcpy(page + offset, bytes.data(), bytes.size());
        return true;
    };

    const std::uintptr_t loaded_chunk_logger =
        module_base + (latest_steam_layout ? kLatestLoadedChunkErrorLoggerRva
                                           : kLegacyLoadedChunkErrorLoggerRva);
    const std::uintptr_t loaded_chunk_file_logger =
        module_base + (latest_steam_layout
                           ? kLatestLoadedChunkFileErrorLoggerRva
                           : kLegacyLoadedChunkFileErrorLoggerRva);
    if (!emit_log_relay(kLoadedChunkErrorOffset, loaded_chunk_logger) ||
        !emit_log_relay(kLoadedChunkFileErrorOffset,
                        loaded_chunk_file_logger)) {
        VirtualFree(page, 0, MEM_RELEASE);
        return nullptr;
    }

    // The file-size failure path has already proved that no operation remains.
    // Keep every reference, clear stale payload metadata, return the object to
    // IDLE, and replace the call's return address with the stock epilogue. The
    // original waiter cleanup still runs, but the unsafe ref decrement and
    // unconditional destruction immediately following it are skipped.
    std::array<std::uint8_t, 41> size_bytes{
        0x33, 0xC0,                                      // xor eax, eax
        0x48, 0x89, 0x81, 0xA0, 0x00, 0x00, 0x00,        // mov [rcx+A0h], rax
        0x89, 0x81, 0xB0, 0x00, 0x00, 0x00,              // mov [rcx+B0h], eax
        0xC6, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01,        // mov byte [work], 1
        0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // mov rax, epilogue
        0x00, 0x00, 0x48, 0x89, 0x04, 0x24,              // mov [rsp], rax
        0xE9, 0x00, 0x00, 0x00, 0x00};                   // jmp waiter cleanup
    const std::uintptr_t size_epilogue =
        module_base + (latest_steam_layout
                           ? kLatestLoadedChunkFileSizeEpilogueRva
                           : kLegacyLoadedChunkFileSizeEpilogueRva);
    const std::uintptr_t release_waiters =
        module_base + (latest_steam_layout ? kLatestReleaseResourceWaitersRva
                                           : kLegacyReleaseResourceWaitersRva);
    const auto size_pending_displacement = ComputeRelativeBranchDisplacement(
        loaded_chunk_file_size_error + 15, 7, work_pending);
    const auto cleanup_displacement = ComputeRelativeBranchDisplacement(
        loaded_chunk_file_size_error + 36, 5, release_waiters);
    if (!size_pending_displacement.has_value() ||
        !cleanup_displacement.has_value()) {
        VirtualFree(page, 0, MEM_RELEASE);
        return nullptr;
    }
    std::memcpy(size_bytes.data() + 17, &*size_pending_displacement,
                sizeof(*size_pending_displacement));
    std::memcpy(size_bytes.data() + 24, &size_epilogue, sizeof(size_epilogue));
    std::memcpy(size_bytes.data() + 37, &*cleanup_displacement,
                sizeof(*cleanup_displacement));
    std::memcpy(page + kLoadedChunkFileSizeErrorOffset, size_bytes.data(),
                size_bytes.size());

    // Indexed loads can finish with either a direct read count or a larger
    // decompressed payload size, so only the qFile/decompressor failure
    // sentinel is invalid at this shared finalizer. Short device reads are
    // normalized to that sentinel by ReadFileDevice before reaching here.
    // Loose files are never decompressed on this path and retain the exact
    // requested byte count in R14, allowing a second defensive equality check.
    const std::uintptr_t resource_finalize =
        module_base + (latest_steam_layout ? kLatestResourceFinalizeRva
                                           : kLegacyResourceFinalizeRva);
    std::array<std::uint8_t, 38> indexed_read_bytes{
        0x48, 0x83, 0xB9, 0xA0, 0x00, 0x00, 0x00, 0xFF,  // cmp [rcx+A0h], -1
        0x75, 0x17,                                      // jne stock finalizer
        0x33, 0xC0,                                      // xor eax, eax
        0x48, 0x89, 0x81, 0xA0, 0x00, 0x00, 0x00,        // mov [rcx+A0h], rax
        0x89, 0x81, 0xB0, 0x00, 0x00, 0x00,              // mov [rcx+B0h], eax
        0xC6, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01,        // mov byte [work], 1
        0xC3,                                            // ret
        0xE9, 0x00, 0x00, 0x00, 0x00};                   // jmp stock finalizer
    const auto indexed_pending_displacement = ComputeRelativeBranchDisplacement(
        synchronous_indexed_read_error + 25, 7, work_pending);
    const auto indexed_finalize_displacement = ComputeRelativeBranchDisplacement(
        synchronous_indexed_read_error + 33, 5, resource_finalize);
    if (!indexed_pending_displacement.has_value() ||
        !indexed_finalize_displacement.has_value()) {
        VirtualFree(page, 0, MEM_RELEASE);
        return nullptr;
    }
    std::memcpy(indexed_read_bytes.data() + 27,
                &*indexed_pending_displacement,
                sizeof(*indexed_pending_displacement));
    std::memcpy(indexed_read_bytes.data() + 34,
                &*indexed_finalize_displacement,
                sizeof(*indexed_finalize_displacement));
    std::memcpy(page + kSynchronousIndexedReadErrorOffset,
                indexed_read_bytes.data(),
                indexed_read_bytes.size());

    std::array<std::uint8_t, 37> loose_read_bytes{
        0x4C, 0x39, 0xB1, 0xA0, 0x00, 0x00, 0x00,        // cmp [rcx+A0h], r14
        0x74, 0x17,                                      // je stock finalizer
        0x33, 0xC0,                                      // xor eax, eax
        0x48, 0x89, 0x81, 0xA0, 0x00, 0x00, 0x00,        // mov [rcx+A0h], rax
        0x89, 0x81, 0xB0, 0x00, 0x00, 0x00,              // mov [rcx+B0h], eax
        0xC6, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01,        // mov byte [work], 1
        0xC3,                                            // ret
        0xE9, 0x00, 0x00, 0x00, 0x00};                   // jmp stock finalizer
    const auto loose_read_pending_displacement =
        ComputeRelativeBranchDisplacement(synchronous_loose_read_error + 24, 7,
                                          work_pending);
    const auto loose_read_finalize_displacement =
        ComputeRelativeBranchDisplacement(synchronous_loose_read_error + 32, 5,
                                          resource_finalize);
    if (!loose_read_pending_displacement.has_value() ||
        !loose_read_finalize_displacement.has_value()) {
        VirtualFree(page, 0, MEM_RELEASE);
        return nullptr;
    }
    std::memcpy(loose_read_bytes.data() + 26,
                &*loose_read_pending_displacement,
                sizeof(*loose_read_pending_displacement));
    std::memcpy(loose_read_bytes.data() + 33,
                &*loose_read_finalize_displacement,
                sizeof(*loose_read_finalize_displacement));
    std::memcpy(page + kSynchronousLooseReadErrorOffset,
                loose_read_bytes.data(),
                loose_read_bytes.size());

    // The loose-file helper has two earlier false-return exits. A failed open
    // skips the stock unwinder while leaving the resource LOADING; an invalid
    // size is instead mislabeled LOADED without a payload. Normalize both to
    // the same retryable IDLE state used by failed reads.
    std::array<std::uint8_t, 27> loose_open_bytes{
        0x33, 0xC0,                                // xor eax, eax
        0x48, 0x89, 0x87, 0xA0, 0x00, 0x00, 0x00,  // mov [rdi+A0h], rax
        0x89, 0x87, 0xB0, 0x00, 0x00, 0x00,        // mov [rdi+B0h], eax
        0xC6, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01,  // mov byte [work], 1
        0xE9, 0x00, 0x00, 0x00, 0x00};             // jmp stock epilogue
    const std::uintptr_t loose_open_epilogue =
        module_base + (latest_steam_layout
                           ? kLatestSynchronousLooseOpenFailureEpilogueRva
                           : kLegacySynchronousLooseOpenFailureEpilogueRva);
    const auto loose_open_pending_displacement =
        ComputeRelativeBranchDisplacement(synchronous_loose_open_error + 15, 7,
                                          work_pending);
    const auto loose_open_epilogue_displacement =
        ComputeRelativeBranchDisplacement(synchronous_loose_open_error + 22, 5,
                                          loose_open_epilogue);
    if (!loose_open_pending_displacement.has_value() ||
        !loose_open_epilogue_displacement.has_value()) {
        VirtualFree(page, 0, MEM_RELEASE);
        return nullptr;
    }
    std::memcpy(loose_open_bytes.data() + 17, &*loose_open_pending_displacement,
                sizeof(*loose_open_pending_displacement));
    std::memcpy(loose_open_bytes.data() + 23,
                &*loose_open_epilogue_displacement,
                sizeof(*loose_open_epilogue_displacement));
    std::memcpy(page + kSynchronousLooseOpenErrorOffset,
                loose_open_bytes.data(), loose_open_bytes.size());

    std::array<std::uint8_t, 23> loose_size_bytes{
        0x33, 0xC0,                                // xor eax, eax
        0x48, 0x89, 0x87, 0xA0, 0x00, 0x00, 0x00,  // mov [rdi+A0h], rax
        0x89, 0x87, 0xB0, 0x00, 0x00, 0x00,        // mov [rdi+B0h], eax
        0xC6, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01,  // mov byte [work], 1
        0xC3};                                     // ret
    const auto loose_size_pending_displacement =
        ComputeRelativeBranchDisplacement(synchronous_loose_size_error + 15, 7,
                                          work_pending);
    if (!loose_size_pending_displacement.has_value()) {
        VirtualFree(page, 0, MEM_RELEASE);
        return nullptr;
    }
    std::memcpy(loose_size_bytes.data() + 17, &*loose_size_pending_displacement,
                sizeof(*loose_size_pending_displacement));
    std::memcpy(page + kSynchronousLooseSizeErrorOffset,
                loose_size_bytes.data(), loose_size_bytes.size());

    // The buffered qFile helper narrows the decompressor's UINT64_MAX failure
    // sentinel to 0xFFFFFFFF and passes it directly to its final buffer copy.
    // Skip that copy on failure; otherwise one malformed QCMP stream turns a
    // controlled decode rejection into a four-gigabyte out-of-bounds copy.
    std::array<std::uint8_t, 12> qcmp_copy_bytes{
        0x41, 0x83, 0xF8, 0xFF,        // cmp r8d, -1
        0x74, 0x05,                    // je return
        0xE9, 0x00, 0x00, 0x00, 0x00,  // jmp stock buffer copy
        0xC3};                         // return
    const std::uintptr_t buffer_copy =
        module_base + (latest_steam_layout ? kLatestBufferCopyRva
                                           : kLegacyBufferCopyRva);
    const auto buffer_copy_displacement = ComputeRelativeBranchDisplacement(
        qcmp_failure_copy_guard + 6, 5, buffer_copy);
    if (!buffer_copy_displacement.has_value()) {
        VirtualFree(page, 0, MEM_RELEASE);
        return nullptr;
    }
    std::memcpy(qcmp_copy_bytes.data() + 7, &*buffer_copy_displacement,
                sizeof(*buffer_copy_displacement));
    std::memcpy(page + kQcmpFailureCopyGuardOffset, qcmp_copy_bytes.data(),
                qcmp_copy_bytes.size());

    // The compressed-XML loader forms allocation_size = output_size + 0x80
    // in ECX. A wrapped result is therefore in [0, 0x7F]. Reject that range
    // before the allocator sees a tiny buffer paired with the original large
    // output capacity in EBX.
    std::array<std::uint8_t, 16> xml_allocation_bytes{
        0x81, 0xF9, 0x80, 0x00, 0x00, 0x00,  // cmp ecx, 80h
        0x72, 0x05,                          // jb return_null
        0xE9, 0x00, 0x00, 0x00, 0x00,        // jmp stock allocator
        0x33, 0xC0,                          // xor eax, eax
        0xC3};                               // ret
    const std::uintptr_t resource_allocator =
        module_base + (latest_steam_layout ? kLatestResourceAllocatorRva
                                           : kLegacyResourceAllocatorRva);
    const auto resource_allocator_displacement =
        ComputeRelativeBranchDisplacement(compressed_xml_allocation_guard + 8,
                                          5, resource_allocator);
    if (!resource_allocator_displacement.has_value()) {
        VirtualFree(page, 0, MEM_RELEASE);
        return nullptr;
    }
    std::memcpy(xml_allocation_bytes.data() + 9,
                &*resource_allocator_displacement,
                sizeof(*resource_allocator_displacement));
    std::memcpy(page + kCompressedXmlAllocationGuardOffset,
                xml_allocation_bytes.data(), xml_allocation_bytes.size());

    // Stock ignores the QCMP result and always terminates through RDI. Accept
    // only a non-null allocation and an exact decoded byte count. Otherwise
    // release the temporary allocation, return null, and let the caller use
    // its existing loose-XML fallback.
    std::array<std::uint8_t, 55> xml_finalize_bytes{
        0x48, 0x85, 0xFF,                    // test rdi, rdi
        0x74, 0x2B,                          // jz return_null
        0x48, 0x83, 0xF8, 0xFF,              // cmp rax, -1
        0x74, 0x0E,                          // je release
        0x48, 0x3B, 0xC3,                    // cmp rax, rbx
        0x75, 0x09,                          // jne release
        0xC6, 0x04, 0x3B, 0x00,              // mov byte [rdi+rbx], 0
        0xE9, 0x00, 0x00, 0x00, 0x00,        // jmp cleanup
        0x48, 0x83, 0xEC, 0x20,              // release: sub rsp, 20h
        0x48, 0x8B, 0x0D, 0x00, 0x00, 0x00,  // mov rcx, [allocator]
        0x00,
        0x48, 0x8B, 0xD7,                    // mov rdx, rdi
        0xE8, 0x00, 0x00, 0x00, 0x00,        // call resource free
        0x48, 0x83, 0xC4, 0x20,              // add rsp, 20h
        0x33, 0xFF,                          // return_null: xor edi, edi
        0xE9, 0x00, 0x00, 0x00, 0x00};       // jmp cleanup
    const std::uintptr_t allocator_instance =
        module_base + kResourceAllocatorInstanceRva;
    const std::uintptr_t resource_free =
        module_base + (latest_steam_layout ? kLatestResourceFreeRva
                                           : kLegacyResourceFreeRva);
    const std::uintptr_t xml_cleanup =
        module_base + (latest_steam_layout ? kLatestCompressedXmlCleanupRva
                                           : kLegacyCompressedXmlCleanupRva);
    const auto xml_success_displacement = ComputeRelativeBranchDisplacement(
        compressed_xml_finalize_guard + 20, 5, xml_cleanup);
    const auto allocator_instance_displacement =
        ComputeRelativeBranchDisplacement(compressed_xml_finalize_guard + 29,
                                          7, allocator_instance);
    const auto resource_free_displacement = ComputeRelativeBranchDisplacement(
        compressed_xml_finalize_guard + 39, 5, resource_free);
    const auto xml_failure_displacement = ComputeRelativeBranchDisplacement(
        compressed_xml_finalize_guard + 50, 5, xml_cleanup);
    if (!xml_success_displacement.has_value() ||
        !allocator_instance_displacement.has_value() ||
        !resource_free_displacement.has_value() ||
        !xml_failure_displacement.has_value()) {
        VirtualFree(page, 0, MEM_RELEASE);
        return nullptr;
    }
    std::memcpy(xml_finalize_bytes.data() + 21, &*xml_success_displacement,
                sizeof(*xml_success_displacement));
    std::memcpy(xml_finalize_bytes.data() + 32,
                &*allocator_instance_displacement,
                sizeof(*allocator_instance_displacement));
    std::memcpy(xml_finalize_bytes.data() + 40,
                &*resource_free_displacement,
                sizeof(*resource_free_displacement));
    std::memcpy(xml_finalize_bytes.data() + 51, &*xml_failure_displacement,
                sizeof(*xml_failure_displacement));
    std::memcpy(page + kCompressedXmlFinalizeGuardOffset,
                xml_finalize_bytes.data(), xml_finalize_bytes.size());

    DWORD old_protection = 0;
    if (!VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old_protection) ||
        !FlushInstructionCache(GetCurrentProcess(), page, 4096)) {
        VirtualFree(page, 0, MEM_RELEASE);
        return nullptr;
    }

    *free_slot = ResourceFailureRelay{page,
                                      loaded_chunk_error,
                                      loaded_chunk_file_error,
                                      loaded_chunk_file_size_error,
                                      synchronous_indexed_read_error,
                                      synchronous_loose_read_error,
                                      synchronous_loose_open_error,
                                      synchronous_loose_size_error,
                                      qcmp_failure_copy_guard,
                                      compressed_xml_allocation_guard,
                                      compressed_xml_finalize_guard,
                                      module_base,
                                      latest_steam_layout,
                                      true};
    return free_slot;
}

void WriteAbsoluteJump(std::uint8_t* destination, std::uintptr_t target) {
    constexpr std::array<std::uint8_t, 6> jump{0xFF, 0x25, 0x00,
                                               0x00, 0x00, 0x00};
    std::memcpy(destination, jump.data(), jump.size());
    std::memcpy(destination + jump.size(), &target, sizeof(target));
}

bool BuildRelayLayout(std::uintptr_t module_base, bool latest_steam_layout) {
    if (g_relay_layout.ready) {
        return g_relay_layout.latest_steam == latest_steam_layout;
    }

    std::uint8_t* const page = AllocateRelayPageNear(module_base);
    if (page == nullptr) {
        return false;
    }

    constexpr std::size_t kSaveParserRelayOffset = 0;
    constexpr std::size_t kCreateThreadRelayOffset = 16;
    constexpr std::size_t kSaveHeaderRelayOffset = 32;
    constexpr std::size_t kCreateIoThreadRelayOffset = 64;
    constexpr std::size_t kCreateTaskThreadRelayOffset = 80;
    constexpr std::size_t kCreateBankManagerThreadRelayOffset = 96;
    constexpr std::size_t kCreateBankShutdownFenceEventRelayOffset = 112;
    constexpr std::size_t kCompleteWwiseBlockingOperationRelayOffset = 128;
    constexpr std::size_t kWaitWwiseBlockingOperationRelayOffset = 144;
    const std::uintptr_t save_parser_relay =
        reinterpret_cast<std::uintptr_t>(page + kSaveParserRelayOffset);
    const std::uintptr_t create_thread_relay =
        reinterpret_cast<std::uintptr_t>(page + kCreateThreadRelayOffset);
    const std::uintptr_t save_header_relay =
        reinterpret_cast<std::uintptr_t>(page + kSaveHeaderRelayOffset);
    const std::uintptr_t create_io_thread_relay =
        reinterpret_cast<std::uintptr_t>(page + kCreateIoThreadRelayOffset);
    const std::uintptr_t create_task_thread_relay =
        reinterpret_cast<std::uintptr_t>(page + kCreateTaskThreadRelayOffset);
    const std::uintptr_t create_bank_manager_thread_relay =
        reinterpret_cast<std::uintptr_t>(page +
                                         kCreateBankManagerThreadRelayOffset);
    const std::uintptr_t create_bank_shutdown_fence_event_relay =
        reinterpret_cast<std::uintptr_t>(
            page + kCreateBankShutdownFenceEventRelayOffset);
    const std::uintptr_t complete_wwise_blocking_operation_relay =
        reinterpret_cast<std::uintptr_t>(
            page + kCompleteWwiseBlockingOperationRelayOffset);
    const std::uintptr_t wait_wwise_blocking_operation_relay =
        reinterpret_cast<std::uintptr_t>(
            page + kWaitWwiseBlockingOperationRelayOffset);

    WriteAbsoluteJump(page + kSaveParserRelayOffset,
                      reinterpret_cast<std::uintptr_t>(&SafeDeserializeSave));
    WriteAbsoluteJump(
        page + kCreateThreadRelayOffset,
        reinterpret_cast<std::uintptr_t>(&CreateThreadSentinelAdapter));
    WriteAbsoluteJump(
        page + kCreateIoThreadRelayOffset,
        reinterpret_cast<std::uintptr_t>(&CreateIoThreadWithEventGuard));
    WriteAbsoluteJump(
        page + kCreateTaskThreadRelayOffset,
        reinterpret_cast<std::uintptr_t>(&CreateTaskThreadWithEventGuard));
    WriteAbsoluteJump(page + kCreateBankManagerThreadRelayOffset,
                      reinterpret_cast<std::uintptr_t>(
                          &CreateBankManagerThreadWithEventGuard));
    WriteAbsoluteJump(page + kCreateBankShutdownFenceEventRelayOffset,
                      reinterpret_cast<std::uintptr_t>(
                          &CreateBankShutdownFenceEventWithReserve));
    WriteAbsoluteJump(
        page + kCompleteWwiseBlockingOperationRelayOffset,
        reinterpret_cast<std::uintptr_t>(&CompleteWwiseBlockingOperation));
    WriteAbsoluteJump(
        page + kWaitWwiseBlockingOperationRelayOffset,
        reinterpret_cast<std::uintptr_t>(&WaitForWwiseBlockingOperation));

    // test rdx,rdx; jz fail; cmp r15d,0xB8; jl fail; jmp stock; fail: xor
    // eax,eax; ret
    constexpr std::array<std::uint8_t, 22> header_template{
        0x48, 0x85, 0xD2, 0x74, 0x0E, 0x41, 0x81, 0xFF, 0xB8, 0x00, 0x00,
        0x00, 0x7C, 0x05, 0xE9, 0x00, 0x00, 0x00, 0x00, 0x33, 0xC0, 0xC3};
    auto header_relay = header_template;
    const std::uintptr_t stock_header =
        module_base + (latest_steam_layout ? kLatestSaveHeaderFunctionRva
                                           : kLegacySaveHeaderFunctionRva);
    const auto header_displacement = ComputeRelativeBranchDisplacement(
        save_header_relay + 14, 5, stock_header);
    if (!header_displacement.has_value()) {
        VirtualFree(page, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(header_relay.data() + 15, &*header_displacement,
                sizeof(*header_displacement));
    std::memcpy(page + kSaveHeaderRelayOffset, header_relay.data(),
                header_relay.size());

    DWORD old_protection = 0;
    if (!VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old_protection) ||
        !FlushInstructionCache(GetCurrentProcess(), page, 4096)) {
        // No call site is published until the layout is committed below, so
        // this page is still exclusively ours and can be released on a seal
        // failure.  Keeping failed bootstrap allocations around would leak a
        // page on every retry.
        VirtualFree(page, 0, MEM_RELEASE);
        return false;
    }

    g_relay_layout = RelayLayout{page,
                                 save_parser_relay,
                                 save_header_relay,
                                 create_thread_relay,
                                 create_io_thread_relay,
                                 create_task_thread_relay,
                                 create_bank_manager_thread_relay,
                                 create_bank_shutdown_fence_event_relay,
                                 complete_wwise_blocking_operation_relay,
                                 wait_wwise_blocking_operation_relay,
                                 latest_steam_layout,
                                 true};
    return true;
}

template <std::size_t Size>
std::optional<std::array<std::uint8_t, Size>> MakeRelativeCall(
    std::uintptr_t call_site,
    std::uintptr_t relay) {
    static_assert(Size >= 5);
    const auto displacement =
        ComputeRelativeBranchDisplacement(call_site, 5, relay);
    if (!displacement.has_value()) {
        return std::nullopt;
    }
    std::array<std::uint8_t, Size> replacement{};
    replacement.fill(0x90);
    replacement[0] = 0xE8;
    std::memcpy(replacement.data() + 1, &*displacement, sizeof(*displacement));
    return replacement;
}

template <std::size_t Size>
std::optional<std::array<std::uint8_t, Size>> MakeRelativeJump(
    std::uintptr_t jump_site,
    std::uintptr_t relay) {
    static_assert(Size >= 5);
    const auto displacement =
        ComputeRelativeBranchDisplacement(jump_site, 5, relay);
    if (!displacement.has_value()) {
        return std::nullopt;
    }
    std::array<std::uint8_t, Size> replacement{};
    replacement.fill(0x90);
    replacement[0] = 0xE9;
    std::memcpy(replacement.data() + 1, &*displacement, sizeof(*displacement));
    return replacement;
}

std::optional<std::array<std::uint8_t, 6>> MakeRelativeZeroJump(
    std::uintptr_t jump_site,
    std::uintptr_t relay) {
    const auto displacement =
        ComputeRelativeBranchDisplacement(jump_site, 6, relay);
    if (!displacement.has_value()) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 6> replacement{0x0F, 0x84, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(replacement.data() + 2, &*displacement, sizeof(*displacement));
    return replacement;
}

template <std::size_t Size>
bool ApplyTransactionalPatch(
    const char* name,
    std::uintptr_t address,
    const std::array<std::uint8_t, Size>& expected,
    const std::array<std::uint8_t, Size>& replacement) {
    const auto result =
        g_static_patches.Apply(name, address, expected, replacement);
    LogPatchResult(name, address, result);
    return result == runtime_patch::ApplyResult::Applied ||
           result == runtime_patch::ApplyResult::AlreadyApplied;
}

template <std::size_t Size>
bool IsPatchable(std::uintptr_t address,
                 const std::array<std::uint8_t, Size>& expected,
                 const std::array<std::uint8_t, Size>& replacement) {
    return runtime_patch::MatchesBytes(address, expected) ||
           runtime_patch::MatchesBytes(address, replacement);
}

bool ApplyRawMouseInput(std::uintptr_t module_base) {
    auto forced_signature = kRawMouseOptionSignature;
    std::copy(kForceRawMouseOption.begin(), kForceRawMouseOption.end(),
              forced_signature.begin() + kRawMouseOptionReadOffset);
    const auto signature =
        ResolveUniquePattern(module_base, "raw_mouse_input",
                             kRawMouseOptionSignature, forced_signature,
                             PatternMemoryClass::Executable);
    if (!signature.has_value()) {
        return false;
    }

    const std::uintptr_t store_address = *signature + kRawMouseStateStoreOffset;
    MEMORY_BASIC_INFORMATION store_memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(store_address),
                     &store_memory,
                     sizeof(store_memory)) != sizeof(store_memory) ||
        reinterpret_cast<std::uintptr_t>(store_memory.AllocationBase) !=
            module_base ||
        store_memory.State != MEM_COMMIT ||
        !IsReadableProtection(store_memory.Protect)) {
        log::Warn(
            "engine_patch group=raw_mouse_input disabled=1 "
            "reason=state_store_unreadable");
        return false;
    }
    const std::uintptr_t store_region_end =
        reinterpret_cast<std::uintptr_t>(store_memory.BaseAddress) +
        store_memory.RegionSize;
    constexpr std::size_t kStateStoreSize = 6;
    if (store_region_end < store_address ||
        store_region_end - store_address < kStateStoreSize ||
        !runtime_patch::MatchesBytes(store_address,
                                     kRawMouseStateStoreOpcode)) {
        log::Warn(
            "engine_patch group=raw_mouse_input disabled=1 "
            "reason=state_store_mismatch");
        return false;
    }

    std::int32_t displacement = 0;
    std::memcpy(&displacement,
                reinterpret_cast<const void*>(store_address +
                                              kRawMouseStateStoreOpcode.size()),
                sizeof(displacement));
    if (store_address >
        (std::numeric_limits<std::uintptr_t>::max)() - kStateStoreSize) {
        log::Warn(
            "engine_patch group=raw_mouse_input disabled=1 "
            "reason=state_address_overflow");
        return false;
    }
    const std::uintptr_t instruction_end = store_address + kStateStoreSize;
    std::uintptr_t state_address = instruction_end;
    if (displacement >= 0) {
        const auto distance = static_cast<std::uintptr_t>(displacement);
        if (state_address >
            (std::numeric_limits<std::uintptr_t>::max)() - distance) {
            log::Warn(
                "engine_patch group=raw_mouse_input disabled=1 "
                "reason=state_address_overflow");
            return false;
        }
        state_address += distance;
    } else {
        const auto distance = static_cast<std::uintptr_t>(
            -static_cast<std::int64_t>(displacement));
        if (state_address < distance) {
            log::Warn(
                "engine_patch group=raw_mouse_input disabled=1 "
                "reason=state_address_underflow");
            return false;
        }
        state_address -= distance;
    }

    MEMORY_BASIC_INFORMATION state_memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(state_address),
                     &state_memory,
                     sizeof(state_memory)) != sizeof(state_memory) ||
        reinterpret_cast<std::uintptr_t>(state_memory.AllocationBase) !=
            module_base ||
        state_memory.State != MEM_COMMIT ||
        !IsReadableProtection(state_memory.Protect) ||
        !IsPatchable(state_address, kRawMouseDisabled, kRawMouseEnabled)) {
        log::Warn(
            "engine_patch group=raw_mouse_input disabled=1 "
            "reason=state_target_mismatch");
        return false;
    }

    const std::size_t checkpoint = g_static_patches.checkpoint();
    const std::uintptr_t option_address =
        *signature + kRawMouseOptionReadOffset;
    const bool applied =
        ApplyTransactionalPatch("raw_mouse_option", option_address,
                                kRawMouseOptionRead, kForceRawMouseOption) &&
        ApplyTransactionalPatch("raw_mouse_state", state_address,
                                kRawMouseDisabled, kRawMouseEnabled);
    if (!applied && !g_static_patches.RestoreTo(checkpoint)) {
        log::Error("engine_patch group=raw_mouse_input rollback_incomplete=1");
    }
    return applied;
}

bool ApplyCameraSmoothingDisable(std::uintptr_t module_base) {
    auto replacement = kMouseCameraSignature;
    StoreFloat(replacement, kHorizontalMouseDrainRateOffset,
               kImmediateMouseDrainRate);
    StoreFloat(replacement, kVerticalMouseDrainRateOffset,
               kImmediateMouseDrainRate);
    const auto address =
        ResolveUniquePattern(module_base, "camera_mouse_smoothing",
                             kMouseCameraSignature, replacement,
                             PatternMemoryClass::NonExecutable);
    if (!address.has_value()) {
        return false;
    }
    const auto result = g_static_patches.Apply(
        "camera_mouse_smoothing", *address, kMouseCameraSignature, replacement);
    LogPatchResult("camera_mouse_smoothing", *address, result);
    return result == runtime_patch::ApplyResult::Applied ||
           result == runtime_patch::ApplyResult::AlreadyApplied;
}

bool ApplyControllerDeadzones(std::uintptr_t module_base,
                              int left_percent,
                              int right_percent) {
    auto replacement = kControllerResponseSignature;
    if (left_percent != input::kStockDeadzone) {
        const float deadzone = input::DeadzoneFraction(left_percent);
        StoreFloat(replacement, kLeftStickDeadzoneOffset, deadzone);
        StoreFloat(replacement, kLeftStickScaleOffset,
                   input::DeadzoneScale(deadzone));
    }
    if (right_percent != input::kStockDeadzone) {
        const float deadzone = input::DeadzoneFraction(right_percent);
        StoreFloat(replacement, kRightStickDeadzoneOffset, deadzone);
        StoreFloat(replacement, kRightStickScaleOffset,
                   input::DeadzoneScale(deadzone));
    }
    const auto address =
        ResolveUniquePattern(module_base, "controller_deadzones",
                             kControllerResponseSignature, replacement,
                             PatternMemoryClass::NonExecutable);
    if (!address.has_value()) {
        return false;
    }
    const auto result =
        g_static_patches.Apply("controller_deadzones", *address,
                               kControllerResponseSignature, replacement);
    LogPatchResult("controller_deadzones", *address, result);
    if (result == runtime_patch::ApplyResult::Applied ||
        result == runtime_patch::ApplyResult::AlreadyApplied) {
        log::InfoF(
            "input controller_deadzones left_percent=%d right_percent=%d",
            left_percent, right_percent);
        return true;
    }
    return false;
}

bool ApplyAudioFileOpenGuard(std::uintptr_t module_base,
                             bool latest_steam_layout) {
    const std::uintptr_t handle_test =
        module_base + (latest_steam_layout ? kLatestAudioFileHandleTestRva
                                           : kLegacyAudioFileHandleTestRva);
    const std::uintptr_t mapping_argument =
        module_base + (latest_steam_layout
                           ? kLatestAudioFileMappingArgumentRva
                           : kLegacyAudioFileMappingArgumentRva);
    if (!IsPatchable(mapping_argument, kMappingArgumentFromRax,
                     kMappingArgumentFromRbx) ||
        !IsPatchable(handle_test, kNullFileHandleTest,
                     kInvalidFileHandleTest)) {
        log::Warn(
            "engine_patch group=audio_file_open disabled=1 "
            "reason=signature_mismatch");
        return false;
    }

    const std::size_t checkpoint = g_static_patches.checkpoint();
    // Changing the mapping argument is behavior-neutral while RAX and RBX are
    // equal, so install it before changing the sentinel test that increments
    // RAX. This keeps the live transition valid in either intermediate state.
    const bool applied =
        ApplyTransactionalPatch("audio_file_mapping_handle", mapping_argument,
                                kMappingArgumentFromRax,
                                kMappingArgumentFromRbx) &&
        ApplyTransactionalPatch("audio_file_invalid_handle", handle_test,
                                kNullFileHandleTest, kInvalidFileHandleTest);
    if (!applied && !g_static_patches.RestoreTo(checkpoint)) {
        log::Error("engine_patch group=audio_file_open rollback_incomplete=1");
    }
    return applied;
}

bool ApplyVramPoolLockBalance(std::uintptr_t module_base,
                              bool latest_steam_layout) {
    const std::uintptr_t call_site =
        module_base + (latest_steam_layout
                           ? kLatestVramPoolFinalValidationCallRva
                           : kLegacyVramPoolFinalValidationCallRva);
    const auto relay = BuildVramUnlockRelay(module_base, latest_steam_layout);
    if (!relay.has_value()) {
        log::Warn(
            "engine_patch group=vram_pool_lock disabled=1 "
            "reason=relay_unavailable");
        return false;
    }
    const auto replacement = MakeRelativeCall<5>(call_site, *relay);
    if (!replacement.has_value()) {
        log::Warn(
            "engine_patch group=vram_pool_lock disabled=1 "
            "reason=relay_out_of_range");
        return false;
    }

    const auto& expected = latest_steam_layout
                               ? kLatestVramPoolFinalValidationCall
                               : kLegacyVramPoolFinalValidationCall;
    if (!IsPatchable(call_site, expected, *replacement)) {
        log::Warn(
            "engine_patch group=vram_pool_lock disabled=1 "
            "reason=callsite_mismatch");
        return false;
    }
    return ApplyTransactionalPatch("vram_pool_lock_balance", call_site,
                                   expected, *replacement);
}

bool ApplyVramCapacityReporting(std::uintptr_t module_base,
                                bool latest_steam_layout) {
    const std::uintptr_t read_site =
        module_base + (latest_steam_layout ? kLatestBenchmarkVramReadRva
                                           : kLegacyBenchmarkVramReadRva);
    const auto relay = BuildVramCapacityRelay(module_base, latest_steam_layout);
    if (!relay.has_value()) {
        log::Warn(
            "engine_patch group=vram_capacity disabled=1 "
            "reason=relay_unavailable");
        return false;
    }
    const auto replacement = MakeRelativeCall<9>(read_site, *relay);
    if (!replacement.has_value()) {
        log::Warn(
            "engine_patch group=vram_capacity disabled=1 "
            "reason=relay_out_of_range");
        return false;
    }

    const auto& expected = latest_steam_layout
                               ? kLatestTruncatedBenchmarkVramRead
                               : kLegacyTruncatedBenchmarkVramRead;
    if (!IsPatchable(read_site, expected, *replacement)) {
        log::Warn(
            "engine_patch group=vram_capacity disabled=1 "
            "reason=callsite_mismatch");
        return false;
    }
    return ApplyTransactionalPatch("vram_capacity_64bit", read_site, expected,
                                   *replacement);
}

bool ApplyCharacterSurfaceBridge(std::uintptr_t module_base,
                                 bool latest_steam_layout,
                                 bool bridge_wetness) {
    const std::uintptr_t call_site =
        module_base + (latest_steam_layout
                           ? kLatestCharacterSurfaceCopyCallRva
                           : kLegacyCharacterSurfaceCopyCallRva);
    const auto relay = BuildCharacterSurfaceRelay(
        module_base, latest_steam_layout, bridge_wetness);
    if (!relay.has_value()) {
        log::Warn(
            "engine_patch group=character_surface disabled=1 "
            "reason=relay_unavailable");
        return false;
    }
    const auto replacement = MakeRelativeCall<5>(call_site, *relay);
    if (!replacement.has_value()) {
        log::Warn(
            "engine_patch group=character_surface disabled=1 "
            "reason=relay_out_of_range");
        return false;
    }

    const auto& expected = latest_steam_layout
                               ? kLatestCharacterSurfaceCopyCall
                               : kLegacyCharacterSurfaceCopyCall;
    if (!IsPatchable(call_site, expected, *replacement)) {
        log::Warn(
            "engine_patch group=character_surface disabled=1 "
            "reason=callsite_mismatch");
        return false;
    }
    return ApplyTransactionalPatch("character_surface_render_bridge", call_site,
                                   expected, *replacement);
}

bool ApplyResourceFailureRecovery(std::uintptr_t module_base,
                                  bool latest_steam_layout) {
    ResourceFailureRelay* const relays =
        BuildResourceFailureRelays(module_base, latest_steam_layout);
    if (relays == nullptr) {
        log::Warn(
            "engine_patch group=resource_failure_recovery disabled=1 "
            "reason=relay_allocation_or_seal_failed");
        return false;
    }

    const std::uintptr_t loaded_chunk_call =
        module_base + (latest_steam_layout ? kLatestLoadedChunkErrorLogCallRva
                                           : kLegacyLoadedChunkErrorLogCallRva);
    const std::uintptr_t loaded_chunk_file_call =
        module_base + (latest_steam_layout
                           ? kLatestLoadedChunkFileErrorLogCallRva
                           : kLegacyLoadedChunkFileErrorLogCallRva);
    const std::uintptr_t file_size_cleanup_call =
        module_base + (latest_steam_layout
                           ? kLatestLoadedChunkFileSizeCleanupCallRva
                           : kLegacyLoadedChunkFileSizeCleanupCallRva);
    const std::uintptr_t synchronous_finalize_call =
        module_base + (latest_steam_layout
                           ? kLatestSynchronousResourceFinalizeCallRva
                           : kLegacySynchronousResourceFinalizeCallRva);
    const std::uintptr_t synchronous_loose_open_failure_branch =
        module_base + (latest_steam_layout
                           ? kLatestSynchronousLooseOpenFailureBranchRva
                           : kLegacySynchronousLooseOpenFailureBranchRva);
    const std::uintptr_t synchronous_loose_finalize_call =
        module_base + (latest_steam_layout
                           ? kLatestSynchronousLooseFinalizeCallRva
                           : kLegacySynchronousLooseFinalizeCallRva);
    const std::uintptr_t synchronous_loose_invalid_size_state =
        module_base + (latest_steam_layout
                           ? kLatestSynchronousLooseInvalidSizeStateRva
                           : kLegacySynchronousLooseInvalidSizeStateRva);
    const std::uintptr_t qcmp_failure_copy_call =
        module_base + (latest_steam_layout ? kLatestQcmpFailureCopyCallRva
                                           : kLegacyQcmpFailureCopyCallRva);
    const std::uintptr_t compressed_xml_allocation_call =
        module_base + (latest_steam_layout
                           ? kLatestCompressedXmlAllocationCallRva
                           : kLegacyCompressedXmlAllocationCallRva);
    const std::uintptr_t compressed_xml_finalize =
        module_base + (latest_steam_layout ? kLatestCompressedXmlFinalizeRva
                                           : kLegacyCompressedXmlFinalizeRva);
    const auto loaded_chunk_replacement =
        MakeRelativeCall<5>(loaded_chunk_call, relays->loaded_chunk_error);
    const auto loaded_chunk_file_replacement = MakeRelativeCall<5>(
        loaded_chunk_file_call, relays->loaded_chunk_file_error);
    const auto file_size_cleanup_replacement = MakeRelativeCall<5>(
        file_size_cleanup_call, relays->loaded_chunk_file_size_error);
    const auto synchronous_finalize_replacement = MakeRelativeCall<5>(
        synchronous_finalize_call, relays->synchronous_indexed_read_error);
    const auto synchronous_loose_open_replacement =
        MakeRelativeZeroJump(synchronous_loose_open_failure_branch,
                             relays->synchronous_loose_open_error);
    const auto synchronous_loose_finalize_replacement = MakeRelativeCall<5>(
        synchronous_loose_finalize_call, relays->synchronous_loose_read_error);
    const auto synchronous_loose_size_replacement =
        MakeRelativeCall<10>(synchronous_loose_invalid_size_state,
                             relays->synchronous_loose_size_error);
    const auto qcmp_failure_copy_replacement = MakeRelativeCall<5>(
        qcmp_failure_copy_call, relays->qcmp_failure_copy_guard);
    const auto compressed_xml_allocation_replacement = MakeRelativeCall<5>(
        compressed_xml_allocation_call,
        relays->compressed_xml_allocation_guard);
    const auto compressed_xml_finalize_replacement = MakeRelativeJump<6>(
        compressed_xml_finalize, relays->compressed_xml_finalize_guard);
    if (!loaded_chunk_replacement.has_value() ||
        !loaded_chunk_file_replacement.has_value() ||
        !file_size_cleanup_replacement.has_value() ||
        !synchronous_finalize_replacement.has_value() ||
        !synchronous_loose_open_replacement.has_value() ||
        !synchronous_loose_finalize_replacement.has_value() ||
        !synchronous_loose_size_replacement.has_value() ||
        !qcmp_failure_copy_replacement.has_value() ||
        !compressed_xml_allocation_replacement.has_value() ||
        !compressed_xml_finalize_replacement.has_value()) {
        log::Warn(
            "engine_patch group=resource_failure_recovery disabled=1 "
            "reason=relay_out_of_range");
        return false;
    }

    const auto& loaded_chunk_expected = latest_steam_layout
                                            ? kLatestLoadedChunkErrorLogCall
                                            : kLegacyLoadedChunkErrorLogCall;
    const auto& loaded_chunk_file_expected =
        latest_steam_layout ? kLatestLoadedChunkFileErrorLogCall
                            : kLegacyLoadedChunkFileErrorLogCall;
    const auto& file_size_cleanup_expected =
        latest_steam_layout ? kLatestLoadedChunkFileSizeCleanupCall
                            : kLegacyLoadedChunkFileSizeCleanupCall;
    const auto& synchronous_finalize_expected =
        latest_steam_layout ? kLatestSynchronousResourceFinalizeCall
                            : kLegacySynchronousResourceFinalizeCall;
    const auto& synchronous_loose_finalize_expected =
        latest_steam_layout ? kLatestSynchronousLooseFinalizeCall
                            : kLegacySynchronousLooseFinalizeCall;
    const auto& qcmp_failure_copy_expected =
        latest_steam_layout ? kLatestQcmpFailureCopyCall
                            : kLegacyQcmpFailureCopyCall;
    const auto& compressed_xml_allocation_expected =
        latest_steam_layout ? kLatestCompressedXmlAllocationCall
                            : kLegacyCompressedXmlAllocationCall;
    if (!IsPatchable(loaded_chunk_call, loaded_chunk_expected,
                     *loaded_chunk_replacement) ||
        !IsPatchable(loaded_chunk_file_call, loaded_chunk_file_expected,
                     *loaded_chunk_file_replacement) ||
        !IsPatchable(file_size_cleanup_call, file_size_cleanup_expected,
                     *file_size_cleanup_replacement) ||
        !IsPatchable(synchronous_finalize_call, synchronous_finalize_expected,
                     *synchronous_finalize_replacement) ||
        !IsPatchable(synchronous_loose_open_failure_branch,
                     kSynchronousLooseOpenFailureBranch,
                     *synchronous_loose_open_replacement) ||
        !IsPatchable(synchronous_loose_finalize_call,
                     synchronous_loose_finalize_expected,
                     *synchronous_loose_finalize_replacement) ||
        !IsPatchable(synchronous_loose_invalid_size_state,
                     kSynchronousLooseInvalidSizeState,
                     *synchronous_loose_size_replacement) ||
        !IsPatchable(qcmp_failure_copy_call, qcmp_failure_copy_expected,
                     *qcmp_failure_copy_replacement) ||
        !IsPatchable(compressed_xml_allocation_call,
                     compressed_xml_allocation_expected,
                     *compressed_xml_allocation_replacement) ||
        !IsPatchable(compressed_xml_finalize, kCompressedXmlFinalize,
                     *compressed_xml_finalize_replacement)) {
        log::Warn(
            "engine_patch group=resource_failure_recovery disabled=1 "
            "reason=callsite_mismatch");
        return false;
    }

    const std::size_t checkpoint = g_static_patches.checkpoint();
    const bool applied =
        ApplyTransactionalPatch("loaded_chunk_error_recovery",
                                loaded_chunk_call, loaded_chunk_expected,
                                *loaded_chunk_replacement) &&
        ApplyTransactionalPatch(
            "loaded_chunk_file_error_recovery", loaded_chunk_file_call,
            loaded_chunk_file_expected, *loaded_chunk_file_replacement) &&
        ApplyTransactionalPatch(
            "loaded_chunk_file_size_error_recovery", file_size_cleanup_call,
            file_size_cleanup_expected, *file_size_cleanup_replacement) &&
        ApplyTransactionalPatch("synchronous_resource_read_error_recovery",
                                synchronous_finalize_call,
                                synchronous_finalize_expected,
                                *synchronous_finalize_replacement) &&
        ApplyTransactionalPatch("synchronous_loose_open_error_recovery",
                                synchronous_loose_open_failure_branch,
                                kSynchronousLooseOpenFailureBranch,
                                *synchronous_loose_open_replacement) &&
        ApplyTransactionalPatch("synchronous_loose_read_error_recovery",
                                synchronous_loose_finalize_call,
                                synchronous_loose_finalize_expected,
                                *synchronous_loose_finalize_replacement) &&
        ApplyTransactionalPatch("synchronous_loose_size_error_recovery",
                                synchronous_loose_invalid_size_state,
                                kSynchronousLooseInvalidSizeState,
                                *synchronous_loose_size_replacement) &&
        ApplyTransactionalPatch("qcmp_failure_copy_guard",
                                qcmp_failure_copy_call,
                                qcmp_failure_copy_expected,
                                *qcmp_failure_copy_replacement) &&
        ApplyTransactionalPatch("compressed_xml_allocation_overflow_guard",
                                compressed_xml_allocation_call,
                                compressed_xml_allocation_expected,
                                *compressed_xml_allocation_replacement) &&
        ApplyTransactionalPatch("compressed_xml_decode_result_guard",
                                compressed_xml_finalize,
                                kCompressedXmlFinalize,
                                *compressed_xml_finalize_replacement);
    if (!applied && !g_static_patches.RestoreTo(checkpoint)) {
        log::Warn(
            "engine_patch group=resource_failure_recovery "
            "rollback_incomplete=1");
    }
    return applied;
}

bool ApplyForceAnisotropicFiltering(std::uintptr_t module_base,
                                    bool latest_steam_layout,
                                    bool sampler_builder_prevalidated) {
    const texture_filtering::AddressProfile addresses =
        texture_filtering::SelectAddresses(latest_steam_layout);
    const std::uintptr_t builder_address =
        module_base + addresses.sampler_builder_rva;
    const std::uintptr_t prefix_address =
        builder_address + texture_filtering::kForceBranchPrefixOffset;
    const std::uintptr_t instruction_address =
        module_base + addresses.force_trilinear_instruction_rva;
    const std::uintptr_t suffix_address =
        builder_address + texture_filtering::kForceBranchSuffixOffset;

    // When the sampler-builder detour was installed first, MinHook owns the
    // entry bytes. Hooks.cpp only reports that state after validating the
    // complete stock prologue and successfully creating the detour. The exact
    // branch bytes below remain untouched and still identify the patch site.
    const bool builder_identity_verified =
        sampler_builder_prevalidated ||
        runtime_patch::MatchesBytes(
            builder_address, texture_filtering::kSamplerBuilderPrologue);
    if (!builder_identity_verified ||
        !runtime_patch::MatchesBytes(
            prefix_address, texture_filtering::kForceBranchPrefix) ||
        !runtime_patch::MatchesBytes(
            suffix_address, texture_filtering::kForceBranchSuffix)) {
        log::WarnF(
            "engine_patch name=force_anisotropic_filtering "
            "result=signature_mismatch builder=0x%p target=0x%p disabled=1",
            reinterpret_cast<void*>(builder_address),
            reinterpret_cast<void*>(instruction_address));
        return false;
    }

    const auto result = g_static_patches.Apply(
        "force_anisotropic_filtering",
        instruction_address,
        texture_filtering::kStockTrilinearInstruction,
        texture_filtering::kForcedAnisotropicInstruction);
    LogPatchResult("force_anisotropic_filtering", instruction_address, result);
    return result == runtime_patch::ApplyResult::Applied ||
           result == runtime_patch::ApplyResult::AlreadyApplied;
}

std::uint32_t RequestedFeatureMask(const Config& config) noexcept {
    std::uint32_t mask = 0;
    if (config.fix_first_run_resolution) {
        mask |= kFeatureFirstRunResolution;
    }
    if (config.fix_scaleform_qpc_clock) {
        mask |= kFeatureScaleformQpcClock;
    }
    if (config.fix_file_timestamp_open_mode) {
        mask |= kFeatureFileTimestampOpenMode;
    }
    if (config.fix_audio_file_open) {
        mask |= kFeatureAudioFileOpen;
    }
    if (config.fix_large_file_sizes) {
        mask |= kFeatureFileSize;
    }
    if (config.fix_vram_pool_lock) {
        mask |= kFeatureVramPoolLock;
    }
    if (config.fix_vram_capacity_reporting) {
        mask |= kFeatureVramCapacity;
    }
    if (config.fix_resource_loading) {
        mask |= kFeatureResourceFailureRecovery;
    }
    if (config.fix_contact_list_overflow) {
        mask |= kFeatureContactListOverflow;
    }
    if (config.improve_spherical_reflections) {
        mask |= kFeatureSphericalReflection;
    }
    if (config.force_anisotropic_filtering) {
        mask |= kFeatureForceAnisotropicFiltering;
    }
    if (config.remove_hidden_120_fps_cap) {
        mask |= kFeatureHiddenCap;
    }
    if (config.fix_corrupt_save_handling) {
        mask |= kFeatureSaveGuard;
    }
    if (config.fix_thread_creation_failure) {
        mask |= kFeatureThreadGuard;
    }
    if (config.restore_character_wetness) {
        mask |= kFeatureCharacterSurface;
    }
    if (config.force_raw_mouse_input) {
        mask |= kFeatureRawMouseInput;
    }
    if (config.disable_camera_smoothing) {
        mask |= kFeatureCameraSmoothing;
    }
    if (config.controller_left_stick_deadzone != input::kStockDeadzone ||
        config.controller_right_stick_deadzone != input::kStockDeadzone) {
        mask |= kFeatureControllerDeadzone;
    }
    return mask;
}

bool AllRequestedFeaturesResolved(std::uint32_t mask) noexcept {
    return ((mask & kFeatureSphericalReflection) == 0 ||
            g_spherical_reflection_resolved) &&
           ((mask & kFeatureForceAnisotropicFiltering) == 0 ||
            g_force_anisotropic_filtering_resolved) &&
           ((mask & kFeatureHiddenCap) == 0 || g_hidden_cap_resolved) &&
           ((mask & kFeatureSaveGuard) == 0 || g_save_guard_resolved) &&
           ((mask & kFeatureThreadGuard) == 0 || g_thread_guard_resolved) &&
           ((mask & kFeatureFirstRunResolution) == 0 ||
            g_first_run_resolution_resolved) &&
           ((mask & kFeatureScaleformQpcClock) == 0 ||
            g_scaleform_qpc_clock_resolved) &&
           ((mask & kFeatureFileTimestampOpenMode) == 0 ||
            g_file_timestamp_open_mode_resolved) &&
           ((mask & kFeatureAudioFileOpen) == 0 ||
            g_audio_file_open_resolved) &&
           ((mask & kFeatureFileSize) == 0 || g_file_size_resolved) &&
           ((mask & kFeatureVramPoolLock) == 0 || g_vram_pool_lock_resolved) &&
           ((mask & kFeatureVramCapacity) == 0 || g_vram_capacity_resolved) &&
           ((mask & kFeatureCharacterSurface) == 0 ||
            g_character_surface_resolved) &&
           ((mask & kFeatureRawMouseInput) == 0 ||
            g_raw_mouse_input_resolved) &&
           ((mask & kFeatureCameraSmoothing) == 0 ||
            g_camera_smoothing_resolved) &&
           ((mask & kFeatureControllerDeadzone) == 0 ||
            g_controller_deadzone_resolved) &&
           ((mask & kFeatureResourceFailureRecovery) == 0 ||
            g_resource_failure_recovery_resolved) &&
           ((mask & kFeatureContactListOverflow) == 0 ||
            g_contact_list_overflow_resolved);
}

void ResetFeatureResolutionState() noexcept {
    g_initialization_complete.store(false, std::memory_order_release);
    g_requested_feature_mask = 0;
    g_requested_feature_mask_valid = false;
    g_active_request.reset();
    g_spherical_reflection_resolved = false;
    g_force_anisotropic_filtering_resolved = false;
    g_hidden_cap_resolved = false;
    g_save_guard_resolved = false;
    g_thread_guard_resolved = false;
    g_first_run_resolution_resolved = false;
    g_scaleform_qpc_clock_resolved = false;
    g_file_timestamp_open_mode_resolved = false;
    g_audio_file_open_resolved = false;
    g_file_size_resolved = false;
    g_vram_pool_lock_resolved = false;
    g_vram_capacity_resolved = false;
    g_character_surface_resolved = false;
    g_raw_mouse_input_resolved = false;
    g_camera_smoothing_resolved = false;
    g_controller_deadzone_resolved = false;
    g_resource_failure_recovery_resolved = false;
    g_contact_list_overflow_resolved = false;
}

bool ApplySaveGuards(std::uintptr_t module_base, bool latest_steam_layout) {
    const std::uintptr_t header_load =
        module_base + (latest_steam_layout ? kLatestSaveHeaderLoadRva
                                           : kLegacySaveHeaderLoadRva);
    const std::uintptr_t parser_function =
        module_base + (latest_steam_layout ? kLatestSaveParserFunctionRva
                                           : kLegacySaveParserFunctionRva);
    if (!runtime_patch::MatchesBytes(header_load, kSaveHeaderLoadSignature) ||
        !runtime_patch::MatchesBytes(parser_function, kSaveParserSignature)) {
        log::Warn(
            "engine_patch group=corrupt_save_guard disabled=1 "
            "reason=signature_mismatch");
        return false;
    }
    g_deserialize_save_original.store(
        reinterpret_cast<DeserializeSaveFn>(parser_function),
        std::memory_order_release);

    const std::uintptr_t header_site =
        module_base + (latest_steam_layout ? kLatestSaveHeaderCallRva
                                           : kLegacySaveHeaderCallRva);
    const std::uintptr_t parser_site1 =
        module_base + (latest_steam_layout ? kLatestSaveParserCall1Rva
                                           : kLegacySaveParserCall1Rva);
    const std::uintptr_t parser_site2 =
        module_base + (latest_steam_layout ? kLatestSaveParserCall2Rva
                                           : kLegacySaveParserCall2Rva);
    const auto header_replacement =
        MakeRelativeCall<5>(header_site, g_relay_layout.save_header);
    const auto parser_replacement1 =
        MakeRelativeCall<5>(parser_site1, g_relay_layout.save_parser);
    const auto parser_replacement2 =
        MakeRelativeCall<5>(parser_site2, g_relay_layout.save_parser);
    if (!header_replacement || !parser_replacement1 || !parser_replacement2) {
        log::Warn(
            "engine_patch group=corrupt_save_guard disabled=1 "
            "reason=relay_out_of_range");
        return false;
    }

    const auto& header_expected =
        latest_steam_layout ? kLatestSaveHeaderCall : kLegacySaveHeaderCall;
    const auto& parser_expected1 =
        latest_steam_layout ? kLatestSaveParserCall1 : kLegacySaveParserCall1;
    const auto& parser_expected2 =
        latest_steam_layout ? kLatestSaveParserCall2 : kLegacySaveParserCall2;
    if (!IsPatchable(header_site, header_expected, *header_replacement) ||
        !IsPatchable(parser_site1, parser_expected1, *parser_replacement1) ||
        !IsPatchable(parser_site2, parser_expected2, *parser_replacement2)) {
        log::Warn(
            "engine_patch group=corrupt_save_guard disabled=1 "
            "reason=callsite_mismatch");
        return false;
    }
    const std::size_t checkpoint = g_static_patches.checkpoint();
    const bool applied =
        ApplyTransactionalPatch("corrupt_save_header", header_site,
                                header_expected, *header_replacement) &&
        ApplyTransactionalPatch("corrupt_save_parser_primary", parser_site1,
                                parser_expected1, *parser_replacement1) &&
        ApplyTransactionalPatch("corrupt_save_parser_secondary", parser_site2,
                                parser_expected2, *parser_replacement2);
    if (!applied && !g_static_patches.RestoreTo(checkpoint)) {
        log::Error(
            "engine_patch group=corrupt_save_guard rollback_incomplete=1");
    }
    return applied;
}

bool ApplyThreadFailureGuards(std::uintptr_t module_base,
                              bool latest_steam_layout) {
    const std::uintptr_t task_site =
        module_base + (latest_steam_layout ? kLatestTaskCreateThreadCallRva
                                           : kLegacyTaskCreateThreadCallRva);
    const std::uintptr_t generic_site =
        module_base + (latest_steam_layout ? kLatestGenericCreateThreadCallRva
                                           : kLegacyGenericCreateThreadCallRva);
    const std::uintptr_t io_site =
        module_base + (latest_steam_layout ? kLatestIoCreateThreadCallRva
                                           : kLegacyIoCreateThreadCallRva);
    const std::uintptr_t bank_manager_site =
        module_base + (latest_steam_layout
                           ? kLatestBankManagerCreateThreadCallRva
                           : kLegacyBankManagerCreateThreadCallRva);
    const std::uintptr_t bank_shutdown_fence_site =
        module_base + kBankShutdownFenceCreateEventCallRva;
    const std::uintptr_t wwise_blocking_completion_site =
        module_base + (latest_steam_layout ? kLatestWwiseBlockingCompletionRva
                                           : kLegacyWwiseBlockingCompletionRva);
    const std::uintptr_t wwise_blocking_wait_site =
        module_base + (latest_steam_layout ? kLatestWwiseBlockingWaitRva
                                           : kLegacyWwiseBlockingWaitRva);
    const auto task_replacement =
        MakeRelativeCall<6>(task_site, g_relay_layout.create_task_thread);
    const auto generic_replacement =
        MakeRelativeCall<6>(generic_site, g_relay_layout.create_thread);
    const auto io_replacement =
        MakeRelativeCall<6>(io_site, g_relay_layout.create_io_thread);
    const auto bank_manager_replacement = MakeRelativeCall<6>(
        bank_manager_site, g_relay_layout.create_bank_manager_thread);
    const auto bank_shutdown_fence_replacement =
        MakeRelativeCall<6>(bank_shutdown_fence_site,
                            g_relay_layout.create_bank_shutdown_fence_event);
    const auto wwise_blocking_completion_replacement =
        MakeRelativeJump<15>(wwise_blocking_completion_site,
                             g_relay_layout.complete_wwise_blocking_operation);
    const auto wwise_blocking_wait_replacement = MakeRelativeJump<14>(
        wwise_blocking_wait_site, g_relay_layout.wait_wwise_blocking_operation);
    if (!task_replacement || !generic_replacement || !io_replacement ||
        !bank_manager_replacement || !bank_shutdown_fence_replacement ||
        !wwise_blocking_completion_replacement ||
        !wwise_blocking_wait_replacement) {
        log::Warn(
            "engine_patch group=thread_failure_guard disabled=1 "
            "reason=relay_out_of_range");
        return false;
    }

    const auto& task_expected = latest_steam_layout
                                    ? kLatestTaskCreateThreadCall
                                    : kLegacyTaskCreateThreadCall;
    const auto& generic_expected = latest_steam_layout
                                       ? kLatestGenericCreateThreadCall
                                       : kLegacyGenericCreateThreadCall;
    const auto& io_expected = latest_steam_layout ? kLatestIoCreateThreadCall
                                                  : kLegacyIoCreateThreadCall;
    const auto& bank_manager_expected =
        latest_steam_layout ? kLatestBankManagerCreateThreadCall
                            : kLegacyBankManagerCreateThreadCall;
    const auto& wwise_blocking_completion_expected =
        latest_steam_layout ? kLatestWwiseBlockingCompletion
                            : kLegacyWwiseBlockingCompletion;
    const auto& wwise_blocking_wait_expected = latest_steam_layout
                                                   ? kLatestWwiseBlockingWait
                                                   : kLegacyWwiseBlockingWait;
    if (!IsPatchable(task_site, task_expected, *task_replacement) ||
        !IsPatchable(generic_site, generic_expected, *generic_replacement) ||
        !IsPatchable(io_site, io_expected, *io_replacement) ||
        !IsPatchable(bank_manager_site, bank_manager_expected,
                     *bank_manager_replacement) ||
        !IsPatchable(bank_shutdown_fence_site,
                     kBankShutdownFenceCreateEventCall,
                     *bank_shutdown_fence_replacement) ||
        !IsPatchable(wwise_blocking_completion_site,
                     wwise_blocking_completion_expected,
                     *wwise_blocking_completion_replacement) ||
        !IsPatchable(wwise_blocking_wait_site, wwise_blocking_wait_expected,
                     *wwise_blocking_wait_replacement)) {
        log::Warn(
            "engine_patch group=thread_failure_guard disabled=1 "
            "reason=callsite_mismatch");
        return false;
    }
    if (!PrepareBankShutdownFenceEventReserve()) {
        log::Warn(
            "engine_patch group=thread_failure_guard disabled=1 "
            "reason=bank_shutdown_fence_reserve_failed");
        return false;
    }
    const std::size_t checkpoint = g_static_patches.checkpoint();
    const bool applied =
        ApplyTransactionalPatch("task_thread_event_guard", task_site,
                                task_expected, *task_replacement) &&
        ApplyTransactionalPatch("generic_thread_failure_sentinel", generic_site,
                                generic_expected, *generic_replacement) &&
        ApplyTransactionalPatch("io_thread_event_guard", io_site, io_expected,
                                *io_replacement) &&
        ApplyTransactionalPatch("bank_manager_thread_event_guard",
                                bank_manager_site, bank_manager_expected,
                                *bank_manager_replacement) &&
        ApplyTransactionalPatch("bank_shutdown_fence_event_reserve",
                                bank_shutdown_fence_site,
                                kBankShutdownFenceCreateEventCall,
                                *bank_shutdown_fence_replacement) &&
        ApplyTransactionalPatch("wwise_blocking_completion_protocol",
                                wwise_blocking_completion_site,
                                wwise_blocking_completion_expected,
                                *wwise_blocking_completion_replacement) &&
        ApplyTransactionalPatch(
            "wwise_blocking_wait_protocol", wwise_blocking_wait_site,
            wwise_blocking_wait_expected, *wwise_blocking_wait_replacement);
    if (!applied && !g_static_patches.RestoreTo(checkpoint)) {
        log::Error(
            "engine_patch group=thread_failure_guard rollback_incomplete=1");
    }
    return applied;
}

}  // namespace

bool IsSafeSavePayload(std::uintptr_t payload,
                       std::uint32_t payload_size) noexcept {
    return payload != 0 && payload_size >= kMinimumSavePayloadSize &&
           payload_size <=
               static_cast<std::uint32_t>((std::numeric_limits<int>::max)()) &&
           payload <=
               (std::numeric_limits<std::uintptr_t>::max)() - payload_size;
}

bool IsSafeSaveFile(std::uintptr_t file_data,
                    std::uint32_t file_size) noexcept {
    return file_data != 0 && file_size >= kMinimumSaveFileSize &&
           file_size <=
               static_cast<std::uint32_t>((std::numeric_limits<int>::max)()) &&
           file_data <=
               (std::numeric_limits<std::uintptr_t>::max)() - file_size;
}

std::uintptr_t NormalizeThreadHandle(std::uintptr_t handle) noexcept {
    return handle == 0 ? (std::numeric_limits<std::uintptr_t>::max)() : handle;
}

bool AreTaskManagerEventsReady(std::uintptr_t sync_event,
                               std::uintptr_t close_event,
                               std::uintptr_t add_event,
                               std::uintptr_t all_done_event) noexcept {
    return sync_event != 0 && close_event != 0 && add_event != 0 &&
           all_done_event != 0;
}

bool AreIoThreadBootstrapEventsReady(std::uintptr_t wake_event,
                                     std::uintptr_t shutdown_event,
                                     std::uintptr_t work_event,
                                     std::uintptr_t idle_event) noexcept {
    return wake_event != 0 && shutdown_event != 0 && work_event != 0 &&
           idle_event != 0;
}

bool AreBankManagerEventsReady(std::uintptr_t wake_event,
                               std::uintptr_t callback_fence_event) noexcept {
    return wake_event != 0 && callback_fence_event != 0;
}

std::uintptr_t SelectCreatedOrReservedEvent(
    std::uintptr_t created_event,
    std::uintptr_t reserved_event) noexcept {
    return created_event != 0 ? created_event : reserved_event;
}

bool IsWwiseBlockingOperationPending(std::uint8_t pending_flag) noexcept {
    return pending_flag != 0;
}

std::optional<std::int32_t> ComputeRelativeBranchDisplacement(
    std::uintptr_t instruction_address,
    std::size_t instruction_size,
    std::uintptr_t target_address) noexcept {
    if (instruction_size >
        (std::numeric_limits<std::uintptr_t>::max)() - instruction_address) {
        return std::nullopt;
    }
    const std::uintptr_t next = instruction_address + instruction_size;
    if (target_address >= next) {
        const std::uintptr_t distance = target_address - next;
        if (distance > static_cast<std::uintptr_t>(
                           (std::numeric_limits<std::int32_t>::max)())) {
            return std::nullopt;
        }
        return static_cast<std::int32_t>(distance);
    }
    const std::uintptr_t distance = next - target_address;
    constexpr std::uintptr_t kMaximumNegativeDistance =
        static_cast<std::uintptr_t>(
            (std::numeric_limits<std::int32_t>::max)()) +
        1;
    if (distance > kMaximumNegativeDistance) {
        return std::nullopt;
    }
    if (distance == kMaximumNegativeDistance) {
        return (std::numeric_limits<std::int32_t>::min)();
    }
    return -static_cast<std::int32_t>(distance);
}

ReflectionResolution ResolveReflectionResolution(int configured_width,
                                                 int detected_display_width) {
    int width = configured_width == kSphericalReflectionWidthAuto
                    ? detected_display_width
                    : configured_width;
    width = (std::clamp)(width, kSphericalReflectionWidthMin,
                         kSphericalReflectionWidthMax);
    if ((width & 1) != 0) {
        --width;
    }
    return ReflectionResolution{width, width / 2};
}

int DetectGameDisplayWidth(const std::filesystem::path& display_settings_path) {
    if (const auto configured_width =
            ReadDisplayResolutionWidth(display_settings_path);
        configured_width.has_value()) {
        return *configured_width;
    }

    WindowSearchState state{GetCurrentProcessId()};
    EnumWindows(&InspectProcessWindow, reinterpret_cast<LPARAM>(&state));
    if (state.best_width > 0) {
        return state.best_width;
    }

    DEVMODEW display_mode{};
    display_mode.dmSize = sizeof(display_mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &display_mode) &&
        display_mode.dmPelsWidth > 0) {
        return static_cast<int>(display_mode.dmPelsWidth);
    }
    return kSphericalReflectionWidthMin;
}

bool InitializeStaticPatches(
    const Config& config,
    std::uintptr_t module_base,
    bool latest_steam_layout,
    bool sampler_builder_prevalidated,
    const std::filesystem::path& display_settings_path) {
    std::lock_guard<std::mutex> lifecycle_lock(g_lifecycle_mutex);
    if (module_base == 0) {
        log::Warn(
            "engine_patch group=static disabled=1 reason=missing_module_base");
        return false;
    }
    const std::uint32_t requested_mask = RequestedFeatureMask(config);
    int detected_width = 0;
    ReflectionResolution reflection_resolution{};
    if (config.improve_spherical_reflections) {
        detected_width = DetectGameDisplayWidth(display_settings_path);
        reflection_resolution = ResolveReflectionResolution(
            config.spherical_reflection_width, detected_width);
    }
    const StaticPatchRequest request{
        requested_mask,
        module_base,
        latest_steam_layout,
        config.improve_spherical_reflections ? reflection_resolution.width : 0,
        config.controller_left_stick_deadzone,
        config.controller_right_stick_deadzone,
    };
    if (g_initialization_complete.load(std::memory_order_acquire) &&
        g_active_request.has_value() && *g_active_request == request) {
        return true;
    }

    if (g_active_request.has_value() && *g_active_request != request) {
        if (g_initialized.load(std::memory_order_acquire) &&
            !g_static_patches.RestoreAll()) {
            log::Warn(
                "engine_patch group=static reconfigure_restore_incomplete=1");
            return false;
        }
        ReleaseUnusedBankShutdownFenceEventReserve();
        g_initialized.store(false, std::memory_order_release);
        ResetFeatureResolutionState();
    }
    g_active_request = request;
    bool expected = false;
    if (!g_initialization_in_progress.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }

    // Commit g_initialized only after the transaction has run.  The guard also
    // handles an unexpected C++ exception (for example, an allocation failure)
    // after a patch was recorded: the caller can then invoke Shutdown and
    // restore that owned patch instead of silently losing its rollback record.
    struct InitializationScope final {
        ~InitializationScope() noexcept {
            g_initialized.store(!g_static_patches.empty(),
                                std::memory_order_release);
            g_initialization_complete.store(
                AllRequestedFeaturesResolved(g_requested_feature_mask),
                std::memory_order_release);
            g_initialization_in_progress.store(false,
                                               std::memory_order_release);
        }
    } initialization_scope;

    if (!g_requested_feature_mask_valid ||
        g_requested_feature_mask != requested_mask) {
        const std::uint32_t previous_mask =
            g_requested_feature_mask_valid ? g_requested_feature_mask : 0;
        const bool previous_spherical_resolved =
            g_spherical_reflection_resolved;
        const bool previous_force_anisotropic_filtering_resolved =
            g_force_anisotropic_filtering_resolved;
        const bool previous_hidden_cap_resolved = g_hidden_cap_resolved;
        const bool previous_save_guard_resolved = g_save_guard_resolved;
        const bool previous_thread_guard_resolved = g_thread_guard_resolved;
        const bool previous_first_run_resolution_resolved =
            g_first_run_resolution_resolved;
        const bool previous_scaleform_qpc_clock_resolved =
            g_scaleform_qpc_clock_resolved;
        const bool previous_file_timestamp_open_mode_resolved =
            g_file_timestamp_open_mode_resolved;
        const bool previous_audio_file_open_resolved =
            g_audio_file_open_resolved;
        const bool previous_file_size_resolved = g_file_size_resolved;
        const bool previous_vram_pool_lock_resolved = g_vram_pool_lock_resolved;
        const bool previous_vram_capacity_resolved = g_vram_capacity_resolved;
        const bool previous_character_surface_resolved =
            g_character_surface_resolved;
        const bool previous_raw_mouse_input_resolved =
            g_raw_mouse_input_resolved;
        const bool previous_camera_smoothing_resolved =
            g_camera_smoothing_resolved;
        const bool previous_controller_deadzone_resolved =
            g_controller_deadzone_resolved;
        const bool previous_resource_failure_recovery_resolved =
            g_resource_failure_recovery_resolved;
        const bool previous_contact_list_overflow_resolved =
            g_contact_list_overflow_resolved;
        g_spherical_reflection_resolved =
            (requested_mask & kFeatureSphericalReflection) == 0 ||
            ((previous_mask & kFeatureSphericalReflection) != 0 &&
             previous_spherical_resolved);
        g_force_anisotropic_filtering_resolved =
            (requested_mask & kFeatureForceAnisotropicFiltering) == 0 ||
            ((previous_mask & kFeatureForceAnisotropicFiltering) != 0 &&
             previous_force_anisotropic_filtering_resolved);
        g_hidden_cap_resolved = (requested_mask & kFeatureHiddenCap) == 0 ||
                                ((previous_mask & kFeatureHiddenCap) != 0 &&
                                 previous_hidden_cap_resolved);
        g_save_guard_resolved = (requested_mask & kFeatureSaveGuard) == 0 ||
                                ((previous_mask & kFeatureSaveGuard) != 0 &&
                                 previous_save_guard_resolved);
        g_thread_guard_resolved = (requested_mask & kFeatureThreadGuard) == 0 ||
                                  ((previous_mask & kFeatureThreadGuard) != 0 &&
                                   previous_thread_guard_resolved);
        g_first_run_resolution_resolved =
            (requested_mask & kFeatureFirstRunResolution) == 0 ||
            ((previous_mask & kFeatureFirstRunResolution) != 0 &&
             previous_first_run_resolution_resolved);
        g_scaleform_qpc_clock_resolved =
            (requested_mask & kFeatureScaleformQpcClock) == 0 ||
            ((previous_mask & kFeatureScaleformQpcClock) != 0 &&
             previous_scaleform_qpc_clock_resolved);
        g_file_timestamp_open_mode_resolved =
            (requested_mask & kFeatureFileTimestampOpenMode) == 0 ||
            ((previous_mask & kFeatureFileTimestampOpenMode) != 0 &&
             previous_file_timestamp_open_mode_resolved);
        g_audio_file_open_resolved =
            (requested_mask & kFeatureAudioFileOpen) == 0 ||
            ((previous_mask & kFeatureAudioFileOpen) != 0 &&
             previous_audio_file_open_resolved);
        g_file_size_resolved = (requested_mask & kFeatureFileSize) == 0 ||
                               ((previous_mask & kFeatureFileSize) != 0 &&
                                previous_file_size_resolved);
        g_vram_pool_lock_resolved =
            (requested_mask & kFeatureVramPoolLock) == 0 ||
            ((previous_mask & kFeatureVramPoolLock) != 0 &&
             previous_vram_pool_lock_resolved);
        g_vram_capacity_resolved =
            (requested_mask & kFeatureVramCapacity) == 0 ||
            ((previous_mask & kFeatureVramCapacity) != 0 &&
             previous_vram_capacity_resolved);
        g_character_surface_resolved =
            (requested_mask & kFeatureCharacterSurface) == 0 ||
            ((previous_mask & kFeatureCharacterSurface) != 0 &&
             previous_character_surface_resolved);
        g_raw_mouse_input_resolved =
            (requested_mask & kFeatureRawMouseInput) == 0 ||
            ((previous_mask & kFeatureRawMouseInput) != 0 &&
             previous_raw_mouse_input_resolved);
        g_camera_smoothing_resolved =
            (requested_mask & kFeatureCameraSmoothing) == 0 ||
            ((previous_mask & kFeatureCameraSmoothing) != 0 &&
             previous_camera_smoothing_resolved);
        g_controller_deadzone_resolved =
            (requested_mask & kFeatureControllerDeadzone) == 0 ||
            ((previous_mask & kFeatureControllerDeadzone) != 0 &&
             previous_controller_deadzone_resolved);
        g_resource_failure_recovery_resolved =
            (requested_mask & kFeatureResourceFailureRecovery) == 0 ||
            ((previous_mask & kFeatureResourceFailureRecovery) != 0 &&
             previous_resource_failure_recovery_resolved);
        g_contact_list_overflow_resolved =
            (requested_mask & kFeatureContactListOverflow) == 0 ||
            ((previous_mask & kFeatureContactListOverflow) != 0 &&
             previous_contact_list_overflow_resolved);
        g_requested_feature_mask = requested_mask;
        g_requested_feature_mask_valid = true;
        g_initialization_complete.store(false, std::memory_order_release);
    }

    for (unsigned int attempt = 0; attempt < kFeatureApplyAttempts; ++attempt) {
        if (!g_resource_failure_recovery_resolved) {
            g_resource_failure_recovery_resolved =
                ApplyResourceFailureRecovery(module_base, latest_steam_layout);
            log::InfoF("engine_fix resource_failure_recovery=%d",
                       g_resource_failure_recovery_resolved ? 1 : 0);
        }

        if (!g_contact_list_overflow_resolved) {
            const std::uintptr_t address =
                module_base +
                (latest_steam_layout ? kLatestContactImageFormatCallRva
                                     : kLegacyContactImageFormatCallRva);
            const auto& expected_call =
                latest_steam_layout ? kLatestContactImageFormatCall
                                    : kLegacyContactImageFormatCall;
            const auto result = g_static_patches.Apply(
                "contact_list_overflow", address, expected_call,
                kSkipContactImageFormatCall);
            LogPatchResult("contact_list_overflow", address, result);
            g_contact_list_overflow_resolved =
                result == runtime_patch::ApplyResult::Applied ||
                result == runtime_patch::ApplyResult::AlreadyApplied;
        }

        if (!g_force_anisotropic_filtering_resolved) {
            g_force_anisotropic_filtering_resolved =
                ApplyForceAnisotropicFiltering(module_base,
                                                latest_steam_layout,
                                                sampler_builder_prevalidated);
        }

        if (!g_raw_mouse_input_resolved) {
            g_raw_mouse_input_resolved = ApplyRawMouseInput(module_base);
        }

        if (!g_camera_smoothing_resolved) {
            g_camera_smoothing_resolved =
                ApplyCameraSmoothingDisable(module_base);
        }

        if (!g_controller_deadzone_resolved) {
            g_controller_deadzone_resolved = ApplyControllerDeadzones(
                module_base, config.controller_left_stick_deadzone,
                config.controller_right_stick_deadzone);
        }

        if (!g_first_run_resolution_resolved) {
            const std::uintptr_t address =
                module_base + (latest_steam_layout
                                   ? kLatestFirstRunResolutionRva
                                   : kLegacyFirstRunResolutionRva);
            const auto result = g_static_patches.Apply(
                "first_run_resolution", address, kInvalidFirstRunResolution,
                kValidFirstRunResolution);
            LogPatchResult("first_run_resolution", address, result);
            g_first_run_resolution_resolved =
                result == runtime_patch::ApplyResult::Applied ||
                result == runtime_patch::ApplyResult::AlreadyApplied;
        }

        if (!g_scaleform_qpc_clock_resolved) {
            const std::uintptr_t address =
                module_base + (latest_steam_layout
                                   ? kLatestScaleformQpcClockRva
                                   : kLegacyScaleformQpcClockRva);
            const auto result = g_static_patches.Apply(
                "scaleform_qpc_clock", address,
                kTruncatedScaleformQpcConversion, kFullScaleformQpcConversion);
            LogPatchResult("scaleform_qpc_clock", address, result);
            g_scaleform_qpc_clock_resolved =
                result == runtime_patch::ApplyResult::Applied ||
                result == runtime_patch::ApplyResult::AlreadyApplied;
        }

        if (!g_file_timestamp_open_mode_resolved) {
            const std::uintptr_t address =
                module_base + (latest_steam_layout
                                   ? kLatestFileTimestampOpenModeRva
                                   : kLegacyFileTimestampOpenModeRva);
            const auto result = g_static_patches.Apply(
                "file_timestamp_open_mode", address, kTimestampOpenExisting,
                kTimestampOpenAlways);
            LogPatchResult("file_timestamp_open_mode", address, result);
            g_file_timestamp_open_mode_resolved =
                result == runtime_patch::ApplyResult::Applied ||
                result == runtime_patch::ApplyResult::AlreadyApplied;
        }

        if (!g_audio_file_open_resolved) {
            g_audio_file_open_resolved =
                ApplyAudioFileOpenGuard(module_base, latest_steam_layout);
        }

        if (!g_file_size_resolved) {
            const std::uintptr_t address =
                module_base + (latest_steam_layout ? kLatestFileSizeCombineRva
                                                   : kLegacyFileSizeCombineRva);
            const auto result = g_static_patches.Apply(
                "file_size_64bit", address, kTruncatedFileSizeCombine,
                kFullFileSizeCombine);
            LogPatchResult("file_size_64bit", address, result);
            g_file_size_resolved =
                result == runtime_patch::ApplyResult::Applied ||
                result == runtime_patch::ApplyResult::AlreadyApplied;
        }

        if (!g_vram_pool_lock_resolved) {
            g_vram_pool_lock_resolved =
                ApplyVramPoolLockBalance(module_base, latest_steam_layout);
        }

        if (!g_vram_capacity_resolved) {
            g_vram_capacity_resolved =
                ApplyVramCapacityReporting(module_base, latest_steam_layout);
        }

        if (config.restore_character_wetness && !g_character_surface_resolved) {
            g_character_surface_resolved =
                ApplyCharacterSurfaceBridge(module_base, latest_steam_layout,
                                            config.restore_character_wetness);
        }

        if (config.improve_spherical_reflections &&
            !g_spherical_reflection_resolved) {
            auto replacement = kStockSphericalReflectionSetup;
            StoreLittleEndian32(replacement, 3,
                                static_cast<std::uint32_t>(
                                    reflection_resolution.width));
            StoreLittleEndian32(replacement, 10,
                                static_cast<std::uint32_t>(
                                    reflection_resolution.height));
            const std::uintptr_t address =
                module_base + (latest_steam_layout
                                   ? kLatestSphericalReflectionSetupRva
                                   : kLegacySphericalReflectionSetupRva);
            const auto result = g_static_patches.Apply(
                "spherical_reflection_resolution", address,
                kStockSphericalReflectionSetup, replacement);
            LogPatchResult("spherical_reflection_resolution", address, result);
            g_spherical_reflection_resolved =
                result == runtime_patch::ApplyResult::Applied ||
                result == runtime_patch::ApplyResult::AlreadyApplied;
            log::InfoF(
                "spherical_reflection configured_width=%d detected_width=%d "
                "requested=%dx%d",
                config.spherical_reflection_width, detected_width,
                reflection_resolution.width, reflection_resolution.height);
        }

        if (config.remove_hidden_120_fps_cap && !g_hidden_cap_resolved) {
            const std::uintptr_t function_address =
                module_base + (latest_steam_layout ? kLatestPresentFunctionRva
                                                   : kLegacyPresentFunctionRva);
            const std::uintptr_t branch_address =
                module_base + (latest_steam_layout
                                   ? kLatestHidden120FpsWaitBranchRva
                                   : kLegacyHidden120FpsWaitBranchRva);
            if (!runtime_patch::MatchesBytes(function_address,
                                             kPresentFunctionSignature)) {
                log::WarnF(
                    "engine_patch name=hidden_120_fps_cap "
                    "result=signature_mismatch "
                    "target=0x%p disabled=1",
                    reinterpret_cast<void*>(function_address));
            } else {
                const auto result = g_static_patches.Apply(
                    "hidden_120_fps_cap", branch_address,
                    kHidden120FpsWaitBranch, kSkipHidden120FpsWait);
                LogPatchResult("hidden_120_fps_cap", branch_address, result);
                g_hidden_cap_resolved =
                    result == runtime_patch::ApplyResult::Applied ||
                    result == runtime_patch::ApplyResult::AlreadyApplied;
            }
        }

        if ((config.fix_corrupt_save_handling && !g_save_guard_resolved) ||
            (config.fix_thread_creation_failure && !g_thread_guard_resolved)) {
            if (!BuildRelayLayout(module_base, latest_steam_layout)) {
                log::Warn(
                    "engine_patch group=robustness_relays disabled=1 "
                    "reason=allocation_or_seal_failed");
            } else {
                if (config.fix_corrupt_save_handling &&
                    !g_save_guard_resolved) {
                    const bool applied =
                        ApplySaveGuards(module_base, latest_steam_layout);
                    g_save_guard_resolved = applied;
                    log::InfoF(
                        "engine_fix corrupt_save_guard=%d minimum_payload=0x%X "
                        "minimum_file=0x%X",
                        applied ? 1 : 0, kMinimumSavePayloadSize,
                        kMinimumSaveFileSize);
                }
                if (config.fix_thread_creation_failure &&
                    !g_thread_guard_resolved) {
                    const bool applied = ApplyThreadFailureGuards(
                        module_base, latest_steam_layout);
                    g_thread_guard_resolved = applied;
                    log::InfoF("engine_fix thread_failure_guard=%d",
                               applied ? 1 : 0);
                }
            }
        }

        if (AllRequestedFeaturesResolved(requested_mask)) {
            break;
        }
        if (attempt + 1 < kFeatureApplyAttempts) {
            // A failed write can be caused by a thread currently executing
            // across the target. Give that thread a scheduling turn before
            // retrying unresolved features; already-resolved siblings are
            // skipped by their per-feature state flags.
            SwitchToThread();
        }
    }

    const bool complete = AllRequestedFeaturesResolved(requested_mask);
    if (!complete) {
        log::ErrorF(
            "engine_patch group=static result=incomplete requested_mask=0x%X",
            requested_mask);
    }
    return complete;
}

bool ShutdownStaticPatches() {
    std::lock_guard<std::mutex> lifecycle_lock(g_lifecycle_mutex);
    if (!g_initialized.load(std::memory_order_acquire)) {
        // There is no owned byte range to restore, so a caller that is
        // deliberately tearing down/restarting the hook lifecycle can start
        // the feature-resolution state machine from a clean slate as well.
        ReleaseUnusedBankShutdownFenceEventReserve();
        ResetFeatureResolutionState();
        return true;
    }
    if (g_static_patches.RestoreAll()) {
        ReleaseUnusedBankShutdownFenceEventReserve();
        ResetFeatureResolutionState();
        g_initialized.store(false, std::memory_order_release);
        return true;
    } else {
        log::Warn("engine_patch group=static restore_incomplete=1");
        return false;
    }
}

bool IsInitialized() {
    return g_initialized.load(std::memory_order_acquire);
}

}  // namespace spatch::engine_fixes
