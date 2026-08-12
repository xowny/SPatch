#include "ShenLongNative.hpp"

#include <array>
#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

}  // namespace

int main() {
    using namespace spatch::graphics::native;

    static_assert(kShenLongConfigVersion == 1,
                  "review ShenLong.ini compatibility before bumping it");

    Expect(ParseAoMode(L"Original") == AoMode::Original,
           "Original AO should parse");
    Expect(ParseAoMode(L"sdao") == AoMode::Sdao,
           "SDAO should parse case-insensitively");
    Expect(ParseAoMode(L"GTAO_Lite") == AoMode::GtaoLite,
           "GTAO Lite compatibility spelling should parse");
    Expect(ParseAoMode(L"invalid", AoMode::Sdao) == AoMode::Sdao,
           "invalid AO should retain the supplied fallback");
    Expect(UsesCustomAo(AoMode::Sdao) && UsesCustomAo(AoMode::GtaoLite) &&
               !UsesCustomAo(AoMode::Original),
           "only SDAO and GTAO Lite should request the native scheduler");
    Expect(ValidateOriginalAoQuality(-99) == -1 &&
               ValidateOriginalAoQuality(-1) == -1 &&
               ValidateOriginalAoQuality(0) == 0 &&
               ValidateOriginalAoQuality(1) == 1 &&
               ValidateOriginalAoQuality(2) == -1,
           "OriginalAOQuality should accept only -1, 0, and 1");

    const ExecutableProfile* legacy = FindExecutableProfile(
        kLegacyProfile.file_size,
        kLegacyProfile.timestamp,
        kLegacyProfile.size_of_image,
        kLegacyProfile.sha256);
    const ExecutableProfile* latest = FindExecutableProfile(
        kLatestSteamProfile.file_size,
        kLatestSteamProfile.timestamp,
        kLatestSteamProfile.size_of_image,
        kLatestSteamProfile.sha256);
    Expect(legacy == &kExecutableProfiles[0] &&
               std::string(legacy->id) == "legacy_researched" &&
               legacy->ao_stage_rva == 0x35370 &&
               legacy->hair_blur_submit_rva == 0x3E7C0,
           "legacy executable profile should retain verified mappings");
    Expect(latest == &kExecutableProfiles[1] &&
               std::string(latest->id) == "latest_steam" &&
               latest->ao_stage_rva == 0x35650 &&
               latest->hair_blur_submit_rva == 0x3EA60,
           "latest Steam executable profile should retain verified mappings");
    auto wrong_hash = kLegacyProfile.sha256;
    wrong_hash[0] ^= 0xFF;
    Expect(FindExecutableProfile(kLegacyProfile.file_size,
                                 kLegacyProfile.timestamp,
                                 kLegacyProfile.size_of_image,
                                 wrong_hash) == nullptr,
           "a mismatched SHA-256 should reject fixed-RVA mappings");
    Expect(FindExecutableProfile(kLegacyProfile.file_size,
                                 kLatestSteamProfile.timestamp,
                                 kLegacyProfile.size_of_image,
                                 kLegacyProfile.sha256) == nullptr,
           "mixed PE metadata and SHA-256 should reject a profile");
    Expect(kLegacyProfile.ao_stage_signature !=
               kLatestSteamProfile.ao_stage_signature &&
               kLegacyProfile.hair_blur_submit_signature ==
                   kLatestSteamProfile.hair_blur_submit_signature,
           "per-profile signatures should match the extracted executable bytes");

    {
        constexpr std::string_view xml =
            "<DisplaySettings>\r\n\t<SSAO>0</SSAO>\r\n</DisplaySettings>\r\n";
        const SsaoXmlInspection inspection = InspectSsaoXml(xml);
        Expect(inspection.status == SsaoXmlStatus::Ok &&
                   inspection.value == 0,
               "SSAO inspection should read a valid stock quality");
        const SsaoXmlEdit edit = EditSsaoXml(xml, 1);
        Expect(edit.status == SsaoXmlStatus::Ok && edit.changed &&
                   edit.previous_value == 0 &&
                   edit.text ==
                       "<DisplaySettings>\r\n\t<SSAO>1</SSAO>\r\n"
                       "</DisplaySettings>\r\n",
               "SSAO staging should preserve the surrounding CRLF XML");
    }
    {
        constexpr std::string_view xml =
            "\xEF\xBB\xBF<DisplaySettings>\n</DisplaySettings>\n";
        const SsaoXmlEdit edit = EditSsaoXml(xml, 0);
        Expect(edit.status == SsaoXmlStatus::Ok && edit.changed &&
                   !edit.previous_value.has_value() &&
                   edit.text ==
                       "\xEF\xBB\xBF<DisplaySettings>\n\t<SSAO>0</SSAO>\n"
                       "</DisplaySettings>\n",
               "missing SSAO should be inserted without stripping the BOM");
    }
    {
        constexpr std::string_view xml =
            "<DisplaySettings><SSAO> 1 </SSAO></DisplaySettings>";
        const SsaoXmlEdit edit = EditSsaoXml(xml, 1);
        Expect(edit.status == SsaoXmlStatus::Ok && !edit.changed &&
                   edit.previous_value == 1 && edit.text == xml,
               "an equivalent stock quality should not rewrite the file");
    }
    {
        constexpr std::string_view xml =
            "<DisplaySettings><SSAO>invalid</SSAO></DisplaySettings>";
        const SsaoXmlEdit edit = EditSsaoXml(xml, 0);
        Expect(edit.status == SsaoXmlStatus::Ok && edit.changed &&
                   !edit.previous_value.has_value() &&
                   edit.text ==
                       "<DisplaySettings><SSAO>0</SSAO></DisplaySettings>",
               "an explicit stock quality should repair a malformed value");
    }
    {
        constexpr std::string_view duplicate =
            "<DisplaySettings><SSAO>0</SSAO><SSAO>1</SSAO>"
            "</DisplaySettings>";
        Expect(InspectSsaoXml(duplicate).status == SsaoXmlStatus::Malformed,
               "duplicate SSAO tags should fail closed");
        Expect(InspectSsaoXml("<DisplaySettings><SSAO>0</DisplaySettings>")
                   .status == SsaoXmlStatus::Malformed,
               "unbalanced SSAO tags should fail closed");
        Expect(InspectSsaoXml("<SSAO>0</SSAO>").status ==
                   SsaoXmlStatus::Malformed,
               "XML without the DisplaySettings root should fail closed");
        Expect(EditSsaoXml("<DisplaySettings></DisplaySettings>", 2).status ==
                   SsaoXmlStatus::Malformed,
               "an invalid requested quality should fail closed");
    }

    Expect(ParseRestoreJournal("0\r\n") == 0 &&
               ParseRestoreJournal(" 1 \n") == 1,
           "restore journal should accept one bounded quality value");
    Expect(!ParseRestoreJournal("-1\n").has_value() &&
               !ParseRestoreJournal("1\nextra").has_value() &&
               !ParseRestoreJournal("").has_value(),
           "restore journal should reject ambiguous or invalid state");

    if (g_failures != 0) {
        std::cerr << g_failures << " ShenLong native policy test(s) failed\n";
        return 1;
    }
    std::cout << "PASS: ShenLong native policy tests\n";
    return 0;
}
