#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace nppqr {

enum class DelimitedImportMode {
    append,
    replace,
};

struct RuleExchangeResult {
    bool ok = false;
    std::size_t itemCount = 0;
    std::string text;
    std::string error;
    std::vector<std::string> warnings;
};

class RuleExchange {
public:
    static RuleExchangeResult importDelimited(
        std::string_view existingJson,
        std::string_view delimitedText,
        char delimiter,
        DelimitedImportMode mode);

    static RuleExchangeResult exportDelimited(
        std::string_view replacementsJson,
        char delimiter);
};

} // namespace nppqr
