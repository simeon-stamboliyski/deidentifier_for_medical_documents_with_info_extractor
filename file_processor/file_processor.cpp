#include "file_processor.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
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
        const fs::path tempOutput = generateUniqueTempPath("deid_extract", ".txt");
        std::string command;

        if (ext_ == ".pdf") {
            command = "pdftotext " + escapeShellArg(filePath.string()) + " "
                    + escapeShellArg(tempOutput.string()) + " 2>/dev/null";
        } else if (ext_ == ".docx" || ext_ == ".doc") {
            command = "textutil -convert txt -encoding utf-8 -output "
                    + escapeShellArg(tempOutput.string()) + " "
                    + escapeShellArg(filePath.string()) + " 2>/dev/null";
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
        bool success = false;

        if (ext_ == ".docx" || ext_ == ".doc") {
            const fs::path tempTxt = generateUniqueTempPath("deid_render", ".txt");
            {
                std::ofstream tempOut(tempTxt, std::ios::binary);
                if (!tempOut) {
                    std::cerr << "writeText: failed to open tempTxt for writing: " << tempTxt << "\n";
                    return false;
                }
                tempOut << content;
                tempOut.flush();
            }

            {
                fs::path dbgTemp = outputPath.parent_path() / "debug_temp_for_pandoc.txt";
                std::error_code ec;
                fs::copy_file(tempTxt, dbgTemp,
                            fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    std::cerr << "writeText: failed to copy tempTxt to " << dbgTemp
                            << ": " << ec.message() << "\n";
                }
            }

            if (!fs::exists(tempTxt) || fs::file_size(tempTxt) == 0) {
                std::cerr << "writeText: tempTxt missing or empty: " << tempTxt << "\n";
                std::error_code ec;
                fs::remove(tempTxt, ec);
                return false;
            }

            fs::path pandocOut = outputPath;
            if (ext_ == ".doc") {
                pandocOut.replace_extension(".docx");
            }

            std::string pandocCmd =
                "pandoc " + escapeShellArg(tempTxt.string()) +
                " -f markdown_strict-smart -t docx -o " +
                escapeShellArg(pandocOut.string()) +
                " 2>/dev/null";

            std::cerr << "writeText: running pandoc cmd: " << pandocCmd << "\n";

            int rc = std::system(pandocCmd.c_str());
            if (rc != 0) {
                std::cerr << "writeText: pandoc exited with code " << rc << "\n";
            }

            if (fs::exists(pandocOut)) {
                auto sz = fs::file_size(pandocOut);
                std::cerr << "writeText: pandocOut exists, size = " << sz << "\n";
                if (rc == 0 && sz > 0) {
                    success = true;

                    if (ext_ == ".doc" && pandocOut != outputPath) {
                        std::error_code ec;
                        fs::rename(pandocOut, outputPath, ec);
                        if (ec) {
                            std::cerr << "writeText: failed to rename " << pandocOut
                                    << " to " << outputPath << ": " << ec.message() << "\n";
                            success = false;
                        }
                    }
                } else if (sz == 0) {
                    std::cerr << "writeText: pandocOut is zero-length file: " << pandocOut << "\n";
                }
            } else {
                std::cerr << "writeText: pandocOut was not created: " << pandocOut << "\n";
            }

            std::error_code ec;
            fs::remove(tempTxt, ec);
            if (ec) {
                std::cerr << "writeText: failed to remove tempTxt: " << tempTxt
                        << ", ec = " << ec.message() << "\n";
            }
        } else if (ext_ == ".pdf") {
            fs::path txtOutput = outputPath;
            txtOutput.replace_extension(".txt");

            std::ofstream output(txtOutput, std::ios::binary);
            if (!output) {
                std::cerr << "writeText: failed to open txtOutput for PDF: " << txtOutput << "\n";
            } else {
                output << content;
                success = output.good();
                if (!success) {
                    std::cerr << "writeText: writing to txtOutput failed: " << txtOutput << "\n";
                }
            }
        } else {
            std::ofstream output(outputPath, std::ios::binary);
            if (!output) {
                std::cerr << "writeText: failed to open outputPath: " << outputPath << "\n";
            } else {
                output << content;
                success = output.good();
                if (!success) {
                    std::cerr << "writeText: writing to outputPath failed: " << outputPath << "\n";
                }
            }
        }

        if (!success) {
            std::cerr << "writeText: overall failed for " << outputPath << " (ext_=" << ext_ << ")\n";
        }

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
        return {FileStatus::Skipped, 0,
                "Пропуснат: неподдържан формат на файла (" + ext + ")"};
    }

    std::string rawContent = parser->extractText(inputFile);
    if (rawContent.empty()) {
        return {FileStatus::Failed, 0,
                "Грешка: неуспешно четене или празен документ."};
    }

    {
        fs::path dbg1 = outputRoot / "debug_raw.txt";
        std::ofstream f(dbg1, std::ios::binary);
        f << rawContent;
    }

    const std::string cleanedContent =
        MetadataProcessor::stripMetadata(rawContent, inputFile);

    {
        fs::path dbg2 = outputRoot / "debug_cleaned.txt";
        std::ofstream f(dbg2, std::ios::binary);
        f << cleanedContent;
    }

    const auto deidentified = deidentifier.deidentifyText(cleanedContent);

    {
        fs::path dbg3 = outputRoot / "debug_deid.txt";
        std::ofstream f(dbg3, std::ios::binary);
        f << deidentified.text;
    }

    std::error_code error;
    const fs::path relativePath = fs::relative(inputFile, inputRoot, error);
    if (error) {
        return {FileStatus::Failed, 0,
                "Грешка: неуспешно изчисляване на изходния път."};
    }

    fs::path outputFile = outputRoot / relativePath;
    fs::create_directories(outputFile.parent_path(), error);
    if (error) {
        return {FileStatus::Failed, 0,
                "Грешка: директорията не може да бъде създадена."};
    }

    if (!parser->writeText(outputFile, deidentified.text)) {
        return {FileStatus::Failed, 0,
                "Грешка: записът на анонимизирания файл беше неуспешен."};
    }

    return {
        FileStatus::Processed,
        deidentified.redactions,
        "Обработен (" + ext + "): " +
            std::to_string(deidentified.redactions) + " редакции."
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