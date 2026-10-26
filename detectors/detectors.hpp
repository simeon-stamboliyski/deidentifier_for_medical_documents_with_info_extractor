#pragma once

#include <cstddef>
#include <regex>
#include <string>
#include <vector>

namespace deid {

enum class EntityType {
    Email,
    Phone,
    IPAddress,
    IDNumber,
    Date,
    URLToken,
    Person,
    Location,
    Organization
};

struct DetectedEntity {
    EntityType type;
    std::size_t start;
    std::size_t length;
    std::string text;
    double score; // Confidence score (0.0 to 1.0)
};

class EntityDetector {
public:
    EntityDetector();
    std::vector<DetectedEntity> detectAll(const std::string& text) const;

private:
    void detectRegex(const std::string& text, std::vector<DetectedEntity>& results) const;
    void detectLNCh(const std::string& text, std::vector<DetectedEntity>& results) const;
    void detectUIN(const std::string& text, std::vector<DetectedEntity>& results) const;
    void detectDictionaries(const std::string& text, std::vector<DetectedEntity>& results) const;
    void detectNER(const std::string& text, std::vector<DetectedEntity>& results) const;
    void detectEGN(const std::string& text, std::vector<DetectedEntity>& results) const;

    std::vector<std::pair<EntityType, std::regex>> regexes_;
    std::vector<std::pair<EntityType, std::vector<std::string>>> dictionaries_;
};

} // namespace deid