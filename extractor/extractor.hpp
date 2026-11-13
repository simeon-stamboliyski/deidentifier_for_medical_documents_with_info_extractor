#pragma once
 
#include <filesystem>
#include <string>
#include <vector>
 
namespace extractor {
 
extern const std::vector<std::string> FIELDS;
 
void processDocuments(
    const std::filesystem::path& folderPath,
    const std::filesystem::path& outputDir = ""
);
 
} // namespace extractor
 
