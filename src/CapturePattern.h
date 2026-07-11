#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nppqr {

struct CaptureMatch {
    std::array<std::string, 10> values;
};

class CapturePattern {
public:
    struct CompileResult {
        bool ok = false;
        std::string error;
    };

    CompileResult compile(std::string_view pattern, bool caseSensitive);
    [[nodiscard]] CompileResult validateReplacement(std::string_view replacement) const;
    [[nodiscard]] std::optional<CaptureMatch> match(std::string_view value) const;
    [[nodiscard]] bool empty() const noexcept { return tokens_.empty(); }

private:
    struct Token {
        bool capture = false;
        unsigned number = 0;
        std::string text;
        std::string foldedText;
    };

    std::vector<Token> tokens_;
    std::array<bool, 10> captureNumbers_{};
    bool caseSensitive_ = false;
};

} // namespace nppqr