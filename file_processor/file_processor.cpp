#include "file_processor.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <process.h>
#endif

#include "deidentifier.hpp"
#include "metadata.hpp"

namespace deid {

namespace {

fs::path pathFromUtf8(const std::string& text) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(text.begin(), text.end()));
#else
    return fs::u8path(text);
#endif
}

#ifdef _WIN32
std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        &result[0], size, nullptr, nullptr);
    return result;
}

std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) return L"";
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) return L"";
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &result[0], size);
    return result;
}
#endif

std::string normalizedExtension(const fs::path& path) {
    std::string extension = pathToUtf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return extension;
}

std::string quoteArg(const std::string& arg) {
#ifdef _WIN32
    return "\"" + arg + "\"";
#else
    std::string quoted = "'";
    for (char c : arg) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += "'";
    return quoted;
#endif
}

std::string quotePath(const fs::path& path) {
    return quoteArg(pathToUtf8(path));
}

int runCommand(const std::string& command) {
#ifdef _WIN32
    return _wsystem(utf8ToWide(command + " >NUL 2>NUL").c_str());
#else
    const std::string full = command + " >/dev/null 2>&1";
    return std::system(full.c_str());
#endif
}

fs::path generateUniqueTempPath(const std::string& prefix, const std::string& ext) {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto uniqueId = std::to_string(now) + "_" + std::to_string(++counter);
    return fs::temp_directory_path() / (prefix + "_" + uniqueId + ext);
}

std::string escapeHtml(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size() + text.size() / 8);
    for (char c : text) {
        switch (c) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        default: escaped += c; break;
        }
    }
    return escaped;
}

std::string textToHtml(const std::string& content) {
    std::string html = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"></head><body>\n";
    std::size_t start = 0;
    while (start <= content.size()) {
        std::size_t end = content.find('\n', start);
        if (end == std::string::npos) end = content.size();
        std::string line = content.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::string paragraph;
        std::size_t pos = 0;
        while (pos < line.size()) {
            const std::size_t sep = line.find("\xE2\x80\xA8", pos);
            const std::size_t stop = (sep == std::string::npos) ? line.size() : sep;
            paragraph += escapeHtml(line.substr(pos, stop - pos));
            if (sep == std::string::npos) break;
            paragraph += "<br/>";
            pos = sep + 3;
        }

        if (paragraph.find_first_not_of(" \t") != std::string::npos) {
            html += "<p>" + paragraph + "</p>\n";
        }
        if (end == content.size()) break;
        start = end + 1;
    }
    html += "</body></html>\n";
    return html;
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
        std::string extracted;

        if (ext_ == ".pdf") {
            extracted = convertToText("pdftotext -enc UTF-8 " + quotePath(filePath), filePath, "", true);
            if (extracted.empty()) reportMissingTool("pdftotext (Poppler)");
            return extracted;
        }

#ifdef __APPLE__
        if (ext_ == ".docx" || ext_ == ".doc") {
            extracted = convertToText("textutil -convert txt -encoding utf-8", filePath, "-output", false);
        }
#endif
        if (extracted.empty() && ext_ != ".doc") {
            extracted = convertToText("pandoc -t plain --wrap=none", filePath, "-o", false);
        }
        if (extracted.empty()) {
            extracted = convertWithLibreOffice(filePath);
        }
        if (extracted.empty()) {
#ifdef __APPLE__
            reportMissingTool(ext_ == ".odt" ? "pandoc" : "textutil");
#else
            reportMissingTool(ext_ == ".doc" ? "LibreOffice (soffice)" : "pandoc");
#endif
        }
        return extracted;
    }

    bool writeText(const fs::path& outputPath, const std::string& content) override {
        bool success = false;

        if (ext_ == ".docx" || ext_ == ".doc" || ext_ == ".odt") {
            const fs::path tempHtml = generateUniqueTempPath("deid_render", ".html");
            {
                std::ofstream tempOut(tempHtml, std::ios::binary);
                if (!tempOut) {
                    std::cerr << "writeText: failed to open temp file for writing: "
                              << pathToUtf8(tempHtml) << "\n";
                    return false;
                }
                tempOut << textToHtml(content);
                tempOut.flush();
            }

#ifdef DEID_DEBUG_DUMPS
            {
                fs::path dbgTemp = outputPath.parent_path() / "debug_temp_for_pandoc.html";
                std::error_code ec;
                fs::copy_file(tempHtml, dbgTemp,
                            fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    std::cerr << "writeText: failed to copy temp file to " << pathToUtf8(dbgTemp)
                            << ": " << ec.message() << "\n";
                }
            }
#endif

            std::error_code sizeError;
            if (!fs::exists(tempHtml) || fs::file_size(tempHtml, sizeError) == 0) {
                std::cerr << "writeText: temp file missing or empty: " << pathToUtf8(tempHtml) << "\n";
                std::error_code ec;
                fs::remove(tempHtml, ec);
                return false;
            }

            fs::path pandocOut = outputPath;
            if (ext_ == ".doc") {
                pandocOut.replace_extension(".docx");
            }

            const std::string targetFormat =
                (ext_ == ".odt") ? "odt" : "docx";

            const std::string pandocCmd =
                "pandoc " + quotePath(tempHtml) +
                " -f html -t " + targetFormat + " -o " + quotePath(pandocOut);

            const int rc = runCommand(pandocCmd);
            if (rc != 0) {
                std::cerr << "writeText: pandoc exited with code " << rc
                          << " (проверете дали pandoc е инсталиран)\n";
            }

            std::error_code ec;
            if (fs::exists(pandocOut, ec)) {
                const auto sz = fs::file_size(pandocOut, ec);
                if (rc == 0 && sz > 0) {
                    success = true;

                    if (ext_ == ".doc" && pandocOut != outputPath) {
                        fs::rename(pandocOut, outputPath, ec);
                        if (ec) {
                            std::cerr << "writeText: failed to rename " << pathToUtf8(pandocOut)
                                    << " to " << pathToUtf8(outputPath) << ": " << ec.message() << "\n";
                            success = false;
                        }
                    }
                } else if (sz == 0) {
                    std::cerr << "writeText: output is zero-length file: " << pathToUtf8(pandocOut) << "\n";
                }
            } else {
                std::cerr << "writeText: output was not created: " << pathToUtf8(pandocOut) << "\n";
            }

            fs::remove(tempHtml, ec);
        } else if (ext_ == ".pdf") {
            fs::path txtOutput = outputPath;
            txtOutput.replace_extension(".txt");

            std::ofstream output(txtOutput, std::ios::binary);
            if (!output) {
                std::cerr << "writeText: failed to open txtOutput for PDF: " << pathToUtf8(txtOutput) << "\n";
            } else {
                output << content;
                success = output.good();
                if (!success) {
                    std::cerr << "writeText: writing to txtOutput failed: " << pathToUtf8(txtOutput) << "\n";
                }
            }
        } else {
            std::ofstream output(outputPath, std::ios::binary);
            if (!output) {
                std::cerr << "writeText: failed to open outputPath: " << pathToUtf8(outputPath) << "\n";
            } else {
                output << content;
                success = output.good();
                if (!success) {
                    std::cerr << "writeText: writing to outputPath failed: " << pathToUtf8(outputPath) << "\n";
                }
            }
        }

        if (!success) {
            std::cerr << "writeText: overall failed for " << pathToUtf8(outputPath) << " (ext_=" << ext_ << ")\n";
        }

        return success;
    }

private:
    std::string convertToText(const std::string& commandPrefix, const fs::path& filePath,
                              const std::string& outputOption, bool outputLast) {
        const fs::path tempOutput = generateUniqueTempPath("deid_extract", ".txt");
        std::string command;
        if (outputLast) {
            command = commandPrefix + " " + quotePath(tempOutput);
        } else {
            command = commandPrefix + " " + outputOption + " " + quotePath(tempOutput) + " " + quotePath(filePath);
        }

        std::string extracted;
        std::error_code ec;
        if (runCommand(command) == 0 && fs::exists(tempOutput, ec)) {
            TextDocumentParser textParser;
            extracted = textParser.extractText(tempOutput);
        }
        fs::remove(tempOutput, ec);
        return extracted;
    }

    std::string convertWithLibreOffice(const fs::path& filePath) {
        const fs::path tempDir = generateUniqueTempPath("deid_extract_dir", "");
        std::error_code ec;
        fs::create_directories(tempDir, ec);
        if (ec) return "";

        const std::string command =
            "soffice --headless --convert-to \"txt:Text (encoded):UTF8\" --outdir " +
            quotePath(tempDir) + " " + quotePath(filePath);

        std::string extracted;
        if (runCommand(command) == 0) {
            for (const auto& entry : fs::directory_iterator(tempDir, ec)) {
                if (entry.path().extension() == ".txt") {
                    TextDocumentParser textParser;
                    extracted = textParser.extractText(entry.path());
                    break;
                }
            }
        }
        fs::remove_all(tempDir, ec);

        if (extracted.size() >= 3 &&
            static_cast<unsigned char>(extracted[0]) == 0xEF &&
            static_cast<unsigned char>(extracted[1]) == 0xBB &&
            static_cast<unsigned char>(extracted[2]) == 0xBF) {
            extracted.erase(0, 3);
        }
        return extracted;
    }

    void reportMissingTool(const std::string& tool) const {
        std::cerr << "  Неуспешно четене на " << ext_ << " файл. Нужен е " << tool << ".\n";
    }

    std::string ext_;
};

} // namespace

std::string pathToUtf8(const fs::path& path) {
#if defined(__cpp_char8_t)
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
#else
    return path.u8string();
#endif
}

void setupConsole() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

std::unique_ptr<IDocumentParser> createParserForExtension(const std::string& extension) {
    if (extension == ".txt" || extension == ".md" || extension == ".csv") {
        return std::make_unique<TextDocumentParser>();
    } else if (extension == ".pdf" || extension == ".doc" || extension == ".docx" ||
               extension == ".odt") {
        return std::make_unique<BinaryDocumentParser>(extension);
    }
    return nullptr;
}

fs::path getInputFolder(int argc, char* argv[]) {
#ifdef _WIN32
    (void)argv;
    if (argc >= 2) {
        int wideCount = 0;
        LPWSTR* wideArgs = CommandLineToArgvW(GetCommandLineW(), &wideCount);
        if (wideArgs != nullptr) {
            fs::path result;
            if (wideCount >= 2) result = fs::path(wideArgs[1]);
            LocalFree(wideArgs);
            if (!result.empty()) return result;
        }
    }
#else
    if (argc >= 2) {
        return argv[1];
    }
#endif

    std::cout << "Въведете път към папката с документи: " << std::flush;

    std::string input;
#ifdef _WIN32
    HANDLE inputHandle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD consoleMode = 0;
    if (inputHandle != INVALID_HANDLE_VALUE && GetConsoleMode(inputHandle, &consoleMode)) {
        std::wstring buffer(32768, L'\0');
        DWORD charsRead = 0;
        if (ReadConsoleW(inputHandle, &buffer[0], static_cast<DWORD>(buffer.size()), &charsRead, nullptr)) {
            buffer.resize(charsRead);
            input = wideToUtf8(buffer);
        }
    } else {
        std::getline(std::cin, input);
    }
#else
    std::getline(std::cin, input);
#endif

    while (!input.empty() && (input.back() == '\n' || input.back() == '\r' ||
                              input.back() == ' ' || input.back() == '\t')) {
        input.pop_back();
    }
    const std::size_t first = input.find_first_not_of(" \t");
    input = (first == std::string::npos) ? "" : input.substr(first);
    if (input.size() >= 2 &&
        ((input.front() == '"' && input.back() == '"') || (input.front() == '\'' && input.back() == '\''))) {
        input = input.substr(1, input.size() - 2);
    }
#ifndef _WIN32
    std::string unescaped;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' && i + 1 < input.size()) {
            unescaped += input[++i];
        } else {
            unescaped += input[i];
        }
    }
    input = unescaped;
#endif

    return pathFromUtf8(input);
}

fs::path createOutputFolder(const fs::path& inputFolder) {
    const fs::path absoluteInput = fs::absolute(inputFolder).lexically_normal();
    const fs::path parent = absoluteInput.parent_path();
    fs::path folderName = absoluteInput.filename();
    if (folderName.empty()) folderName = absoluteInput.parent_path().filename();
    const std::string baseName = pathToUtf8(folderName) + "_deidentified";

    fs::path outputFolder = parent / pathFromUtf8(baseName);
    for (unsigned int suffix = 2; fs::exists(outputFolder); ++suffix) {
        outputFolder = parent / pathFromUtf8(baseName + "_" + std::to_string(suffix));
    }

    fs::create_directories(outputFolder);
    return outputFolder;
}

std::vector<fs::path> findSupportedFiles(const fs::path& inputFolder) {
    const std::set<std::string> supportedExtensions = {
            ".txt", ".md", ".csv", ".doc",
            ".docx", ".pdf", ".odt"
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

        const std::string name = pathToUtf8(iterator->path().filename());
        if (name.rfind("~$", 0) == 0 || name.rfind(".", 0) == 0) {
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

#ifdef DEID_DEBUG_DUMPS
    {
        fs::path dbg1 = outputRoot / "debug_raw.txt";
        std::ofstream f(dbg1, std::ios::binary);
        f << rawContent;
    }
#endif

    const std::string cleanedContent =
        MetadataProcessor::stripMetadata(rawContent, inputFile);

#ifdef DEID_DEBUG_DUMPS
    {
        fs::path dbg2 = outputRoot / "debug_cleaned.txt";
        std::ofstream f(dbg2, std::ios::binary);
        f << cleanedContent;
    }
#endif

    DeidentificationResult deidentified;
    try {
        deidentified = deidentifier.deidentifyText(cleanedContent);
    } catch (const std::exception& ex) {
        // Fail closed: do not write a partially anonymized document.
        return {FileStatus::Failed, 0,
                std::string("Грешка при анонимизацията: ") + ex.what()};
    }

#ifdef DEID_DEBUG_DUMPS
    {
        fs::path dbg3 = outputRoot / "debug_deid.txt";
        std::ofstream f(dbg3, std::ios::binary);
        f << deidentified.text;
    }
#endif

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