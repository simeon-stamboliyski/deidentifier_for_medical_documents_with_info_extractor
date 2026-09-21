#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <regex>
#include <map>
#include <cctype>
#include <cstdio>

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

std::map<std::string, std::string> parseEpicrisis(const std::string& text) {
    std::map<std::string, std::string> data;
    
    for (size_t i = 0; i < FIELDS.size(); ++i) {
        std::string currentField = FIELDS[i];
        
        std::string pattern = currentField + R"(\s*:\s*)";
        std::regex fieldRegex(pattern);
        
        std::smatch match;
        if (!std::regex_search(text, match, fieldRegex)) {
            data[currentField] = "";
            continue;
        }
        
        size_t startPos = match.position() + match.length();
        size_t endPos = text.length();
        
        for (size_t j = 0; j < FIELDS.size(); ++j) {
            if (i == j) continue;
            std::string nextPattern = FIELDS[j] + R"(\s*:\s*)";
            std::regex nextRegex(nextPattern);
            std::smatch nextMatch;
            
            std::string searchSubstr = text.substr(startPos);
            if (std::regex_search(searchSubstr, nextMatch, nextRegex)) {
                size_t absoluteNextPos = startPos + nextMatch.position();
                if (absoluteNextPos < endPos) {
                    endPos = absoluteNextPos;
                }
            }
        }
        
        std::string value = text.substr(startPos, endPos - startPos);

        value.erase(0, value.find_first_not_of(" \t\n\r"));
        value.erase(value.find_last_not_of(" \t\n\r") + 1);
        data[currentField] = value;
    }
    return data;
}

struct Utf8Char {
    std::string bytes;
    size_t byteOffset;
};

std::vector<Utf8Char> decodeUtf8(const std::string& str) {
    std::vector<Utf8Char> chars;
    size_t i = 0;
    while (i < str.length()) {
        unsigned char c = str[i];
        size_t len = 1;
        if ((c & 0x80) == 0) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;

        if (i + len > str.length()) len = str.length() - i;

        Utf8Char uc;
        uc.bytes = str.substr(i, len);
        uc.byteOffset = i;
        chars.push_back(uc);
        i += len;
    }
    return chars;
}

bool matchesEpicrisisLetter(const std::string& utf8Bytes, int letterIndex) {
    if (utf8Bytes.empty()) return false;
    unsigned char b1 = utf8Bytes[0];
    unsigned char b2 = (utf8Bytes.length() > 1) ? utf8Bytes[1] : 0;

    if (b1 == 0xD0 || b1 == 0xD1) {
        switch (letterIndex) {
            case 0: return (b2 == 0x95 || b2 == 0xB5);
            case 1: return (b2 == 0x9F || b2 == 0xBF);
            case 2: 
            case 5: return (b2 == 0x98 || b2 == 0xB8);
            case 3: return (b2 == 0x9A || b2 == 0xBA);
            case 4: return (b2 == 0xA0 || b2 == 0xB0);
            case 6: return (b2 == 0x97 || b2 == 0xB7);
            case 7: return (b2 == 0x90 || b2 == 0xB0);
            default: break;
        }
    }
    return false;
}

std::vector<size_t> findEpicrisisPositions(const std::string& content) {
    std::vector<size_t> positions;
    std::vector<Utf8Char> chars = decodeUtf8(content);
    size_t targetLen = 8;

    for (size_t i = 0; i < chars.size(); ++i) {
        size_t charIdx = i;
        size_t matchCount = 0;
        size_t startByteOffset = chars[i].byteOffset;

        while (charIdx < chars.size() && matchCount < targetLen) {
            std::string currBytes = chars[charIdx].bytes;
            bool isWhitespace = (currBytes.length() == 1 && 
                (currBytes[0] == ' ' || currBytes[0] == '\t' || currBytes[0] == '\r' || currBytes[0] == '\n'));

            if (isWhitespace) {
                charIdx++;
                continue;
            }

            if (matchesEpicrisisLetter(currBytes, matchCount)) {
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

std::string readFileContent(const fs::path& filePath, const std::string& ext) {
    std::string content = "";
    
    if (ext == ".docx" || ext == ".doc") {
        std::string command = "textutil -convert txt -stdout \"" + filePath.string() + "\" 2>/dev/null";
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) return "";
        
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            content += buffer;
        }
        pclose(pipe);
    } else {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) return "";
        std::stringstream bufferStream;
        bufferStream << file.rdbuf();
        content = bufferStream.str();
    }
    
    return content;
}

void processDocuments(const fs::path& folderPath) {
    int epicrisisCount = 0;
    int fileIndex = 1;
    
    std::string homeDir = std::getenv("HOME") ? std::getenv("HOME") : ".";
    fs::path outputDir = fs::path(homeDir) / "Desktop" / "epicrisies_info_extract";
    
    fs::create_directories(outputDir); 

    std::ofstream csvFile;
    
    auto openNewCsv = [&](int index) {
        if (csvFile.is_open()) csvFile.close();
        
        fs::path outFilePath = outputDir / ("epicrisies_info_" + std::to_string(index) + ".csv");
        csvFile.open(outFilePath, std::ios::out | std::ios::binary);
        
        unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        csvFile.write((char*)bom, sizeof(bom));
        
        for (size_t i = 0; i < FIELDS.size(); ++i) {
            csvFile << "\"" << FIELDS[i] << "\"";
            if (i + 1 < FIELDS.size()) csvFile << ",";
        }
        csvFile << "\n";
    };

    openNewCsv(fileIndex);

    for (const auto& entry : fs::directory_iterator(folderPath)) {
        if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            for (auto& c : ext) c = std::tolower(c);
            
            if (ext != ".txt" && ext != ".md" && ext != ".csv" && ext != ".docx" && ext != ".doc") {
                continue; 
            }
            
            if (entry.path().filename().string().rfind("epicrisies_export_", 0) == 0) {
                continue;
            }

            std::string content = readFileContent(entry.path(), ext);
            if (content.empty()) continue;

            if (content.length() >= 3 && 
                (unsigned char)content[0] == 0xEF && 
                (unsigned char)content[1] == 0xBB && 
                (unsigned char)content[2] == 0xBF) {
                content.erase(0, 3);
            }

            std::vector<size_t> positions = findEpicrisisPositions(content);
            
            for (size_t i = 0; i < positions.size(); ++i) {
                size_t start = positions[i];
                size_t length = (i + 1 < positions.size()) ? (positions[i + 1] - start) : (content.length() - start);
                std::string singleEpicrisis = content.substr(start, length);
                
                auto parsedData = parseEpicrisis(singleEpicrisis);
                
                if (parsedData["Окончателна диагноза"].empty()) {
                    continue; 
                }
                
                if (epicrisisCount > 0 && epicrisisCount % 100 == 0) {
                    fileIndex++;
                    openNewCsv(fileIndex);
                }
                
                for (size_t j = 0; j < FIELDS.size(); ++j) {
                    csvFile << escapeCsvField(parsedData[FIELDS[j]]);
                    if (j + 1 < FIELDS.size()) csvFile << ",";
                }
                csvFile << "\n";
                epicrisisCount++;
            }
        }
    }
    
    if (csvFile.is_open()) csvFile.close();
}

}