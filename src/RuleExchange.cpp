#include "RuleExchange.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "RuleStore.h"

namespace nppqr {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kMaxDelimitedBytes = 64U * 1024U * 1024U;

std::string trimAscii(std::string value) {
    const auto whitespace = [](unsigned char character) { return std::isspace(character) != 0; };
    while (!value.empty() && whitespace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && whitespace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::vector<std::vector<std::string>> parseDelimited(std::string_view text, char delimiter) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    bool fieldStarted = false;
    bool quoteClosed = false;
    std::size_t line = 1;
    std::size_t column = 1;

    std::size_t index = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEFU &&
        static_cast<unsigned char>(text[1]) == 0xBBU &&
        static_cast<unsigned char>(text[2]) == 0xBFU) {
        index = 3;
    }
    const auto finishField = [&]() {
        row.push_back(std::move(field));
        field.clear();
        fieldStarted = false;
        quoteClosed = false;
    };
    const auto finishRow = [&]() {
        finishField();
        const bool blank = row.size() == 1 && row.front().empty();
        if (!blank) rows.push_back(std::move(row));
        row.clear();
    };
    for (; index < text.size(); ++index) {
        const char character = text[index];
        if (quoted) {
            if (character == '"') {
                if (index + 1 < text.size() && text[index + 1] == '"') {
                    field.push_back('"');
                    ++index;
                    column += 2;
                } else {
                    quoted = false;
                    quoteClosed = true;
                    ++column;
                }
            } else if (character == '\r' || character == '\n') {
                field.push_back(character);
                if (character == '\r' && index + 1 < text.size() && text[index + 1] == '\n') {
                    field.push_back('\n');
                    ++index;
                }
                ++line;
                column = 1;
            } else {
                field.push_back(character);
                ++column;
            }
            continue;
        }
        if (quoteClosed && character != delimiter && character != '\r' && character != '\n') {
            throw std::runtime_error("Unexpected character after a closing quote at line " +
                std::to_string(line) + ", column " + std::to_string(column) + ".");
        }
        if (character == '"') {
            if (fieldStarted) {
                throw std::runtime_error("Unexpected quote in an unquoted field at line " +
                    std::to_string(line) + ", column " + std::to_string(column) + ".");
            }
            quoted = true;
            fieldStarted = true;
            ++column;
        } else if (character == delimiter) {
            finishField();
            ++column;
        } else if (character == '\r' || character == '\n') {
            if (character == '\r' && index + 1 < text.size() && text[index + 1] == '\n') ++index;
            finishRow();
            ++line;
            column = 1;
        } else {
            field.push_back(character);
            fieldStarted = true;
            ++column;
        }
    }
    if (quoted) {
        throw std::runtime_error("The delimited file ends inside a quoted field at line " +
            std::to_string(line) + ", column " + std::to_string(column) + ".");
    }
    if (fieldStarted || quoteClosed || !field.empty() || !row.empty()) finishRow();
    return rows;
}
bool parseBoolean(std::string value, bool defaultValue) {
    value = RuleStore::foldAscii(trimAscii(std::move(value)));
    if (value.empty()) return defaultValue;
    if (value == "true" || value == "1" || value == "yes" || value == "on") return true;
    if (value == "false" || value == "0" || value == "no" || value == "off") return false;
    throw std::runtime_error("Boolean value '" + value + "' is not recognized.");
}

std::vector<std::string> splitList(std::string_view text) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('|', start);
        std::string value = trimAscii(std::string(text.substr(start, end - start)));
        if (!value.empty()) result.push_back(std::move(value));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

std::string joinJsonArray(const Json& value) {
    if (!value.is_array()) return {};
    std::string result;
    for (const auto& entry : value) {
        if (!entry.is_string()) continue;
        if (!result.empty()) result.push_back('|');
        result.append(entry.get<std::string>());
    }
    return result;
}

std::string quoteField(std::string_view value, char delimiter) {
    const bool needsQuotes = value.find(delimiter) != std::string_view::npos ||
        value.find('"') != std::string_view::npos || value.find('\r') != std::string_view::npos ||
        value.find('\n') != std::string_view::npos;
    if (!needsQuotes) return std::string(value);
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const char character : value) {
        if (character == '"') result.append("\"\"");
        else result.push_back(character);
    }
    result.push_back('"');
    return result;
}

std::uint64_t stableHash(std::string_view value) noexcept {
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= prime;
    }
    return hash;
}

std::string makeImportId(std::string_view trigger, std::string_view replacement,
    const std::unordered_set<std::string>& existingIds) {
    std::string identity(trigger);
    identity.push_back('\0');
    identity.append(replacement);
    const std::string base = "import-" + std::to_string(stableHash(identity));
    std::string result = base;
    std::size_t suffix = 2;
    while (existingIds.contains(result)) result = base + "-" + std::to_string(suffix++);
    return result;
}
std::unordered_map<std::string, std::size_t> headerIndex(const std::vector<std::string>& header) {
    std::unordered_map<std::string, std::size_t> result;
    for (std::size_t index = 0; index < header.size(); ++index) {
        const std::string name = RuleStore::foldAscii(trimAscii(header[index]));
        if (name.empty()) {
            throw std::runtime_error("The import header contains an empty column at column " +
                std::to_string(index + 1) + ".");
        }
        if (!result.emplace(name, index).second) {
            throw std::runtime_error("Duplicate import header '" + name + "' at column " +
                std::to_string(index + 1) + ".");
        }
    }
    return result;
}
std::string cell(const std::vector<std::string>& row,
    const std::unordered_map<std::string, std::size_t>& header, std::string_view name) {
    const auto found = header.find(std::string(name));
    if (found == header.end() || found->second >= row.size()) return {};
    return row[found->second];
}

Json activationJson(std::string_view value) {
    Json result = Json::array();
    const auto names = splitList(value.empty() ? "space|enter|tab" : value);
    for (std::string name : names) {
        name = RuleStore::foldAscii(std::move(name));
        result.push_back(std::move(name));
    }
    return result;
}

Json extensionsJson(std::string_view value) {
    Json result = Json::array();
    for (std::string extension : splitList(value)) {
        if (!extension.empty() && extension.front() != '.') extension.insert(extension.begin(), '.');
        result.push_back(std::move(extension));
    }
    return result;
}

void ensureImportedGroups(Json& root, const Json& imported,
    std::vector<std::string>& warnings) {
    if (!root.contains("groups") || !root["groups"].is_array()) root["groups"] = Json::array();
    std::unordered_set<std::string> known;
    for (const auto& group : root["groups"]) {
        if (!group.is_object()) continue;
        known.insert(group.value("id", ""));
        known.insert(group.value("name", ""));
    }
    for (const auto& item : imported) {
        const std::string group = item.value("group", "");
        if (group.empty() || known.contains(group)) continue;
        root["groups"].push_back({{"id", group}, {"name", group}, {"enabled", true}});
        known.insert(group);
        warnings.push_back("Created group '" + group + "' from imported rules.");
    }
}

Json extraFields(const Json& item) {
    static const std::unordered_set<std::string> known{
        "id", "enabled", "trigger", "replacement", "group", "matchMode",
        "caseSensitive", "activation", "fileExtensions", "pathGlobs", "languages", "description",
    };
    Json extra = item;
    for (const auto& name : known) extra.erase(name);
    return extra;
}

bool spreadsheetFormulaRisk(std::string_view value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return false;
    return value[first] == '=' || value[first] == '+' || value[first] == '-' || value[first] == '@';
}

std::string spreadsheetCell(std::string value, bool safe, std::size_t& riskyCells) {
    if (!spreadsheetFormulaRisk(value)) return value;
    ++riskyCells;
    if (safe) value.insert(value.begin(), '\'');
    return value;
}
} // namespace

RuleExchangeResult RuleExchange::importDelimited(
    std::string_view existingJson,
    std::string_view delimitedText,
    char delimiter,
    DelimitedImportMode mode) {
    RuleExchangeResult result;
    try {
        if (delimiter != ',' && delimiter != '\t') {
            throw std::runtime_error("Only comma and tab delimiters are supported.");
        }
        if (delimitedText.size() > kMaxDelimitedBytes) {
            throw std::runtime_error("The import file exceeds the 64 MiB safety limit.");
        }
        const auto rows = parseDelimited(delimitedText, delimiter);
        if (rows.empty()) throw std::runtime_error("The import file is empty.");
        const auto header = headerIndex(rows.front());
        if (!header.contains("trigger") || !header.contains("replacement")) {
            throw std::runtime_error("The import header must contain trigger and replacement columns.");
        }

        Json root = Json::parse(existingJson.begin(), existingJson.end());
        if (!root.is_object()) throw std::runtime_error("The existing replacements root is not an object.");
        if (!root.contains("items") || !root["items"].is_array()) root["items"] = Json::array();
        std::unordered_set<std::string> existingIds;
        if (mode == DelimitedImportMode::append) {
            for (const auto& item : root["items"]) {
                if (item.is_object()) existingIds.insert(item.value("id", ""));
            }
        }

        Json imported = Json::array();
        for (std::size_t rowIndex = 1; rowIndex < rows.size(); ++rowIndex) {
            const auto& row = rows[rowIndex];
            try {
                const std::string trigger = cell(row, header, "trigger");
                const std::string replacement = cell(row, header, "replacement");
                if (trigger.empty() && replacement.empty()) continue;
                if (trigger.empty() || replacement.empty()) {
                    throw std::runtime_error("must contain both trigger and replacement.");
                }
                std::string id = cell(row, header, "id");
                if (id.empty()) id = makeImportId(trigger, replacement, existingIds);
                if (existingIds.contains(id)) {
                    throw std::runtime_error("uses duplicate id '" + id + "'.");
                }
                existingIds.insert(id);

                Json item = Json::object();
                const std::string extraJson = trimAscii(cell(row, header, "extrajson"));
                if (!extraJson.empty()) {
                    item = Json::parse(extraJson);
                    if (!item.is_object()) {
                        throw std::runtime_error("extraJson must be a JSON object.");
                    }
                }
                item["id"] = id;
                item["enabled"] = parseBoolean(cell(row, header, "enabled"), true);
                item["trigger"] = trigger;
                item["replacement"] = replacement;
                item["group"] = trimAscii(cell(row, header, "group"));
                item["matchMode"] = trimAscii(cell(row, header, "matchmode"));
                if (item["matchMode"].get_ref<const std::string&>().empty()) {
                    item["matchMode"] = "wholeWord";
                }
                item["caseSensitive"] =
                    parseBoolean(cell(row, header, "casesensitive"), false);
                item["activation"] = activationJson(cell(row, header, "activation"));
                item["fileExtensions"] = extensionsJson(cell(row, header, "fileextensions"));
                item["pathGlobs"] = splitList(cell(row, header, "pathglobs"));
                item["languages"] = splitList(cell(row, header, "languages"));
                item["description"] = cell(row, header, "description");
                imported.push_back(std::move(item));
            } catch (const std::exception& exception) {
                throw std::runtime_error("Import row " + std::to_string(rowIndex + 1) +
                    ": " + exception.what());
            }
        }        if (imported.empty()) throw std::runtime_error("The import file contains no rule rows.");

        if (mode == DelimitedImportMode::replace) root["items"] = Json::array();
        ensureImportedGroups(root, imported, result.warnings);
        for (auto& item : imported) root["items"].push_back(std::move(item));

        RuleStore validator;
        const RuleLoadResult validation = validator.loadFromText(root.dump());
        if (!validation.ok) throw std::runtime_error(validation.error);
        result.warnings.insert(result.warnings.end(),
            validation.warnings.begin(), validation.warnings.end());
        result.ok = true;
        result.itemCount = imported.size();
        result.text = root.dump(2);
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

RuleExchangeResult RuleExchange::exportDelimited(
    std::string_view replacementsJson,
    char delimiter,
    bool spreadsheetSafe) {
    RuleExchangeResult result;
    try {
        if (delimiter != ',' && delimiter != '\t') {
            throw std::runtime_error("Only comma and tab delimiters are supported.");
        }
        const Json root = Json::parse(replacementsJson.begin(), replacementsJson.end());
        if (!root.is_object() || !root.contains("items") || !root["items"].is_array()) {
            throw std::runtime_error("The replacements document has no items array.");
        }
        const std::vector<std::string> headers{
            "id", "enabled", "trigger", "replacement", "group", "matchMode", "activation",
            "caseSensitive", "fileExtensions", "pathGlobs", "languages", "description", "extraJson",
        };
        std::string output = "\xEF\xBB\xBF";
        std::size_t riskyCells = 0;
        const auto appendRow = [&](const std::vector<std::string>& values, std::string& target) {
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (index != 0) target.push_back(delimiter);
                target.append(quoteField(
                    spreadsheetCell(values[index], spreadsheetSafe, riskyCells), delimiter));
            }
            target.append("\r\n");
        };
        appendRow(headers, output);
        for (const auto& item : root["items"]) {
            if (!item.is_object()) continue;
            const Json extra = extraFields(item);
            appendRow({
                item.value("id", ""),
                item.value("enabled", true) ? "true" : "false",
                item.value("trigger", ""),
                item.value("replacement", ""),
                item.value("group", ""),
                item.value("matchMode", "wholeWord"),
                joinJsonArray(item.value("activation", Json::array())),
                item.value("caseSensitive", false) ? "true" : "false",
                joinJsonArray(item.value("fileExtensions", Json::array())),
                joinJsonArray(item.value("pathGlobs", Json::array())),
                joinJsonArray(item.value("languages", Json::array())),
                item.value("description", ""),
                extra.empty() ? std::string{} : extra.dump(),
            }, output);
            ++result.itemCount;
        }
        if (riskyCells != 0) {
            result.warnings.push_back(spreadsheetSafe
                ? "Prefixed " + std::to_string(riskyCells) +
                    " spreadsheet-formula cell(s) with an apostrophe; this export is presentation-safe, not lossless."
                : "Detected " + std::to_string(riskyCells) +
                    " cell(s) that spreadsheet software may evaluate as formulas.");
        }
        result.ok = true;
        result.text = std::move(output);
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}
} // namespace nppqr
