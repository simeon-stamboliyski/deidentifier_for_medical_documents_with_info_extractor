#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "deidentifier/deidentifier.hpp"
#include "extractor/extractor.hpp"
#include "file_processor/file_processor.hpp"

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    const fs::path inputFolder = deid::getInputFolder(argc, argv);

    std::error_code ec;
    if (!fs::is_directory(inputFolder, ec)) {
        std::cerr << "Грешка: Въведеният път не е валидна директория.\n";
        return 1;
    }

    const auto files = deid::findSupportedFiles(inputFolder);
    if (files.empty()) {
        std::cout << "Не бяха открити поддържани документи за обработка.\n";
        return 0;
    }

    const fs::path deidentifiedOutputFolder = deid::createOutputFolder(inputFolder);

    std::cout << "Намерени са " << files.size() << " поддържан(и) документ(а).\n";
    std::cout << "Изходна папка за анонимизирани файлове: " << deidentifiedOutputFolder.string() << "\n\n";

    deid::Deidentifier deidentifier;
    deid::ProcessingSummary summary = {};

    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto& file = files[index];
        std::cout << "[" << (index + 1) << "/" << files.size() << "] " << file.filename().string() << "\n";

        const auto result = deid::processFile(file, inputFolder, deidentifiedOutputFolder, deidentifier);
        deid::addToSummary(summary, result);

        std::cout << "  " << result.message << "\n";
    }

    std::cout << "\n--- Резюме на анонимизацията ---\n";
    std::cout << "Успешно обработени: " << summary.processed << "\n";
    std::cout << "Пропуснати:        " << summary.skipped << "\n";
    std::cout << "Неуспешни:         " << summary.failed << "\n";
    std::cout << "Общ брой редакции: " << summary.redactions << "\n";
    std::cout << "--------------------------------\n\n";

    if (summary.processed > 0) {
        std::cout << "Стартиране на извличането на информация от епикризите...\n";
        const fs::path extractionOutputDir = deidentifiedOutputFolder / "extracted_data";
        extractor::processDocuments(deidentifiedOutputFolder, extractionOutputDir);
        std::cout << "Извличането приключи. Резултатите са записани в: " << extractionOutputDir.string() << "\n";
    }

    return (summary.failed == 0) ? 0 : 2;
}