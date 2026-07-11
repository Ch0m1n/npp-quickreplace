#include "CapturePattern.h"

#include <algorithm>
#include <charconv>
#include <cctype>

namespace nppqr {
namespace {

constexpr std::string_view kCapturePrefix = "${capture:";

std::string foldAscii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return character < 0x80U
            ? static_cast<char>(std::tolower(character))
            : static_cast<char>(character);
    });
    return result;
}

} // namespace

CapturePattern::CompileResult CapturePattern::compile(
    std::string_view pattern, bool caseSensitive) {
    tokens_.clear();
    captureNumbers_.fill(false);
    std::vector<Token> parsed;
    std::array<bool, 10> parsedNumbers{};
    bool hasCapture = false;
    std::size_t literalStart = 0;
    std::size_t position = pattern.find(kCapturePrefix);
    while (position != std::string_view::npos) {
        const std::size_t numberStart = position + kCapturePrefix.size();
        const std::size_t close = pattern.find('}', numberStart);
        if (close == std::string_view::npos) {
            return {.error = "A capture marker is missing its closing brace."};
        }
        unsigned number = 0;
        const auto conversion = std::from_chars(
            pattern.data() + numberStart, pattern.data() + close, number);
        if (conversion.ec != std::errc{} || conversion.ptr != pattern.data() + close ||
            number == 0 || number > 9) {
            return {.error = "Capture numbers must be integers from 1 through 9."};
        }
        if (parsedNumbers[number]) {
            return {.error = "Each capture number may appear only once in a trigger."};
        }
        parsedNumbers[number] = true;
        hasCapture = true;

        std::string literal(pattern.substr(literalStart, position - literalStart));
        parsed.push_back({.text = literal, .foldedText = foldAscii(literal)});
        parsed.push_back({.capture = true, .number = number});
        literalStart = close + 1;
        position = pattern.find(kCapturePrefix, literalStart);
    }
    if (!hasCapture) {
        return {.error = "A capture template needs at least one ${capture:N} marker."};
    }
    std::string tail(pattern.substr(literalStart));
    parsed.push_back({.text = tail, .foldedText = foldAscii(tail)});

    for (std::size_t index = 1; index + 1 < parsed.size(); index += 2) {
        if (parsed[index + 1].text.empty() && index + 2 < parsed.size()) {
            return {.error = "Adjacent capture markers need literal text between them."};
        }
    }

    tokens_ = std::move(parsed);
    captureNumbers_ = parsedNumbers;
    caseSensitive_ = caseSensitive;
    return {.ok = true};
}

std::optional<unsigned char> CapturePattern::leadingByte() const noexcept {
    if (tokens_.empty() || tokens_.front().capture || tokens_.front().text.empty()) {
        return std::nullopt;
    }
    unsigned char value = static_cast<unsigned char>(tokens_.front().text.front());
    if (value < 0x80U) value = static_cast<unsigned char>(std::tolower(value));
    return value;
}

CapturePattern::CompileResult CapturePattern::validateReplacement(
    std::string_view replacement) const {
    std::size_t position = replacement.find(kCapturePrefix);
    while (position != std::string_view::npos) {
        const std::size_t numberStart = position + kCapturePrefix.size();
        const std::size_t close = replacement.find('}', numberStart);
        if (close == std::string_view::npos) {
            return {.error = "A replacement capture marker is missing its closing brace."};
        }
        unsigned number = 0;
        const auto conversion = std::from_chars(
            replacement.data() + numberStart, replacement.data() + close, number);
        if (conversion.ec != std::errc{} || conversion.ptr != replacement.data() + close ||
            number == 0 || number > 9 || !captureNumbers_[number]) {
            return {.error = "A replacement references a capture number not defined by its trigger."};
        }
        position = replacement.find(kCapturePrefix, close + 1);
    }
    return {.ok = true};
}
std::optional<CaptureMatch> CapturePattern::match(std::string_view value) const {
    if (tokens_.empty()) return std::nullopt;
    const std::string foldedValue = caseSensitive_ ? std::string{} : foldAscii(value);
    const std::string_view searchable = caseSensitive_ ? value : std::string_view(foldedValue);
    CaptureMatch result;
    std::size_t offset = 0;

    for (std::size_t index = 0; index < tokens_.size(); ++index) {
        const Token& token = tokens_[index];
        if (!token.capture) {
            const std::string_view literal = caseSensitive_ ? token.text : token.foldedText;
            if (!searchable.substr(offset).starts_with(literal)) return std::nullopt;
            offset += literal.size();
            continue;
        }

        const Token& nextLiteralToken = tokens_[index + 1];
        const std::string_view nextLiteral = caseSensitive_
            ? std::string_view(nextLiteralToken.text)
            : std::string_view(nextLiteralToken.foldedText);
        const std::size_t end = nextLiteral.empty()
            ? searchable.size()
            : searchable.find(nextLiteral, offset);
        if (end == std::string_view::npos || end == offset) return std::nullopt;
        result.values[token.number] = std::string(value.substr(offset, end - offset));
        offset = end;
    }
    if (offset != value.size()) return std::nullopt;
    return result;
}

} // namespace nppqr