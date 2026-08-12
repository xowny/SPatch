#include "../src/TextureFilteringPolicy.h"

#include <array>
#include <cstdint>

namespace {

using namespace spatch::texture_filtering;

static_assert(IsAcceptedAnisotropy(kOriginalAnisotropy));
static_assert(IsAcceptedAnisotropy(kAnisotropy4x));
static_assert(IsAcceptedAnisotropy(kAnisotropy8x));
static_assert(IsAcceptedAnisotropy(kAnisotropy16x));
static_assert(!IsAcceptedAnisotropy(0));
static_assert(!IsAcceptedAnisotropy(2));
static_assert(!IsAcceptedAnisotropy(32));

static_assert(ResolveWriterExponent(0, kOriginalAnisotropy) == 0);
static_assert(ResolveWriterExponent(2, kOriginalAnisotropy) == 2);
static_assert(ResolveWriterExponent(7, 2) == 7);
static_assert(ResolveWriterExponent(7, kAnisotropy4x) == 2);
static_assert(ResolveWriterExponent(7, kAnisotropy8x) == 3);
static_assert(ResolveWriterExponent(7, kAnisotropy16x) == 4);
static_assert(!ShouldInstallWriter(kOriginalAnisotropy));
static_assert(!ShouldInstallWriter(2));
static_assert(ShouldInstallWriter(kAnisotropy4x));
static_assert(ShouldInstallWriter(kAnisotropy8x));
static_assert(ShouldInstallWriter(kAnisotropy16x));

static_assert(SelectAddresses(false) == AddressProfile{
    0x00A196E0, 0x00A21F00, 0x00A19788});
static_assert(SelectAddresses(true) == AddressProfile{
    0x00A195B0, 0x00A21DD0, 0x00A19658});
static_assert(kAnisotropyValueRva == 0x020F2A0C);

static_assert(kAnisotropyWriterSignature ==
              std::array<std::uint8_t, 19>{
                  0x48, 0x83, 0xEC, 0x28, 0x85, 0xC9, 0x74, 0x09, 0xB8, 0x01,
                  0x00, 0x00, 0x00, 0xD3, 0xE0, 0xEB, 0x02, 0x33, 0xC0});
static_assert(kSamplerBuilderPrologue ==
              std::array<std::uint8_t, 34>{
                  0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x60,
                  0x48, 0x8D, 0xBA, 0xC0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9,
                  0x48, 0x8B, 0x0F, 0x48, 0x85, 0xC9, 0x74, 0x0D, 0x48, 0x8B,
                  0x01, 0xFF, 0x50, 0x10});
static_assert(kForceBranchPrefix ==
              std::array<std::uint8_t, 20>{
                  0x85, 0xC9, 0x74, 0x2C, 0xFF, 0xC9, 0x74, 0x0C, 0xFF, 0xC9,
                  0x74, 0x1A, 0xFF, 0xC9, 0x74, 0x0F, 0xFF, 0xC9, 0x75, 0x31});
static_assert(kStockTrilinearInstruction ==
              std::array<std::uint8_t, 5>{0xB8, 0x15, 0x00, 0x00, 0x00});
static_assert(kForcedAnisotropicInstruction ==
              std::array<std::uint8_t, 5>{0xB8, 0x55, 0x00, 0x00, 0x00});
static_assert(kForceBranchSuffix ==
              std::array<std::uint8_t, 13>{
                  0x89, 0x44, 0x24, 0x20, 0xEB, 0x26, 0xB9,
                  0x55, 0x00, 0x00, 0x00, 0xEB, 0x1B});

}  // namespace
