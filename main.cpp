#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "deidentifier/deidentifier.hpp"
#include "extractor/extractor.hpp"
#include "file_processor/file_processor.hpp"

namespace fs = std::filesystem;

namespace {

std::string formatDuration(std::chrono::steady_clock::duration duration) {
    const long long totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    const long long hours = totalMs / 3600000;
    const long long minutes = (totalMs / 60000) % 60;
    const double seconds = static_cast<double>(totalMs % 60000) / 1000.0;

    std::ostringstream out;
    if (hours > 0) out << hours << " ч ";
    if (hours > 0 || minutes > 0) out << minutes << " мин ";
    out << std::fixed << std::setprecision(1) << seconds << " с";
    return out.str();
}

}

int main(int argc, char* argv[]) {
    deid::setupConsole();
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    const fs::path inputFolder = deid::getInputFolder(argc, argv);

    const auto startTime = std::chrono::steady_clock::now();

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
    std::cout << "Изходна папка за анонимизирани файлове: " << deid::pathToUtf8(deidentifiedOutputFolder) << "\n\n";

    deid::Deidentifier deidentifier;
    deid::ProcessingSummary summary = {};

    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto& file = files[index];
        std::cout << "[" << (index + 1) << "/" << files.size() << "] " << deid::pathToUtf8(file.filename()) << "\n";

        const auto result = deid::processFile(file, inputFolder, deidentifiedOutputFolder, deidentifier);
        deid::addToSummary(summary, result);

        std::cout << "  " << result.message << "\n";
    }

    const auto deidentificationEnd = std::chrono::steady_clock::now();

    std::cout << "\n--- Резюме на анонимизацията ---\n";
    std::cout << "Успешно обработени: " << summary.processed << "\n";
    std::cout << "Пропуснати:        " << summary.skipped << "\n";
    std::cout << "Неуспешни:         " << summary.failed << "\n";
    std::cout << "Общ брой редакции: " << summary.redactions << "\n";
    std::cout << "Време:             " << formatDuration(deidentificationEnd - startTime) << "\n";
    std::cout << "--------------------------------\n\n";

    if (summary.processed > 0) {
        std::cout << "Стартиране на извличането на информация от епикризите...\n";
        const fs::path extractionOutputDir = deidentifiedOutputFolder / "extracted_data";
        extractor::processDocuments(deidentifiedOutputFolder, extractionOutputDir);
        std::cout << "Извличането приключи за " << formatDuration(std::chrono::steady_clock::now() - deidentificationEnd)
                  << ". Резултатите са записани в: " << deid::pathToUtf8(extractionOutputDir) << "\n";
    }

    std::cout << "\nОбщо време за цялата работа: " << formatDuration(std::chrono::steady_clock::now() - startTime) << "\n";

    return (summary.failed == 0) ? 0 : 2;
}