#include "detectors.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_set>

namespace deid {

namespace {

std::size_t getUtf8SequenceLength(unsigned char leadByte) {
    if ((leadByte & 0x80) == 0) return 1;
    if ((leadByte & 0xE0) == 0xC0) return 2;
    if ((leadByte & 0xF0) == 0xE0) return 3;
    if ((leadByte & 0xF8) == 0xF0) return 4;
    return 1;
}

bool isWordBefore(const std::string& text, std::size_t position) {
    if (position == 0) return false;

    const auto last = static_cast<unsigned char>(text[position - 1]);
    if (last < 128) {
        return std::isalnum(last) != 0 || last == '_';
    }

    std::size_t start = position - 1;
    while (start > 0 &&
           (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
        --start;
    }

    const auto lead = static_cast<unsigned char>(text[start]);
    return (lead == 0xD0 || lead == 0xD1 ||
            lead == 0xD2 || lead == 0xD3);
}

bool isWordAfter(const std::string& text, std::size_t position) {
    if (position >= text.size()) return false;
    const auto lead = static_cast<unsigned char>(text[position]);
    return (lead < 128 && (std::isalnum(lead) != 0 || lead == '_')) ||
           (lead >= 0xD0 && lead <= 0xD3);
}

bool isValidEGN(const std::string& str) {
    if (str.length() != 10) return false;
    for (char c : str) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }

    int month = (str[2] - '0') * 10 + (str[3] - '0');
    int day = (str[4] - '0') * 10 + (str[5] - '0');

    if (month > 40) { month -= 40; }
    else if (month > 20) { month -= 20; }

    if (month < 1 || month > 12 || day < 1 || day > 31) return false;

    static const int weights[9] = {2, 4, 8, 5, 10, 9, 7, 3, 6};
    int sum = 0;
    for (int i = 0; i < 9; ++i) {
        sum += (str[i] - '0') * weights[i];
    }

    int remainder = sum % 11;
    int checksum = (remainder < 10) ? remainder : 0;

    return checksum == (str[9] - '0');
}

bool isValidLNCh(const std::string& str) {
    if (str.length() != 10) return false;
    for (char c : str) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }

    static const int weights[9] = {21, 19, 17, 13, 11, 9, 7, 3, 1};
    int sum = 0;
    for (int i = 0; i < 9; ++i) {
        sum += (str[i] - '0') * weights[i];
    }

    int checksum = sum % 10;
    return checksum == (str[9] - '0');
}

bool isValidUIN(const std::string& str) {
    if (str.length() != 10) return false;
    for (char c : str) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }

    int region = (str[0] - '0') * 10 + (str[1] - '0');
    return (region >= 1 && region <= 28);
}

bool isCyrillicUpper(const std::string& str, std::size_t pos) {
    if (pos + 1 >= str.size()) return false;
    unsigned char c1 = static_cast<unsigned char>(str[pos]);
    unsigned char c2 = static_cast<unsigned char>(str[pos + 1]);

    if (c1 == 0xD0 && (c2 >= 0x90 && c2 <= 0xAF)) return true;
    if (c1 == 0xD0 && c2 == 0x8D) return true; // Ѝ

    return false;
}

bool isCyrillicLower(const std::string& str, std::size_t pos) {
    if (pos + 1 >= str.size()) return false;
    unsigned char c1 = static_cast<unsigned char>(str[pos]);
    unsigned char c2 = static_cast<unsigned char>(str[pos + 1]);

    if (c1 == 0xD0 && (c2 >= 0xB0 && c2 <= 0xBF)) return true;
    if (c1 == 0xD1 && (c2 >= 0x80 && c2 <= 0x8F)) return true;
    if (c1 == 0xD1 && c2 == 0x9D) return true; // ѝ

    return false;
}

bool isCapitalizedWord(const std::string& text, std::size_t start, std::size_t& outLen) {
    if (start >= text.size() || !isCyrillicUpper(text, start)) {
        return false;
    }

    std::size_t i = start + 2;
    while (i + 1 < text.size() && isCyrillicLower(text, i)) {
        i += 2;
    }

    outLen = i - start;
    return outLen >= 4; // At least 2 Cyrillic characters
}

bool isStopWord(const std::string& word) {
    static const std::unordered_set<std::string> stopWords = {
        "От", "На", "По", "За", "При", "До", "След", "Във", "В", "Със", "С", "Из", 
        "Без", "Към", "И", "Или", "През", "Около", "Под", "Над", "Като",

        "Епикриза", "Придружаващи", "Окончателна", "Анамнеза", "Изследвания", 
        "Терапия", "Обсъждане", "Препоръки", "Пациент", "Пациентката", "Диагноза", 
        "Соматичен", "Неврологичен", "Психичен", "Консултативни", "Ход", "Изход", 
        "Контролни", "Описание", "Лекуващ", "Началник", "Обективно", "Статус", 
        "Водеща", "Основна", "Възраст", "Пол", "Адрес", "Отделение", "Клиника",

        "КТ", "ЕМГ", "МРТ", "ЕКГ", "АКР", "ЛЗ", "ЛКК", "УИН", "ЕГН", "ЛНЧ", "ИЗ", 
        "ЗЧЯ", "ВКФ", "КН", "ССС", "ДС", "МПР", "ПИ", "ТРФ", "НПП", "КСП", "СНР", 
        "КП", "ДЗ", "МКБ", "ПКК", "СУЕ", "АЛАТ", "АСАТ", "ГГТ", "Кр", "Урина",

        "No", "№", "бл", "вх", "ап", "ет", "гр", "с", "ул", "бул", "жк", "кв",
        "Сол", "Струп", "Тест", "Табл", "Амп", "Фл"
    };
    return stopWords.find(word) != stopWords.end();
}

} // namespace

EntityDetector::EntityDetector() {
    regexes_ = {
        {EntityType::IDNumber, std::regex(
            R"((?:И\.?З\.?|Из\.?|из\.?|НЗОК|РЗОК|УИН|ЛЗ|ЛКК|РЗИ|Амб\.?[ \t]*(?:лист|№)|Рег\.?[ \t]*(?:№)?)[ \t]*(?:(?:[:.-]|№)[ \t]*)?[A-Za-z]{0,3}[0-9][A-Za-z0-9./-]{0,15})",
            std::regex::icase
        )},
        {EntityType::Location, std::regex(
            R"((?:гр\.|град|с\.|село|ул\.|улица|бул\.|булевард|ж\.?к\.?|кв\.|пл\.|площад)[ \t]+(?:Св\.|(?![ \t]+(?:№|No|бл\.|вх\.|ап\.|ет\.))[^\x80-\xFF\n,;().]|[\xC2-\xDF][\x80-\xBF]|[\xE0-\xEF][\x80-\xBF]{2}|[\xF0-\xF4][\x80-\xBF]{3}){2,80}(?:[ \t]+(?:№|No|бл\.|вх\.|ап\.|ет\.)[ \t]*\d+(?:[A-Za-z]|[\xD0-\xD3][\x80-\xBF])?(?:[ \t]+(?:вх\.|ет\.|ап\.)[ \t]*(?:\d+(?:[A-Za-z]|[\xD0-\xD3][\x80-\xBF])?|[A-Za-z]|[\xD0-\xD3][\x80-\xBF]))*)?)",
            std::regex::icase
        )},
        {EntityType::Phone, std::regex(
            R"((?:\+359[\s./-]?|0)(?:\d{1,3}[\s./-]?)?\d{3}[\s./-]?\d{3,4}\b)"
        )},
        {EntityType::Date, std::regex(
            R"(м\.[ \t]*(?:0?[1-9]|1[0-2])\.[ \t]*(?:\d{4}|\d{2})(?:[ \t]*г(?:\.|одина)?)?|\b\d{1,2}[\./-]\d{1,2}[\./-][ \t]*\d{2,4}\b|\b(?:0?[1-9]|1[0-2])\.(?:\d{4}(?:[ \t]*г\.)?|\d{2}[ \t]*г\.)|\b(?:0?[1-9]|1[0-2])[ \t]*м\.[ \t]*\d{4}(?:[ \t]*г\.?)?|\b(?:\d{1,2}[\./-])?\d{4}\s*г(?:\.|одина)?\b|\b\d{1,2}\s+(?:януари|февруари|март|април|май|юни|юли|август|септември|октомври|ноември|декември)(?:\s+\d{2,4}(?:\s*г\.?)?)?\b)",
            std::regex::icase
        )},
        {EntityType::Email, std::regex(
            R"(([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}))"
        )},
        {EntityType::URLToken, std::regex(
            R"(https?://[^\s/$.?#].[^\s]*|\btoken=[A-Za-z0-9%_-]+\b)"
        )}
    };

    dictionaries_ = {
        {EntityType::Location, {
            "София", "Пловдив", "Варна", "Бургас", "Русе", "Стара Загора", "Плевен", 
            "Сливен", "Добрич", "Шумен", "Перник", "Хасково", "Ямбол", "Пазарджик", 
            "Благоевград", "Велико Търново", "Враца", "Габрово", "Асеновград", "Видин", 
            "Казанлък", "Кърджали", "Кюстендил", "Монтана", "Търговище", "Силистра", 
            "Ловеч", "Разград", "Смолян", "Ботевград", "Сандански", "Самоков"
        }},
        {EntityType::Organization, {
            "МБАЛ", "УМБАЛ", "ВМА", "НЗОК", "РЗОК", "РЗИ", "НАП", "НОИ", "МВР",
            "ДКЦ", "МЦ", "СБАЛ", "СБАЛО", "СБАЛАГ", "КОЦ", "ЦСМП", "ЦРД", "ЛКК",
            "Министерство на здравеопазването", "Пирогов", "Александровска", "Св. Иван Рилски",
            "Св. Анна", "Св. Георги", "Св. Марина", "Токуда", "Софиямед", "Пълмед"
        }}
    };
}

std::vector<DetectedEntity> EntityDetector::detectAll(const std::string& text) const {
    std::vector<DetectedEntity> rawResults;
    detectRegex(text, rawResults);
    detectDictionaries(text, rawResults);
    detectNER(text, rawResults);
    detectEGN(text, rawResults);
    detectLNCh(text, rawResults);
    detectUIN(text, rawResults);

    return rawResults;
}

void EntityDetector::detectRegex(const std::string& text, std::vector<DetectedEntity>& results) const {
    for (const auto& [type, pattern] : regexes_) {
        for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
            std::size_t len = static_cast<std::size_t>(it->length());
            if (len <= (type == EntityType::Location ? 512u : 80u)) {
                const auto pos = static_cast<std::size_t>(it->position());
                if (type == EntityType::Location) {
                    const std::string match = it->str();
                    const bool isCity = match.rfind("гр.", 0) == 0;
                    const bool isVillage = match.rfind("с.", 0) == 0;
                    if (isCity || isVillage) {
                        std::size_t nameStart = pos + (isCity ? std::string("гр.").size()
                                                              : std::string("с.").size());
                        while (nameStart < text.size() &&
                               (text[nameStart] == ' ' || text[nameStart] == '\t')) ++nameStart;
                        if (!isCyrillicUpper(text, nameStart)) continue;
                    }
                }
                if (type == EntityType::Date) {
                    const auto end = pos + len;
                    if ((pos > 0 && (text[pos - 1] == '/' || text[pos - 1] == '.')) ||
                        text.compare(end, 4, "мг") == 0 ||
                        text.compare(end, 2, "mg") == 0) {
                        continue;
                    }
                    if (it->str().find('-') != std::string::npos &&
                        ((end < text.size() && text[end] == '-') ||
                         (pos > 0 && text[pos - 1] == '-') ||
                         (end < text.size() && text.compare(end, 2, "ч") == 0))) {
                        continue;
                    }
                }
                auto atBoundary = [&](std::size_t offset) {
                    return offset == text.size() ||
                        (static_cast<unsigned char>(text[offset]) & 0xC0) != 0x80;
                };
                if ((type == EntityType::Location ||
                     type == EntityType::IDNumber ||
                     type == EntityType::Phone) &&
                    isWordBefore(text, pos)) {
                    continue;
                }
                if (!atBoundary(pos) || !atBoundary(pos + len)) {
                    throw std::runtime_error(
                        "Detector regex split a UTF-8 character; type=" +
                        std::to_string(static_cast<int>(type)) +
                        " byte_start=" + std::to_string(pos) +
                        " byte_length=" + std::to_string(len));
                }
                results.push_back({
                    type,
                    static_cast<std::size_t>(it->position()),
                    len,
                    it->str(),
                    0.95
                });
            }
        }
    }

    static const std::regex sickLeaveNumber(
        R"((?:болничен|Болничен)[ \t]+(?:лист|Лист)[ \t]*№[ \t]*:?[ \t]*((?:E|e|Е|е)[0-9]{8,14}))");
    for (std::sregex_iterator it(text.begin(), text.end(), sickLeaveNumber), end;
         it != end; ++it) {
        const auto& match = *it;
        const auto start = static_cast<std::size_t>(match.position(1));
        const auto value = match[1].str();
        const auto finish = start + value.size();
        if (!isWordBefore(text, start) &&
            !isWordAfter(text, finish)) {
            results.push_back({EntityType::IDNumber, start,
                               value.size(), value, 0.995});
        }
    }

    std::size_t at = 0;
    while ((at = text.find("год. От", at)) != std::string::npos) {
        std::size_t cityStart = at + std::string("год. От").size();
        while (cityStart < text.size() &&
               (text[cityStart] == ' ' || text[cityStart] == '\t'))
            ++cityStart;

        std::size_t cursor = cityStart;
        std::size_t cityEnd = cityStart;
        for (int word = 0; word < 3; ++word) {
            std::size_t length = 0;
            if (!isCapitalizedWord(text, cursor, length)) break;
            cityEnd = cursor + length;
            cursor = cityEnd;
            while (cursor < text.size() &&
                   (text[cursor] == ' ' || text[cursor] == '\t'))
                ++cursor;
            if (cursor < text.size() && text[cursor] == ',') {
                if (cityEnd - cityStart <= 90)
                    results.push_back({EntityType::Location, cityStart,
                                       cityEnd - cityStart,
                                       text.substr(cityStart, cityEnd - cityStart),
                                       0.97});
                break;
            }
            if (cursor == cityEnd) break; 
        }

        std::size_t headerEnd = text.find('\n', cityStart);
        if (headerEnd == std::string::npos || headerEnd > cityStart + 240)
            headerEnd = std::min(text.size(), cityStart + 240);
        const auto admission = text.find("по КП", cityStart);
        if (admission != std::string::npos && admission < headerEnd)
            headerEnd = admission;

        std::size_t no = cityStart;
        while ((no = text.find("No", no)) != std::string::npos && no < headerEnd) {
            std::size_t endNo = no + 2;
            if (isWordBefore(text, no) ||
                (endNo < headerEnd && text[endNo] != ' ' && text[endNo] != '\t')) {
                no += 2;
                continue;
            }
            while (endNo < headerEnd && (text[endNo] == ' ' || text[endNo] == '\t'))
                ++endNo;
            const auto digits = endNo;
            while (endNo < headerEnd &&
                   endNo - digits < 4 && text[endNo] >= '0' && text[endNo] <= '9')
                ++endNo;
            if (endNo > digits) {
                if (endNo < headerEnd &&
                    static_cast<unsigned char>(text[endNo]) >= 0xD0 &&
                    static_cast<unsigned char>(text[endNo]) <= 0xD3 &&
                    endNo + 1 < headerEnd &&
                    (static_cast<unsigned char>(text[endNo + 1]) & 0xC0) == 0x80)
                    endNo += 2;
                else if (endNo < headerEnd &&
                         ((text[endNo] >= 'A' && text[endNo] <= 'Z') ||
                          (text[endNo] >= 'a' && text[endNo] <= 'z')))
                    ++endNo;
                static const std::regex addressTail(
                    R"((?:[ \t]+(?:вх\.?|ет\.?|ап\.?)[ \t]*(?:\d{1,4}(?:[A-Za-z]|[\xD0-\xD3][\x80-\xBF])?|[A-Za-z]|[\xD0-\xD3][\x80-\xBF]))+)"
                );
                std::smatch tail;
                if (std::regex_search(text.cbegin() + endNo,
                                      text.cbegin() + headerEnd, tail, addressTail,
                                      std::regex_constants::match_continuous)) {
                    endNo += tail.length();
                }
                if (!isWordAfter(text, endNo)) {
                    std::size_t streetEnd = no;
                    while (streetEnd > cityStart &&
                           (text[streetEnd - 1] == ' ' ||
                            text[streetEnd - 1] == '\t'))
                        --streetEnd;
                    std::size_t streetStart = streetEnd;
                    while (streetStart > cityStart &&
                           text[streetStart - 1] != ' ' &&
                           text[streetStart - 1] != '\t' &&
                           text[streetStart - 1] != ',' &&
                           text[streetStart - 1] != '.')
                        --streetStart;
                    std::size_t streetLen = 0;
                    if (isCapitalizedWord(text, streetStart, streetLen) &&
                        streetStart + streetLen == streetEnd) {
                        results.push_back({EntityType::Location, streetStart,
                                           streetLen,
                                           text.substr(streetStart, streetLen),
                                           0.96});
                    }
                    results.push_back({EntityType::Location, no, endNo - no,
                                       text.substr(no, endNo - no), 0.97});
                }
            }
            no += 2;
        }
        at = cityStart;
    }

}

void EntityDetector::detectLNCh(const std::string& text, std::vector<DetectedEntity>& results) const {
    static const std::regex lnchCandidateRegex(R"(\b\d{10}\b)");
    for (std::sregex_iterator it(text.begin(), text.end(), lnchCandidateRegex), end; it != end; ++it) {
        std::string candidate = it->str();
        if (isValidLNCh(candidate)) {
            results.push_back({
                EntityType::IDNumber,
                static_cast<std::size_t>(it->position()),
                static_cast<std::size_t>(it->length()),
                candidate,
                0.95 
            });
        }
    }
}

void EntityDetector::detectUIN(const std::string& text, std::vector<DetectedEntity>& results) const {
    static const std::regex uinRegex(R"(\bУИН\s*(?:[:№.]\s*)?(\d{10})\b)", std::regex::icase);
    for (std::sregex_iterator it(text.begin(), text.end(), uinRegex), end; it != end; ++it) {
        std::string candidate = (*it)[1].str();
        if (isValidUIN(candidate)) {
            results.push_back({
                EntityType::IDNumber,
                static_cast<std::size_t>(it->position(1)),
                static_cast<std::size_t>(candidate.length()),
                candidate,
                0.99
            });
        }
    }
}

void EntityDetector::detectDictionaries(const std::string& text, std::vector<DetectedEntity>& results) const {
    for (const auto& [type, terms] : dictionaries_) {
        for (const auto& term : terms) {
            std::size_t pos = text.find(term);
            while (pos != std::string::npos) {
                if (!isWordBefore(text, pos) &&
                    !isWordAfter(text, pos + term.length())) {
                    results.push_back({
                        type,
                        pos,
                        term.length(),
                        term,
                        0.85
                    });
                }
                pos = text.find(term, pos + term.length());
            }
        }
    }
}

void EntityDetector::detectEGN(const std::string& text, std::vector<DetectedEntity>& results) const {
    static const std::regex egnCandidateRegex(R"(\b\d{10}\b)");
    for (std::sregex_iterator it(text.begin(), text.end(), egnCandidateRegex), end; it != end; ++it) {
        std::string candidate = it->str();
        if (isValidEGN(candidate)) {
            results.push_back({
                EntityType::IDNumber,
                static_cast<std::size_t>(it->position()),
                static_cast<std::size_t>(it->length()),
                candidate,
                1.00
            });
        }
    }
}

void EntityDetector::detectNER(const std::string& text, std::vector<DetectedEntity>& results) const {
    static const std::vector<std::string> prefixes = {
        "акад. д-р ", "акад.д-р ", "проф. д-р ", "проф.д-р ", 
        "доц. д-р ", "доц.д-р ", "д-р ", "д-р"
    };

    for (const auto& prefix : prefixes) {
        std::size_t pos = 0;
        while ((pos = text.find(prefix, pos)) != std::string::npos) {
            std::size_t start = pos;
            std::size_t i = pos + prefix.length();

            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;

            while (i < text.size()) {
                unsigned char c = static_cast<unsigned char>(text[i]);
                if (c == '.' || c == ' ' || c == '-') {
                    ++i;
                } else if ((c == 0xD0 || c == 0xD1) && (i + 1 < text.size())) {
                    i += 2;
                } else {
                    break;
                }
            }

            std::size_t matchLen = i - start;
            std::string matchStr = text.substr(start, matchLen);

            while (!matchStr.empty() && (matchStr.back() == '/' || matchStr.back() == ' ' || 
                                         matchStr.back() == '\t' || matchStr.back() == ',')) {
                matchStr.pop_back();
            }

            if (matchStr.length() > prefix.length()) {
                results.push_back({
                    EntityType::Person,
                    start,
                    matchStr.length(),
                    matchStr,
                    0.99
                });
            }

            pos += prefix.length();
        }
    }

    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t w1Len = 0;
        if (isCapitalizedWord(text, i, w1Len)) {
            std::string firstWord = text.substr(i, w1Len);
            if (isStopWord(firstWord)) {
                i += w1Len;
                continue;
            }

            std::size_t j = i + w1Len;
            while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) ++j;

            std::size_t w2Len = 0;
            if (j < text.size() && isCapitalizedWord(text, j, w2Len)) {
                std::string secondWord = text.substr(j, w2Len);
                if (isStopWord(secondWord)) {
                    i = j + w2Len;
                    continue;
                }

                std::size_t k = j + w2Len;
                while (k < text.size() && (text[k] == ' ' || text[k] == '\t')) ++k;

                std::size_t w3Len = 0;
                bool hasW3 = false;
                if (k < text.size() && isCapitalizedWord(text, k, w3Len)) {
                    std::string thirdWord = text.substr(k, w3Len);
                    if (!isStopWord(thirdWord)) {
                        hasW3 = true;
                    }
                }

                std::size_t totalLen = hasW3 ? (k + w3Len - i) : (j + w2Len - i);
                const std::string patientPrefix = "На ";
                const bool afterPatientPrefix = i >= patientPrefix.size() &&
                    text.compare(i - patientPrefix.size(), patientPrefix.size(), patientPrefix) == 0;
                if (totalLen <= 60 && (hasW3 || afterPatientPrefix)) {
                    results.push_back({
                        EntityType::Person,
                        i,
                        totalLen,
                        text.substr(i, totalLen),
                        0.88
                    });
                }

                i += totalLen;
                continue;
            }
        }

        i += getUtf8SequenceLength(static_cast<unsigned char>(text[i]));
    }
}

} // namespace deid