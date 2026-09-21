#include <filesystem>
#include <iostream>
#include <string>
#include <cstdlib>

#include "deidentifier/deidentifier.hpp"
#include "file_processor/file_processor.hpp"
#include "extractor/extractor.hpp"

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(NULL);

    const fs::path inputFolder = deid::getInputFolder(argc, argv);

    if (!fs::is_directory(inputFolder)) {
        std::cerr << "Error: the path is not a valid folder.\n";
        return 1;
    }

    const auto files = deid::findSupportedFiles(inputFolder);
    if (files.empty()) {
        std::cout << "No supported documents found to deidentify.\n";
        return 0;
    }

    std::string homeDir = std::getenv("HOME") ? std::getenv("HOME") : ".";
    fs::path desktopPath = fs::path(homeDir) / "Desktop";
    const fs::path deidentifiedOutputFolder = desktopPath / "files_for_deidentifying_deidentified";

    std::cout << "Found " << files.size() << " supported file(s).\n";
    std::cout << "Writing deidentified files to: " << deidentifiedOutputFolder.string() << "\n\n";

    fs::create_directories(deidentifiedOutputFolder);

    deid::Deidentifier deidentifier;
    deid::ProcessingSummary summary = {};

    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto& file = files[index];
        std::cout << "[" << (index + 1) << "/" << files.size() << "] " << file.string() << "\n";

        const auto result = deid::processFile(file, inputFolder, deidentifiedOutputFolder, deidentifier);
        deid::addToSummary(summary, result);

        std::cout << "  " << result.message << "\n";
    }

    std::cout << "\nDeidentification completed. Starting information extraction...\n";

    extractor::processDocuments(deidentifiedOutputFolder);

    return summary.failed == 0 ? 0 : 2;
}