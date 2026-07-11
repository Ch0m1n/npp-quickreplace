#include "SnippetTemplate.h"

#include <algorithm>
#include <charconv>
#include <system_error>
#include <utility>

namespace nppqr {

SnippetExpansion parseSnippetMarkers(std::string_view source) {
    constexpr std::string_view cursorMarker = "${cursor}";
    constexpr std::string_view tabstopPrefix = "${tabstop:";
    struct Marker {
        unsigned int number = 0;
        std::size_t offset = 0;
    };

    SnippetExpansion result;
    result.text.reserve(source.size());
    std::vector<Marker> markers;
    std::optional<std::size_t> cursor;
    for (std::size_t index = 0; index < source.size();) {
        if (source.substr(index).starts_with(cursorMarker)) {
            if (!cursor.has_value()) cursor = result.text.size();
            index += cursorMarker.size();
            continue;
        }
        if (source.substr(index).starts_with(tabstopPrefix)) {
            const std::size_t numberStart = index + tabstopPrefix.size();
            const std::size_t close = source.find('}', numberStart);
            if (close != std::string_view::npos) {
                unsigned int number = 0;
                const auto parsed = std::from_chars(
                    source.data() + numberStart, source.data() + close, number);
                if (parsed.ec == std::errc{} && parsed.ptr == source.data() + close &&
                    number <= 9) {
                    markers.push_back({number, result.text.size()});
                    index = close + 1;
                    continue;
                }
            }
        }
        result.text.push_back(source[index++]);
    }

    std::stable_sort(markers.begin(), markers.end(), [](const Marker& left, const Marker& right) {
        const unsigned int leftOrder = left.number == 0 ? 10 : left.number;
        const unsigned int rightOrder = right.number == 0 ? 10 : right.number;
        return leftOrder < rightOrder;
    });
    result.tabstopOffsets.reserve(markers.size() + (cursor.has_value() ? 1U : 0U));
    bool hasFinalTabstop = false;
    for (const Marker& marker : markers) {
        result.tabstopOffsets.push_back(marker.offset);
        hasFinalTabstop = hasFinalTabstop || marker.number == 0;
    }
    if (!markers.empty() && cursor.has_value() && !hasFinalTabstop &&
        (result.tabstopOffsets.empty() || result.tabstopOffsets.back() != *cursor)) {
        result.tabstopOffsets.push_back(*cursor);
    }
    if (markers.empty()) result.cursorOffset = cursor;
    return result;
}

} // namespace nppqr
