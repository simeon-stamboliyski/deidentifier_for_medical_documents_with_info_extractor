#include "extractor.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace extractor {

const std::vector<std::string> FIELDS = {
    "Окончателна диагноза",
    "Придружаващи заболявания",
    "Анамнеза",
    "Проведени изследвания",
    "Минали и придружаващи заболявания",
    "Приемана терапия към момента на хоспитализиране",
    "Фамилна анамнеза",
    "Алергии",
    "Обективно състояние",
    "Неврологичен статус",
    "Психичен статус",
    "Изследвания",
    "Терапия",
    "Ход на заболяването",
    "Консултативни прегледи",
    "К-Т по невропсихология",
    "Краткосрочна слухова памет",
    "Benton visual retention test",
    "Isaacs Set test",
    "Струп тест",
    "Заключение",
    "Настъпили усложнения",
    "Изход от заболяването",
    "Обсъждане",
    "Контролни прегледи",
    "Препоръки и назначения",
    "Препоръки към общо практикуващ лекар (ОПЛ)"
};

namespace {

std::string trim(const std::string& str) {
    const auto start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    const auto end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

struct Utf8Char {
    std::string bytes;
    std::size_t byteOffset;
};

std::vector<Utf8Char> decodeUtf8(const std::string& str) {
    std::vector<Utf8Char> chars;
    std::size_t i = 0;
    while (i < str.length()) {
        unsigned char c = static_cast<unsigned char>(str[i]);
        std::size_t len = 1;
        if ((c & 0x80) == 0) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;

        if (i + len > str.length()) len = str.length() - i;

        chars.push_back({str.substr(i, len), i});
        i += len;
    }
    return chars;
}

bool matchesEpicrisisLetter(const std::string& utf8Bytes, int letterIndex) {
    static const std::pair<std::string, std::string> letters[] = {
        {"Е", "е"}, 
        {"П", "п"}, 
        {"И", "и"},
        {"К", "к"}, 
        {"Р", "р"}, 
        {"И", "и"}, 
        {"З", "з"}, 
        {"А", "а"}  
    };

    if (letterIndex < 0 || letterIndex >= 8) {
        return false;
    }

    return (utf8Bytes == letters[letterIndex].first || utf8Bytes == letters[letterIndex].second);
}

std::vector<std::size_t> findEpicrisisPositions(const std::string& content) {
    std::vector<std::size_t> positions;
    const std::vector<Utf8Char> chars = decodeUtf8(content);
    const std::size_t targetLen = 8;

    for (std::size_t i = 0; i < chars.size(); ++i) {
        std::size_t charIdx = i;
        std::size_t matchCount = 0;
        const std::size_t startByteOffset = chars[i].byteOffset;

        while (charIdx < chars.size() && matchCount < targetLen) {
            const std::string& currBytes = chars[charIdx].bytes;
            const bool isWhitespace = (currBytes.length() == 1 && 
                (currBytes[0] == ' ' || currBytes[0] == '\t' || currBytes[0] == '\r' || currBytes[0] == '\n'));

            if (isWhitespace) {
                charIdx++;
                continue;
            }

            if (matchesEpicrisisLetter(currBytes, static_cast<int>(matchCount))) {
                matchCount++;
                charIdx++;
            } else {
                break;
            }
        }

        if (matchCount == targetLen) {
            positions.push_back(startByteOffset);
        }
    }
    return positions;
}

struct FieldLocation {
    std::string fieldName;
    std::size_t headerStart;
    std::size_t contentStart;
};

std::map<std::string, std::string> parseEpicrisis(const std::string& text) {
    std::map<std::string, std::string> data;
    for (const auto& field : FIELDS) {
        data[field] = "";
    }

    std::vector<FieldLocation> foundFields;

    for (const auto& field : FIELDS) {
        std::regex fieldRegex(field + R"(\s*:\s*)", std::regex::icase);
        std::smatch match;
        if (std::regex_search(text, match, fieldRegex)) {
            foundFields.push_back({
                field,
                static_cast<std::size_t>(match.position()),
                static_cast<std::size_t>(match.position() + match.length())
            });
        }
    }

    if (foundFields.empty()) {
        return data;
    }

    std::sort(foundFields.begin(), foundFields.end(), 
        [](const FieldLocation& a, const FieldLocation& b) {
            return a.headerStart < b.headerStart;
        });

    for (std::size_t i = 0; i < foundFields.size(); ++i) {
        const std::size_t start = foundFields[i].contentStart;
        const std::size_t end = (i + 1 < foundFields.size()) ? foundFields[i + 1].headerStart : text.length();

        if (end >= start) {
            data[foundFields[i].fieldName] = trim(text.substr(start, end - start));
        }
    }

    return data;
}

namespace {

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

std::string readFileContent(const fs::path& filePath, const std::string& ext) {
    if (ext == ".docx" || ext == ".doc") {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        fs::path tempTxt =
            fs::temp_directory_path() /
            fs::path("ext_temp_" + filePath.stem().string() + "_" +
                     std::to_string(now) + ".txt");

        std::string command =
            "textutil -convert txt -encoding utf-8 -output " +
            escapeShellArg(tempTxt.string()) + " " +
            escapeShellArg(filePath.string()) + " 2>/dev/null";

        std::string content;
        if (std::system(command.c_str()) == 0 && fs::exists(tempTxt)) {
            std::ifstream file(tempTxt, std::ios::binary);
            if (file.is_open()) {
                std::stringstream bufferStream;
                bufferStream << file.rdbuf();
                content = bufferStream.str();
            }
        }

        std::error_code ec;
        fs::remove(tempTxt, ec);
        return content; 
    }

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return "";
    std::stringstream bufferStream;
    bufferStream << file.rdbuf();
    return bufferStream.str();
}

} // namespace


std::string escapeCsvField(const std::string& field) {
    std::string escaped = "\"";
    for (char c : field) {
        if (c == '"') {
            escaped += "\"\"";
        } else if (c != '\r' && c != '\n') {
            escaped += c;
        } else {
            escaped += " ";
        }
    }
    escaped += "\"";
    return escaped;
}

} // namespace

void processDocuments(const fs::path& folderPath, const fs::path& outputDir) {
    int epicrisisCount = 0;
    int fileIndex = 1;

    fs::path finalOutputDir = outputDir;
    if (finalOutputDir.empty()) {
        const char* homeDir = std::getenv("HOME");
        finalOutputDir = fs::path(homeDir ? homeDir : ".") / "Desktop" / "epicrisies_info_extract";
    }

    fs::create_directories(finalOutputDir);

    std::ofstream csvFile;

    auto openNewCsv = [&](int index) {
        if (csvFile.is_open()) csvFile.close();

        fs::path outFilePath = finalOutputDir / ("epicrisies_info_" + std::to_string(index) + ".csv");
        csvFile.open(outFilePath, std::ios::out | std::ios::binary);

        unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        csvFile.write(reinterpret_cast<char*>(bom), sizeof(bom));

        for (std::size_t i = 0; i < FIELDS.size(); ++i) {
            csvFile << "\"" << FIELDS[i] << "\"";
            if (i + 1 < FIELDS.size()) csvFile << ",";
        }
        csvFile << "\n";
    };

    openNewCsv(fileIndex);

    for (const auto& entry : fs::directory_iterator(folderPath)) {
        if (!entry.is_regular_file()) continue;

        std::string ext = entry.path().extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (ext != ".txt" && ext != ".md" && ext != ".csv" && ext != ".docx" && ext != ".doc") {
            continue;
        }

        const std::string filename = entry.path().filename().string();
        if (filename.rfind("epicrisies_info_", 0) == 0 || filename.rfind("epicrisies_export_", 0) == 0) {
            continue;
        }

        std::string content = readFileContent(entry.path(), ext);
        if (content.empty()) continue;

        if (content.length() >= 3 &&
            static_cast<unsigned char>(content[0]) == 0xEF &&
            static_cast<unsigned char>(content[1]) == 0xBB &&
            static_cast<unsigned char>(content[2]) == 0xBF) {
            content.erase(0, 3);
        }

        std::vector<std::size_t> positions = findEpicrisisPositions(content);

        for (std::size_t i = 0; i < positions.size(); ++i) {
            std::size_t start = positions[i];
            std::size_t length = (i + 1 < positions.size()) ? (positions[i + 1] - start) : (content.length() - start);
            std::string singleEpicrisis = content.substr(start, length);

            auto parsedData = parseEpicrisis(singleEpicrisis);

            if (parsedData["Окончателна диагноза"].empty()) {
                continue;
            }

            if (epicrisisCount > 0 && epicrisisCount % 100 == 0) {
                fileIndex++;
                openNewCsv(fileIndex);
            }

            for (std::size_t j = 0; j < FIELDS.size(); ++j) {
                csvFile << escapeCsvField(parsedData[FIELDS[j]]);
                if (j + 1 < FIELDS.size()) csvFile << ",";
            }
            csvFile << "\n";
            epicrisisCount++;
        }
    }

    if (csvFile.is_open()) {
        csvFile.close();
    }
}

} // namespace extractor