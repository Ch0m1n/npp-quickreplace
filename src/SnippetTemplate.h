#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nppqr {

struct SnippetExpansion {
    std::string text;
    std::optional<std::size_t> cursorOffset;
    std::vector<std::size_t> tabstopOffsets;
};

[[nodiscard]] SnippetExpansion parseSnippetMarkers(std::string_view source);

} // namespace nppqr
