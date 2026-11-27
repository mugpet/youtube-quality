#include "../desktop/QualityFormat.h"

#include <cstdio>

int main() {
    using namespace quality;
    struct AutoCase {
        const wchar_t* summary;    // Lowercase label as supplied by the menu reader.
        const wchar_t* bestOffer;
        bool alreadyBest;
    };
    const AutoCase automatic[] = {
        {L"quality auto (1440p60 hd)", L"1440p60 hd", true},
        {L"kvalitet automatisk (1440p60 hd)", L"1440p60 hd", true},
        {L"kvalitet auto (240p)", L"240p", true},
        {L"quality auto (1440p 2k)", L"1440p60 hd", true}, // "2K" is not a frame rate.
        {L"quality auto (1080p hd)", L"1080p60 premium", true},
        {L"quality auto (1080p standard)", L"1080p premium", false},
        {L"kvalitet automatisk (1080p forbedret bithastighed)", L"1080p premium", true},
        {L"quality auto (1080p30 hd)", L"1080p60 hd", false},
        {L"quality auto (1440p)", L"1080p60 hd", true}, // The list can still be loading.
        {L"quality auto (2160p hd)", L"1440p hd", false},
        {L"quality auto (480p)", L"720p", false},
    };
    for (const auto& sample : automatic) {
        const auto current = ParseReportedFormat(sample.summary);
        if (!current.height || PlayingAtBest(current, ParseReportedFormat(sample.bestOffer)) != sample.alreadyBest) {
            std::fwprintf(stderr, L"Automatic playback decision failed for %ls with %ls\n",
                          sample.summary, sample.bestOffer);
            return 1;
        }
    }
    for (const auto* loading : {L"quality auto", L"kvalitet automatisk (indlæser)"}) {
        if (ParseReportedFormat(loading).height != 0) {
            std::fwprintf(stderr, L"Unreported resolution was treated as known for %ls\n", loading);
            return 1;
        }
    }
    struct ManualCase {
        const wchar_t* summary;
        const wchar_t* checkedOffer;
        bool changing;
    };
    const ManualCase manual[] = {
        {L"quality 1440p", L"480p", true},
        {L"kvalitet 1080p60", L"1080p30", true},
        {L"quality 1080p premium", L"1080p", true},
        {L"quality 1080p standard", L"1080p premium", true},
        {L"quality 1080p hd", L"1080p hd", false},
    };
    for (const auto& sample : manual) {
        if (ManualSummaryDisagrees(ParseReportedFormat(sample.summary), ParseReportedFormat(sample.checkedOffer))
                != sample.changing) {
            std::fwprintf(stderr, L"Manual selection consistency failed for %ls with %ls\n",
                          sample.summary, sample.checkedOffer);
            return 1;
        }
    }
    std::puts("Quality format tests passed.");
}
