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

    std::size_t index = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEFU &&
        static_cast<unsigned char>(text[1]) == 0xBBU &&
        static_cast<unsigned char>(text[2]) == 0xBFU) {
        index = 3;
    }
    for (; index < text.size(); ++index) {
        const char character = text[index];
        if (quoted) {
            if (character == '"') {
                if (index + 1 < text.size() && text[index + 1] == '"') {
                    field.push_back('"');
                    ++index;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(character);
            }
            continue;
        }
        if (character == '"' && !fieldStarted) {
            quoted = true;
            fieldStarted = true;
        } else if (character == delimiter) {
            row.push_back(std::move(field));
            field.clear();
            fieldStarted = false;
        } else if (character == '\r' || character == '\n') {
            if (character == '\r' && index + 1 < text.size() && text[index + 1] == '\n') ++index;
            row.push_back(std::move(field));
            field.clear();
            fieldStarted = false;
            const bool blank = row.size() == 1 && row.front().empty();
            if (!blank) rows.push_back(std::move(row));
            row.clear();
        } else {
            field.push_back(character);
            fieldStarted = true;
        }
    }
    if (quoted) throw std::runtime_error("The delimited file ends inside a quoted field.");
    if (fieldStarted || !field.empty() || !row.empty()) {
        row.push_back(std::move(field));
        const bool blank = row.size() == 1 && row.front().empty();
        if (!blank) rows.push_back(std::move(row));
    }
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

std::string makeImportId(std::string_view trigger, std::size_t row,
    const std::unordered_set<std::string>& existingIds) {
    const std::size_t hash = std::hash<std::string_view>{}(trigger);
    std::string base = "import-" + std::to_string(hash) + "-" + std::to_string(row);
    std::string result = base;
    std::size_t suffix = 2;
    while (existingIds.contains(result)) result = base + "-" + std::to_string(suffix++);
    return result;
}

std::unordered_map<std::string, std::size_t> headerIndex(const std::vector<std::string>& header) {
    std::unordered_map<std::string, std::size_t> result;
    for (std::size_t index = 0; index < header.size(); ++index) {
        result[RuleStore::foldAscii(trimAscii(header[index]))] = index;
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
            const std::string trigger = cell(row, header, "trigger");
            const std::string replacement = cell(row, header, "replacement");
            if (trigger.empty() && replacement.empty()) continue;
            if (trigger.empty() || replacement.empty()) {
                throw std::runtime_error("Import row " + std::to_string(rowIndex + 1) +
                    " must contain both trigger and replacement.");
            }
            std::string id = cell(row, header, "id");
            if (id.empty()) id = makeImportId(trigger, rowIndex + 1, existingIds);
            if (existingIds.contains(id)) {
                throw std::runtime_error("Import row " + std::to_string(rowIndex + 1) +
                    " uses duplicate id '" + id + "'.");
            }
            existingIds.insert(id);
            imported.push_back({
                {"id", id},
                {"enabled", parseBoolean(cell(row, header, "enabled"), true)},
                {"trigger", trigger},
                {"replacement", replacement},
                {"group", trimAscii(cell(row, header, "group"))},
                {"matchMode", "wholeWord"},
                {"caseSensitive", parseBoolean(cell(row, header, "casesensitive"), false)},
                {"activation", activationJson(cell(row, header, "activation"))},
                {"fileExtensions", extensionsJson(cell(row, header, "fileextensions"))},
                {"description", cell(row, header, "description")},
            });
        }
        if (imported.empty()) throw std::runtime_error("The import file contains no rule rows.");

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
    char delimiter) {
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
            "id", "enabled", "trigger", "replacement", "group", "activation",
            "caseSensitive", "fileExtensions", "description",
        };
        std::string output = "\xEF\xBB\xBF";
        const auto appendRow = [&](const std::vector<std::string>& values, std::string& target) {
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (index != 0) target.push_back(delimiter);
                target.append(quoteField(values[index], delimiter));
            }
            target.append("\r\n");
        };
        appendRow(headers, output);
        for (const auto& item : root["items"]) {
            if (!item.is_object()) continue;
            appendRow({
                item.value("id", ""),
                item.value("enabled", true) ? "true" : "false",
                item.value("trigger", ""),
                item.value("replacement", ""),
                item.value("group", ""),
                joinJsonArray(item.value("activation", Json::array())),
                item.value("caseSensitive", false) ? "true" : "false",
                joinJsonArray(item.value("fileExtensions", Json::array())),
                item.value("description", ""),
            }, output);
            ++result.itemCount;
        }
        result.ok = true;
        result.text = std::move(output);
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

} // namespace nppqr
