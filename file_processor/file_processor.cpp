#include "file_processor.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <sstream>

#include "deidentifier.hpp"
#include "metadata.hpp"

namespace deid {

namespace {

std::string normalizedExtension(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return extension;
}

std::string escapeShellArg(const std::string& arg) {
    std::string escaped = "\"";
    for (char c : arg) {
        if (c == '"' || c == '\\' || c == '$' || c == '`') {
            escaped += '\\';
        }
        escaped += c;
    }
    escaped += "\"";
    return escaped;
}

fs::path generateUniqueTempPath(const std::string& prefix, const std::string& ext) {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto uniqueId = std::to_string(now) + "_" + std::to_string(++counter);
    return fs::temp_directory_path() / (prefix + "_" + uniqueId + ext);
}

class TextDocumentParser : public IDocumentParser {
public:
    std::string extractText(const fs::path& filePath) override {
        std::ifstream input(filePath, std::ios::binary);
        if (!input) return "";
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

    bool writeText(const fs::path& outputPath, const std::string& content) override {
        std::ofstream output(outputPath, std::ios::binary);
        if (!output) return false;
        output << content;
        return output.good();
    }
};

class BinaryDocumentParser : public IDocumentParser {
public:
    explicit BinaryDocumentParser(std::string ext) : ext_(std::move(ext)) {}

    std::string extractText(const fs::path& filePath) override {
        std::string command;
        const fs::path tempOutput = generateUniqueTempPath("deid_extract", ".txt");

        if (ext_ == ".pdf") {
            command = "pdftotext " + escapeShellArg(filePath.string()) + " " + escapeShellArg(tempOutput.string()) + " 2>/dev/null";
        } else if (ext_ == ".docx" || ext_ == ".doc") {
            command = "pandoc " + escapeShellArg(filePath.string()) + " -t plain -o " + escapeShellArg(tempOutput.string()) + " 2>/dev/null";
        }

        std::string extracted;
        if (!command.empty() && std::system(command.c_str()) == 0 && fs::exists(tempOutput)) {
            TextDocumentParser textParser;
            extracted = textParser.extractText(tempOutput);
            std::error_code ec;
            fs::remove(tempOutput, ec);
            if (!extracted.empty()) return extracted;
        }

        std::error_code ec;
        fs::remove(tempOutput, ec);

        TextDocumentParser fallback;
        return fallback.extractText(filePath);
    }

    bool writeText(const fs::path& outputPath, const std::string& content) override {
        const fs::path tempTxt = generateUniqueTempPath("deid_render", ".txt");
        {
            std::ofstream tempOut(tempTxt, std::ios::binary);
            if (!tempOut) return false;
            tempOut << content;
        }

        bool success = false;
        std::string command = "pandoc " + escapeShellArg(tempTxt.string()) + " -o " + escapeShellArg(outputPath.string()) + " 2>/dev/null";

        if (std::system(command.c_str()) == 0 && fs::exists(outputPath)) {
            success = true;
        } else {
            std::ofstream output(outputPath, std::ios::binary);
            if (output) {
                output << content;
                success = output.good();
            }
        }

        std::error_code ec;
        fs::remove(tempTxt, ec);
        return success;
    }

private:
    std::string ext_;
};

} // namespace

std::unique_ptr<IDocumentParser> createParserForExtension(const std::string& extension) {
    if (extension == ".txt" || extension == ".md" || extension == ".csv") {
        return std::make_unique<TextDocumentParser>();
    } else if (extension == ".pdf" || extension == ".doc" || extension == ".docx") {
        return std::make_unique<BinaryDocumentParser>(extension);
    }
    return nullptr;
}

fs::path getInputFolder(int argc, char* argv[]) {
    if (argc >= 2) {
        return argv[1];
    }

    std::string input;
    std::cout << "Въведете път към папката с документи: ";
    std::getline(std::cin, input);
    return input;
}

fs::path createOutputFolder(const fs::path& inputFolder) {
    const fs::path absoluteInput = fs::absolute(inputFolder).lexically_normal();
    const fs::path parent = absoluteInput.parent_path();
    const std::string baseName = absoluteInput.filename().string() + "_deidentified";

    fs::path outputFolder = parent / baseName;
    for (unsigned int suffix = 2; fs::exists(outputFolder); ++suffix) {
        outputFolder = parent / (baseName + "_" + std::to_string(suffix));
    }

    fs::create_directories(outputFolder);
    return outputFolder;
}

std::vector<fs::path> findSupportedFiles(const fs::path& inputFolder) {
    const std::set<std::string> supportedExtensions = {
        ".txt", ".md", ".csv", ".doc", ".docx", ".pdf"
    };

    std::vector<fs::path> files;
    std::error_code error;
    fs::recursive_directory_iterator iterator(
        inputFolder, fs::directory_options::skip_permission_denied, error
    );

    for (const auto end = fs::recursive_directory_iterator(); iterator != end; iterator.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }

        if (!iterator->is_regular_file(error)) {
            error.clear();
            continue;
        }

        if (supportedExtensions.find(normalizedExtension(iterator->path())) != supportedExtensions.end()) {
            files.push_back(iterator->path());
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

FileResult processFile(
    const fs::path& inputFile,
    const fs::path& inputRoot,
    const fs::path& outputRoot,
    Deidentifier& deidentifier
) {
    const std::string ext = normalizedExtension(inputFile);
    auto parser = createParserForExtension(ext);
    
    if (!parser) {
        return {FileStatus::Skipped, 0, "Пропуснат: неподдържан формат на файла (" + ext + ")"};
    }

    std::string rawContent = parser->extractText(inputFile);
    if (rawContent.empty()) {
        return {FileStatus::Failed, 0, "Грешка: неуспешно четене или празен документ."};
    }

    const std::string cleanedContent = MetadataProcessor::stripMetadata(rawContent, inputFile);

    const auto deidentified = deidentifier.deidentifyText(cleanedContent);

    std::error_code error;
    const fs::path relativePath = fs::relative(inputFile, inputRoot, error);
    if (error) {
        return {FileStatus::Failed, 0, "Грешка: неуспешно изчисляване на изходния път."};
    }

    const fs::path outputFile = outputRoot / relativePath;
    fs::create_directories(outputFile.parent_path(), error);
    if (error) {
        return {FileStatus::Failed, 0, "Грешка: директорията не може да бъде създадена."};
    }

    if (!parser->writeText(outputFile, deidentified.text)) {
        return {FileStatus::Failed, 0, "Грешка: записът на анонимизирания файл беше неуспешен."};
    }

    return {
        FileStatus::Processed, 
        deidentified.redactions,
        "Обработен (" + ext + "): " + std::to_string(deidentified.redactions) + " редакции."
    };
}

void addToSummary(ProcessingSummary& summary, const FileResult& result) {
    summary.redactions += result.redactions;

    switch (result.status) {
    case FileStatus::Processed:
        ++summary.processed;
        break;
    case FileStatus::Skipped:
        ++summary.skipped;
        break;
    case FileStatus::Failed:
        ++summary.failed;
        break;
    }
}

} // namespace deid