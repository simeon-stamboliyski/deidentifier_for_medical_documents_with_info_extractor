#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "detectors.hpp"

namespace deid {

struct DeidentificationResult {
    std::string text;
    std::size_t redactions = 0;
};

class Deidentifier {
public:
    Deidentifier() = default;

    void reset();

    DeidentificationResult deidentifyText(const std::string& text);

private:
    std::string getPseudonym(const std::string& entityText, EntityType type);

    EntityDetector detector_;
    std::unordered_map<std::string, std::string> pseudonyms_;
    std::unordered_map<int, std::size_t> typeCounters_;
};

} // namespace deid