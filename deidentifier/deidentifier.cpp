#include "deidentifier.hpp"
#include <algorithm>
#include <cctype>

namespace deid {

namespace {

std::string trimPunctuationAndSpace(const std::string& str) {
    std::size_t start = 0;
    while (start < str.size() && (std::isspace(static_cast<unsigned char>(str[start])) || 
                                 std::ispunct(static_cast<unsigned char>(str[start])))) {
        ++start;
    }

    if (start >= str.size()) return "";

    std::size_t end = str.size() - 1;
    while (end > start && (std::isspace(static_cast<unsigned char>(str[end])) || 
                           std::ispunct(static_cast<unsigned char>(str[end])))) {
        --end;
    }

    return str.substr(start, end - start + 1);
}

std::string toLowerUtf8(const std::string& str) {
    std::string result;
    result.reserve(str.size());

    for (std::size_t i = 0; i < str.size(); ++i) {
        unsigned char c1 = static_cast<unsigned char>(str[i]);

        if (c1 == 0xD0 && (i + 1) < str.size()) {
            unsigned char c2 = static_cast<unsigned char>(str[i + 1]);

            if (c2 >= 0x90 && c2 <= 0x9F) {
                result += static_cast<char>(0xD0);
                result += static_cast<char>(c2 + 0x20);
                ++i;
                continue;
            }
            if (c2 >= 0xA0 && c2 <= 0xAF) {
                result += static_cast<char>(0xD1);
                result += static_cast<char>(c2 - 0x20);
                ++i;
                continue;
            }
            if (c2 == 0x8D) {
                result += static_cast<char>(0xD1);
                result += static_cast<char>(0x9D);
                ++i;
                continue;
            }
        }

        if (c1 < 128) {
            result += static_cast<char>(std::tolower(c1));
        } else {
            result += str[i];
        }
    }
    return result;
}

std::string entityTypeToString(EntityType type) {
    switch (type) {
    case EntityType::Email: return "EMAIL";
    case EntityType::Phone: return "PHONE";
    case EntityType::IPAddress: return "IP";
    case EntityType::IDNumber: return "ID";
    case EntityType::Date: return "DATE";
    case EntityType::URLToken: return "TOKEN";
    case EntityType::Person: return "PERSON";
    case EntityType::Location: return "LOCATION";
    case EntityType::Organization: return "ORG";
    }
    return "ENTITY";
}

} // namespace

void Deidentifier::reset() {
    pseudonyms_.clear();
    typeCounters_.clear();
}

DeidentificationResult Deidentifier::deidentifyText(const std::string& text) {
    const auto entities = detector_.detectAll(text);

    DeidentificationResult result;
    result.text.reserve(text.size());

    std::size_t previousEnd = 0;

    for (const auto& entity : entities) {
        if (entity.start < previousEnd) {
            continue; 
        }

        result.text.append(text, previousEnd, entity.start - previousEnd);
        result.text += getPseudonym(entity.text, entity.type);
        previousEnd = entity.start + entity.length;
        ++result.redactions;
    }

    result.text.append(text, previousEnd, std::string::npos);
    return result;
}

std::string Deidentifier::getPseudonym(const std::string& entityText, EntityType type) {
    const std::string cleaned = trimPunctuationAndSpace(entityText);
    const std::string key = toLowerUtf8(cleaned.empty() ? entityText : cleaned);

    const auto existing = pseudonyms_.find(key);
    if (existing != pseudonyms_.end()) {
        return existing->second;
    }

    const int typeKey = static_cast<int>(type);
    const std::size_t count = ++typeCounters_[typeKey];

    const std::string prefix = entityTypeToString(type);
    const std::string pseudonym = "[" + prefix + "_" + std::to_string(count) + "]";

    pseudonyms_[key] = pseudonym;
    return pseudonym;
}

} // namespace deid