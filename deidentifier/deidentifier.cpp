#include "deidentifier.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>

namespace deid {

namespace {

std::string toLowerUtf8(const std::string& str) {
    std::string result;
    result.reserve(str.size());

    for (std::size_t i = 0; i < str.size(); ++i) {
        unsigned char c1 = static_cast<unsigned char>(str[i]);

        // UTF-8 Cyrillic lowercase conversion
        if (c1 == 0xD0 && (i + 1) < str.size()) {
            unsigned char c2 = static_cast<unsigned char>(str[i + 1]);
            if (c2 >= 0x90 && c2 <= 0x9F) { // А-П -> а-п
                result += static_cast<char>(0xD0);
                result += static_cast<char>(c2 + 0x20);
                ++i;
                continue;
            } else if (c2 >= 0xA0 && c2 <= 0xAF) { // Р-Я -> р-я
                result += static_cast<char>(0xD1);
                result += static_cast<char>(c2 - 0x20);
                ++i;
                continue;
            }
        }

        // Standard ASCII lowercase conversion
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

DeidentificationResult Deidentifier::deidentifyText(const std::string& text) {
    DeidentificationResult result;
    result.text = text;
    result.redactions = 0;
    return result;
}



std::vector<DetectedEntity> Deidentifier::resolveOverlaps(std::vector<DetectedEntity> entities) const {
    if (entities.empty()) return {};

    std::sort(entities.begin(), entities.end(), [](const DetectedEntity& a, const DetectedEntity& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.score != b.score) return a.score > b.score;
        return a.length > b.length;
    });

    std::vector<DetectedEntity> resolved;
    std::size_t lastEnd = 0;

    for (const auto& entity : entities) {
        if (entity.start >= lastEnd) {
            resolved.push_back(entity);
            lastEnd = entity.start + entity.length;
        }
    }

    return resolved;
}

std::string Deidentifier::getPseudonym(const std::string& entityText, EntityType type) {
    const std::string key = toLowerUtf8(entityText);
    const auto existing = pseudonyms_.find(key);
    if (existing != pseudonyms_.end()) {
        return existing->second;
    }

    const std::string prefix = entityTypeToString(type);
    const std::string pseudonym = "[" + prefix + "_" + std::to_string(pseudonyms_.size() + 1) + "]";
    pseudonyms_[key] = pseudonym;
    return pseudonym;
}

} // namespace deid