#pragma once

#include <filesystem>
#include <string>

namespace extractor {

void processDocuments(
    const std::filesystem::path& folderPath,
    const std::filesystem::path& outputDir = ""
);

} // namespace extractor