#include "detectors.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace deid {

namespace {

constexpr bool kMaskStandaloneYears = false;

constexpr std::size_t npos = std::string::npos;

#define CYR_UP R"((?:\xD0[\x80-\xAF]))"
#define CYR_LO R"((?:\xD0[\xB0-\xBF]|\xD1[\x80-\x9F]))"

std::size_t getUtf8SequenceLength(unsigned char leadByte) {
    if ((leadByte & 0x80) == 0) return 1;
    if ((leadByte & 0xE0) == 0xC0) return 2;
    if ((leadByte & 0xF0) == 0xE0) return 3;
    if ((leadByte & 0xF8) == 0xF0) return 4;
    return 1;
}

char32_t decodeAt(const std::string& s, std::size_t pos, std::size_t* len) {
    if (pos >= s.size()) {
        if (len) *len = 0;
        return 0;
    }
    const auto b0 = static_cast<unsigned char>(s[pos]);
    std::size_t n = getUtf8SequenceLength(b0);
    if (pos + n > s.size()) n = 1;
    char32_t cp = 0;
    switch (n) {
        case 1: cp = b0; break;
        case 2: cp = static_cast<char32_t>(((b0 & 0x1F) << 6) | (s[pos + 1] & 0x3F)); break;
        case 3: cp = static_cast<char32_t>(((b0 & 0x0F) << 12) | ((s[pos + 1] & 0x3F) << 6) |
                                           (s[pos + 2] & 0x3F)); break;
        default: cp = static_cast<char32_t>(((b0 & 0x07) << 18) | ((s[pos + 1] & 0x3F) << 12) |
                                            ((s[pos + 2] & 0x3F) << 6) | (s[pos + 3] & 0x3F)); break;
    }
    if (len) *len = n;
    return cp;
}

char32_t prevCodepoint(const std::string& s, std::size_t pos) {
    if (pos == 0) return 0;
    std::size_t p = pos - 1;
    while (p > 0 && (static_cast<unsigned char>(s[p]) & 0xC0) == 0x80) --p;
    return decodeAt(s, p, nullptr);
}

bool isCyrUpper(char32_t c) { return c >= 0x400 && c <= 0x42F; }
bool isCyrLower(char32_t c) { return c >= 0x430 && c <= 0x45F; }
bool isUpperLetter(char32_t c) { return (c >= 'A' && c <= 'Z') || isCyrUpper(c); }
bool isLowerLetter(char32_t c) { return (c >= 'a' && c <= 'z') || isCyrLower(c); }
bool isLetter(char32_t c) { return isUpperLetter(c) || isLowerLetter(c); }
bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }

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

std::string lowerUtf8(const std::string& s) {
    std::string out = s;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto c = static_cast<unsigned char>(out[i]);
        if (c >= 'A' && c <= 'Z') {
            out[i] = static_cast<char>(c + 32);
        } else if (c == 0xD0 && i + 1 < out.size()) {
            const auto d = static_cast<unsigned char>(out[i + 1]);
            if (d >= 0x80 && d <= 0x8F) {
                out[i] = static_cast<char>(0xD1);
                out[i + 1] = static_cast<char>(d + 0x10);
            } else if (d >= 0x90 && d <= 0x9F) {
                out[i + 1] = static_cast<char>(d + 0x20);
            } else if (d >= 0xA0 && d <= 0xAF) {
                out[i] = static_cast<char>(0xD1);
                out[i + 1] = static_cast<char>(d - 0x20);
            }
            ++i;
        } else if (c == 0xD1) {
            ++i;
        }
    }
    return out;
}

std::string upperUtf8(const std::string& s) {
    std::string out = s;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto c = static_cast<unsigned char>(out[i]);
        if (c >= 'a' && c <= 'z') {
            out[i] = static_cast<char>(c - 32);
        } else if (c == 0xD0 && i + 1 < out.size()) {
            const auto d = static_cast<unsigned char>(out[i + 1]);
            if (d >= 0xB0 && d <= 0xBF) out[i + 1] = static_cast<char>(d - 0x20);
            ++i;
        } else if (c == 0xD1 && i + 1 < out.size()) {
            const auto d = static_cast<unsigned char>(out[i + 1]);
            if (d >= 0x80 && d <= 0x8F) {
                out[i] = static_cast<char>(0xD0);
                out[i + 1] = static_cast<char>(d + 0x20);
            } else if (d >= 0x90 && d <= 0x9F) {
                out[i] = static_cast<char>(0xD0);
                out[i + 1] = static_cast<char>(d - 0x10);
            }
            ++i;
        }
    }
    return out;
}

bool isSpaceAt(const std::string& t, std::size_t pos, std::size_t* len) {
    if (pos >= t.size()) return false;
    if (t[pos] == ' ' || t[pos] == '\t') { *len = 1; return true; }
    if (static_cast<unsigned char>(t[pos]) == 0xC2 && pos + 1 < t.size() &&
        static_cast<unsigned char>(t[pos + 1]) == 0xA0) { *len = 2; return true; }
    return false;
}

std::size_t skipSpaces(const std::string& t, std::size_t pos) {
    std::size_t l = 0;
    while (isSpaceAt(t, pos, &l)) pos += l;
    return pos;
}

std::size_t skipSpacesBack(const std::string& t, std::size_t pos) {
    while (pos > 0 && (t[pos - 1] == ' ' || t[pos - 1] == '\t')) --pos;
    return pos;
}

bool startsWith(const std::string& s, std::size_t pos, const std::string& prefix) {
    return s.compare(pos, prefix.size(), prefix) == 0;
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

std::size_t nameWordLength(const std::string& t, std::size_t pos, bool allowAllCaps) {
    std::size_t len = 0;
    const char32_t first = decodeAt(t, pos, &len);
    if (!isCyrUpper(first)) return 0;
    std::size_t i = pos + len;
    std::size_t letters = 1;
    bool sawLower = false;
    bool sawUpper = false;
    while (i < t.size()) {
        std::size_t l = 0;
        const char32_t c = decodeAt(t, i, &l);
        if (isCyrLower(c) && !(allowAllCaps && sawUpper && !sawLower)) {
            sawLower = true; ++letters; i += l; continue;
        }
        if (allowAllCaps && !sawLower && isCyrUpper(c)) {
            sawUpper = true; ++letters; i += l; continue;
        }
        if ((c == '-' || c == 0x2010 || c == 0x2011) && letters >= 2) {
            std::size_t l2 = 0;
            const char32_t next = decodeAt(t, i + l, &l2);
            if (isCyrUpper(next)) { i += l + l2; ++letters; continue; }
        }
        break;
    }
    if (letters < 2) return 0;
    if (!sawLower && !(allowAllCaps && sawUpper)) return 0;
    if (isLetter(decodeAt(t, i, nullptr))) return 0;
    return i - pos;
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
        "Сол", "Струп", "Тест", "Табл", "Амп", "Фл",

        "Консултант", "Консултация", "Консултиран", "Консултирана", "Психолог",
        "Психиатър", "Невролог", "Лекар", "Доктор", "Пациентът", "Болният", "Болната",
        "Съпруга", "Съпругата", "Съпруг", "Съпругът", "Дъщеря", "Дъщерята", "Син",
        "Синът", "Майка", "Майката", "Баща", "Бащата", "Брат", "Братът", "Сестра",
        "Сестрата", "Внучка", "Внучката", "Внук", "Внукът", "Снаха", "Снахата",
        "Проведен", "Проведена", "Проведено", "Проведени", "Направен", "Направена",
        "Назначен", "Назначена", "Изписан", "Изписана", "Приет", "Приета", "Постъпва",
        "Хоспитализиран", "Хоспитализирана", "Лекуван", "Лекувана", "Насочен", "Насочена"
    };
    return stopWords.find(word) != stopWords.end();
}

bool isEponym(const std::string& lowerWord) {
    static const std::unordered_set<std::string> eponyms = {
        "бабински", "брудзински", "жуковски", "вишневски", "бехтерев", "бехтерева",
        "корсаков", "корсакова", "павлов", "павлова", "сеченов", "пирогов", "пирогова",
        "мечников", "кожевников", "кожевникова", "ромберг", "ласег", "хофман", "трьомнер",
        "маринеско", "кернинг", "кърниг", "опенхайм", "россолимо", "гордон", "шефер",
        "чадок", "пусеп", "мендел", "вартенберг", "нери", "мингацини", "бинда",
        "ахилов", "ахилова", "ахилово", "ахилеса"
    };
    return eponyms.find(lowerWord) != eponyms.end();
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool looksLikeSurname(const std::string& word, bool isLastWord) {
    const std::string w = lowerUtf8(word);
    std::size_t start = 0;
    while (true) {
        std::size_t dash = w.find('-', start);
        const std::string part = w.substr(start, dash == npos ? npos : dash - start);
        if (!isEponym(part)) {
            for (const char* s : {"ов", "ев", "ова", "ева", "ович", "евич", "ич"}) {
                if (endsWith(part, s) && part.size() > std::string(s).size() + 2) return true;
            }
            if (isLastWord) {
                for (const char* s : {"ски", "ска", "цки", "цка"}) {
                    if (endsWith(part, s) && part.size() > std::string(s).size() + 2) return true;
                }
            }
        }
        if (dash == npos) break;
        start = dash + 1;
    }
    return false;
}

struct HeaderSpans {
    std::size_t nameStart = npos, nameEnd = npos;
    std::size_t townStart = npos, townEnd = npos;
    std::size_t streetStart = npos, streetEnd = npos;
};

bool looksLikeIdSegment(const std::string& lowered, std::size_t s, std::size_t e) {
    const std::string seg = lowered.substr(s, e - s);
    if (startsWith(seg, 0, "[id_") || startsWith(seg, 0, "№") || startsWith(seg, 0, "no")) return true;
    for (const char* p : {"из", "и.з", "и. з"}) {
        if (startsWith(seg, 0, p)) {
            const std::size_t after = std::string(p).size();
            if (after >= seg.size() || !isLetter(decodeAt(seg, after, nullptr))) return true;
        }
    }
    static const std::regex yearRef(R"(\d+[ \t]*/[ \t]*(?:19|20)\d{2})");
    return std::regex_search(seg, yearRef);
}

bool hasStreetMarker(const std::string& lowered, std::size_t s, std::size_t e) {
    const std::string seg = lowered.substr(s, e - s);
    for (const char* m : {"ул.", "бул.", "ж.к", "жк", "кв.", "бл.", "вх.", "ап.", "no", "№"}) {
        if (seg.find(m) != npos) return true;
    }
    return false;
}

std::vector<HeaderSpans> findHeaders(const std::string& text, const std::string& lowered) {
    static const std::regex ageFrom(
        R"(,[ \t]*\d{1,3}[ \t]*(?:год(?:ини|\.)?|г\.)[ \t]*от[ \t]+)");
    std::vector<HeaderSpans> out;
    for (std::sregex_iterator it(lowered.begin(), lowered.end(), ageFrom), end; it != end; ++it) {
        HeaderSpans h;
        const auto comma = static_cast<std::size_t>(it->position());
        const std::size_t addrStart = comma + static_cast<std::size_t>(it->length());
        std::size_t lineStart = (comma == 0) ? npos : lowered.rfind('\n', comma - 1);
        lineStart = (lineStart == npos) ? 0 : lineStart + 1;

        std::size_t na = npos;
        std::size_t from = comma;
        while (from > lineStart) {
            const std::size_t p = lowered.rfind("на ", from - 1);
            if (p == npos || p < lineStart) break;
            if (!isWordBefore(text, p)) { na = p; break; }
            from = p;
        }
        if (na != npos) {
            const std::size_t s = skipSpaces(text, na + std::string("на ").size());
            const std::size_t e = skipSpacesBack(text, comma);
            if (e > s && e - s <= 300) { h.nameStart = s; h.nameEnd = e; }
        }

        std::size_t lineEnd = lowered.find('\n', addrStart);
        if (lineEnd == npos) lineEnd = lowered.size();
        lineEnd = std::min(lineEnd, addrStart + 600);
        std::size_t kp = lowered.find("по кп", addrStart);
        if (kp != npos && kp >= lineEnd) kp = npos;
        const std::size_t regionEnd = (kp != npos) ? kp : lineEnd;

        std::vector<std::pair<std::size_t, std::size_t>> segs;
        std::size_t segStart = addrStart;
        for (std::size_t k = addrStart; k <= regionEnd; ++k) {
            if (k == regionEnd || text[k] == ',') {
                const std::size_t s = skipSpaces(text, segStart);
                const std::size_t e = skipSpacesBack(text, k);
                if (e > s) segs.emplace_back(s, e);
                segStart = k + 1;
            }
        }
        if (!segs.empty() && !looksLikeIdSegment(lowered, segs[0].first, segs[0].second)) {
            std::size_t idIdx = npos;
            for (std::size_t i = 1; i < segs.size(); ++i) {
                if (looksLikeIdSegment(lowered, segs[i].first, segs[i].second)) { idIdx = i; break; }
            }
            std::size_t lastAddr = 0;
            if (idIdx != npos) {
                lastAddr = idIdx - 1;
            } else if (kp != npos) {
                lastAddr = segs.size() - 1;
            } else if (segs.size() > 1 && hasStreetMarker(lowered, segs[1].first, segs[1].second)) {
                lastAddr = 1;
            }
            if (segs[0].second - segs[0].first <= 150) {
                h.townStart = segs[0].first;
                h.townEnd = segs[0].second;
            }
            if (lastAddr >= 1 && segs[lastAddr].second - segs[1].first <= 400) {
                h.streetStart = segs[1].first;
                h.streetEnd = segs[lastAddr].second;
            }
        }
        out.push_back(h);
    }
    return out;
}

bool chainedBefore(const std::string& t, std::size_t pos) {
    if (pos == 0) return false;
    const char prev = t[pos - 1];
    if (isAsciiDigit(prev)) return true;
    return (prev == '.' || prev == '/' || prev == '-' || prev == ',') &&
           pos >= 2 && isAsciiDigit(t[pos - 2]);
}

bool chainedAfter(const std::string& t, std::size_t end) {
    if (end >= t.size()) return false;
    if (isAsciiDigit(t[end])) return true;
    return (t[end] == '.' || t[end] == '/' || t[end] == '-' || t[end] == ',') &&
           end + 1 < t.size() && isAsciiDigit(t[end + 1]);
}

bool numericNeighbour(const std::string& t, std::size_t pos, std::size_t end) {
    if (pos > 0 && isAsciiDigit(t[pos - 1])) return true;
    if (pos >= 2 && (t[pos - 1] == '.' || t[pos - 1] == ',') && isAsciiDigit(t[pos - 2])) return true;
    if (end < t.size() && isAsciiDigit(t[end])) return true;
    return end + 1 < t.size() && (t[end] == '.' || t[end] == ',') && isAsciiDigit(t[end + 1]);
}

bool followedByUnit(const std::string& lowered, std::size_t end) {
    const std::size_t p = skipSpaces(lowered, end);
    static const std::vector<std::string> units = {
        "мкг", "mcg", "мг", "mg", "мм", "mm", "см", "cm", "мл", "ml", "mmol", "ммол",
        "кг", "kg", "мин", "iu", "ед", "%", "ч", "h", "х", "x"
    };
    for (const auto& u : units) {
        if (startsWith(lowered, p, u)) {
            const std::size_t after = p + u.size();
            if (u == "%" || !isLetter(decodeAt(lowered, after, nullptr))) return true;
        }
    }
    return false;
}

int toInt(const std::string& s) { return s.empty() ? -1 : std::stoi(s); }
bool inRange(int v, int lo, int hi) { return v >= lo && v <= hi; }

bool acceptDate(const std::string& lowered, const std::smatch& m, std::size_t base,
                std::size_t& pos, std::size_t& len) {
    const std::size_t end = pos + len;
    auto g = [&](int i) { return m[i].matched ? m[i].str() : std::string(); };
    auto letterBefore = [&](std::size_t p) { return isLetter(prevCodepoint(lowered, p)); };

    if (m[1].matched) {
        const int d = toInt(g(1)), mo = toInt(g(3));
        const std::string y = g(4);
        const bool dayMonth = inRange(d, 1, 31) && inRange(mo, 1, 12);
        const bool monthDay = inRange(d, 1, 12) && inRange(mo, 1, 31);
        if (y.size() == 4) {
            if (!inRange(toInt(y), 1900, 2099) || !(dayMonth || monthDay)) return false;
            return !numericNeighbour(lowered, pos, end);
        }
        if (!dayMonth || g(2) == "-") return false;
        if (y.size() == 3 && !startsWith(y, 0, "19") && !startsWith(y, 0, "20")) return false;
        return !chainedBefore(lowered, pos) && !chainedAfter(lowered, end) &&
               !followedByUnit(lowered, end);
    }
    if (m[5].matched) {
        return inRange(toInt(g(6)), 1, 12) && inRange(toInt(g(7)), 1, 31) &&
               !chainedBefore(lowered, pos);
    }
    if (m[8].matched) {
        const std::string y = g(9);
        if (!inRange(toInt(g(8)), 1, 12)) return false;
        if (y.size() >= 4 && !inRange(toInt(y.substr(0, 4)), 1900, 2099)) return false;
        return !letterBefore(pos);
    }
    if (m[10].matched) {
        return inRange(toInt(g(10)), 1, 12) && inRange(toInt(g(11)), 1900, 2099) &&
               !chainedBefore(lowered, pos);
    }
    if (m[12].matched) {
        const bool spaced = m.position(13) > m.position(12) + m.length(12) + 1;
        if (spaced && g(12).size() != 2) return false;
        return inRange(toInt(g(12)), 1, 12) && inRange(toInt(g(13)), 1900, 2099) &&
               !numericNeighbour(lowered, pos, end);
    }
    if (m[14].matched) {
        return inRange(toInt(g(14)), 1, 12) && !chainedBefore(lowered, pos);
    }
    if (m[17].matched) {
        if (m[16].matched && !inRange(toInt(g(16)), 1, 31)) return false;
        return !letterBefore(pos) && !chainedBefore(lowered, pos);
    }
    if (m[19].matched) {
        return inRange(toInt(g(19)), 1, 31) && !letterBefore(pos) && !chainedBefore(lowered, pos);
    }
    if (m[21].matched) {
        if (letterBefore(pos)) return false;
        if (!inRange(toInt(g(21)), 1, 31) || !inRange(toInt(g(22)), 1, 12)) return false;
        const auto s = base + static_cast<std::size_t>(m.position(21));
        const auto e = base + static_cast<std::size_t>(m.position(22) + m.length(22));
        if (followedByUnit(lowered, e)) return false;
        pos = s;
        len = e - s;
        return true;
    }
    if (m[23].matched) {
        return inRange(toInt(g(23)), 1, 31) && inRange(toInt(g(24)), 1, 12) &&
               !chainedBefore(lowered, pos);
    }
    if (m[25].matched) {
        return kMaskStandaloneYears && !chainedBefore(lowered, pos);
    }
    return false;
}

bool isPlausibleBgPhone(std::string digits) {
    if (startsWith(digits, 0, "00359")) digits = digits.substr(5);
    else if (startsWith(digits, 0, "359")) digits = digits.substr(3);
    if (digits.empty()) return false;
    if (digits[0] != '0') digits = "0" + digits;
    const std::size_t n = digits.size();
    if (n == 10 && (startsWith(digits, 0, "087") || startsWith(digits, 0, "088") ||
                    startsWith(digits, 0, "089") || startsWith(digits, 0, "098") ||
                    startsWith(digits, 0, "099"))) return true;
    if (n == 9 && startsWith(digits, 0, "02")) return true;
    if ((n == 9 || n == 10) && digits[1] >= '3' && digits[1] <= '9' &&
        !startsWith(digits, 0, "087") && !startsWith(digits, 0, "088") &&
        !startsWith(digits, 0, "089")) return true;
    return false;
}

std::size_t validPhoneLength(const std::string& text, std::size_t pos, std::size_t len) {
    std::size_t best = 0;
    std::string digits;
    for (std::size_t i = pos; i < pos + len; ++i) {
        if (isAsciiDigit(text[i])) {
            digits += text[i];
            const bool groupEnd = (i + 1 == pos + len) || !isAsciiDigit(text[i + 1]);
            if (groupEnd && isPlausibleBgPhone(digits)) best = i + 1 - pos;
        }
    }
    return best;
}

bool acceptLocation(const std::string& text, std::size_t pos, std::size_t len) {
    const std::string match = text.substr(pos, len);
    const bool cityOrVillage = startsWith(match, 0, "гр.") || startsWith(match, 0, "Гр.") ||
                               startsWith(match, 0, "с.") || startsWith(match, 0, "град") ||
                               startsWith(match, 0, "село");
    if (!cityOrVillage) return true;

    const std::size_t before = skipSpacesBack(text, pos);
    if (before > 0 && (isAsciiDigit(text[before - 1]) || text[before - 1] == '/')) return false;

    const std::size_t after = skipSpaces(text, pos + len);
    if (after < text.size() && text[after] == ':') return false;

    std::size_t nameStart = startsWith(match, 0, "с.") ? std::string("с.").size() :
                            startsWith(match, 0, "село") ? std::string("село").size() : 0;
    if (nameStart > 0) {
        nameStart = skipSpaces(text, pos + nameStart);
        const std::size_t wl = nameWordLength(text, nameStart, false);
        if (wl > 0 && isEponym(lowerUtf8(text.substr(nameStart, wl)))) return false;
    }
    return true;
}

} // namespace

EntityDetector::EntityDetector() {
    regexes_ = {
        {EntityType::IDNumber, std::regex(
            R"((?:И\.?З\.?|Из(?=[ \t]*№)|НЗОК|РЗОК|УИН|ЛЗ|ЛКК|РЗИ|Амб(?:\.|улаторен)?[ \t]*(?:лист|№)|Рег\.?[ \t]*(?:№)?)[ \t]*(?:(?:[:.-]|№)[ \t]*)?[A-Za-z]{0,3}[0-9][A-Za-z0-9./-]{0,15})",
            std::regex::icase
        )},
        {EntityType::Location, std::regex(
            R"((?:(?:гр|Гр|с|ул|Ул|бул|Бул|кв|Кв|пл)\.|ж\.?[ \t]?к\.?|Ж\.?[ \t]?(?:к|К)\.?)[ \t]*)"
            R"((?:)" CYR_UP CYR_LO R"({0,2}\.[ \t]*){0,2})"
            R"((?:)" CYR_UP CYR_LO R"(+(?:-)" CYR_UP CYR_LO R"(+)?|[A-Z][a-z]+))"
            R"((?:[ \t]+(?:\xD0\xB8[ \t]+)?(?:)" CYR_UP CYR_LO R"({0,2}\.[ \t]*){0,2}(?:)" CYR_UP CYR_LO R"(+(?:-)" CYR_UP CYR_LO R"(+)?|[A-Z][a-z]+|\d{1,3}(?:[A-Za-z]|)" CYR_UP R"()?(?![0-9]))){0,3})"
            R"((?:[ \t]*,?[ \t]*(?:No|№|(?:бл|вх|ет|ап)(?:\.|(?=[ \t]*\d)))[ \t]*(?:\d{1,4}(?:[A-Za-z]|)" CYR_UP R"(|)" CYR_LO R"()?(?![0-9])|[A-Za-z]|)" CYR_UP R"(|)" CYR_LO R"(|\?))*)"
            R"(|(?:град|село|улица|булевард|квартал|площад)[ \t]+)"
            R"((?:)" CYR_UP CYR_LO R"({0,2}\.[ \t]*){0,2})"
            R"((?:)" CYR_UP CYR_LO R"(+(?:-)" CYR_UP CYR_LO R"(+)?)(?:[ \t]+)" CYR_UP CYR_LO R"(+){0,2})"
        )},
        {EntityType::Phone, std::regex(
            R"((?:\+[ \t]?359|00[ \t]?359|\(0|0)[0-9 \t()/-]{6,16}[0-9])"
        )},
        {EntityType::Date, std::regex(
            R"((\d{1,2})[ \t]?([./-])[ \t]?(\d{1,2})[ \t]?\2[ \t]?(\d{4}|\d{3}|\d{2})(?![0-9]))"
            R"(|((?:19|20)\d{2})-(\d{1,2})-(\d{1,2})(?![0-9]))"
            R"(|(?:мес|м)\.?[ \t]*(\d{1,2})[ \t]*[./][ \t]*(\d{4,5}|\d{2})(?![0-9]))"
            R"((?:[ \t]*(?:година|год\.?|г\.?)(?!)" CYR_LO R"())?)"
            R"(|(\d{1,2})[ \t]*(?:мес|м)\.?[ \t]*(\d{4})(?![0-9]))"
            R"((?:[ \t]*(?:година|год\.?|г\.?)(?!)" CYR_LO R"())?)"
            R"(|(\d{1,2})\.[ \t]?(\d{4})(?![0-9]))"
            R"((?:[ \t]*(?:година|год\.?|г\.?)(?!)" CYR_LO R"())?)"
            R"(|(\d{2})\.(\d{2})[ \t]*(?:година|год\.?|г\.?)(?!)" CYR_LO R"())"
            R"(|(?:(\d{1,2})[ \t]*)?(?:м\.?[ \t]*)?)"
            R"((януари|февруари|март|април|май|юни|юли|август|септември|октомври|ноември|декември|ян\.|фев(?:р)?\.|апр\.|авг\.|сеп(?:т)?\.|окт\.|ное(?:м)?\.|дек\.)\.?)"
            R"([ \t]*,?[ \t]*((?:19|20)\d{2})(?![0-9]))"
            R"((?:[ \t]*(?:година|год\.?|г\.?)(?!)" CYR_LO R"())?)"
            R"(|(\d{1,2})(?:[ \t]*-?[ \t]*(?:ви|ри|ти|ми|и))?[ \t]+)"
            R"((януари|февруари|март|април|май|юни|юли|август|септември|октомври|ноември|декември)(?!)" CYR_LO R"())"
            R"(|(?:на|от|до|след|преди|na|ot|do)[ \t]*\(?[ \t]*(\d{1,2})\.(\d{2})(?![0-9]|[ \t]?[./][ \t]?[0-9]))"
            R"(|(\d{1,2})\.(\d{2})(?=[ \t]*(?:и|-|–|до)[ \t]*\d{1,2}\.\d{1,2}\.\d{2,4}))"
            R"(|((?:19|20)\d{2})[ \t]*(?:година|год\.?|г\.?)(?!)" CYR_LO R"())"
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
            "Ловеч", "Разград", "Смолян", "Ботевград", "Сандански", "Самоков",
            "Димитровград", "Горна Оряховица", "Дупница", "Петрич", "Севлиево", "Нова Загора",
            "Троян", "Айтос", "Поморие", "Несебър", "Созопол", "Карлово", "Велинград",
            "Панагюрище", "Гоце Делчев", "Разлог", "Банско", "Свищов", "Попово", "Балчик",
            "Каварна", "Харманли", "Свиленград", "Чирпан", "Раднево", "Първомай", "Раковски",
            "Своге", "Костинброд", "Елин Пелин", "Ихтиман", "Божурище", "Банкя", "Нови Искър",
            "Сливница", "Годеч", "Драгоман", "Правец", "Етрополе", "Пирдоп", "Златица",
            "Радомир", "Брезник", "Козлодуй", "Мездра", "Берковица", "Червен бряг",
            "Бяла Слатина", "Луковит", "Тетевен", "Никопол", "Левски", "Белене", "Павликени",
            "Елена", "Дряново", "Трявна", "Тутракан", "Дулово", "Исперих", "Кубрат",
            "Нови пазар", "Велики Преслав", "Провадия", "Девня", "Девин", "Златоград",
            "Мадан", "Момчилград", "Крумовград", "Ардино", "Хисаря", "Сопот", "Стамболийски",
            "Перущица", "Кричим", "Белово", "Костенец", "Долна баня",
            "Сапарева баня", "Кресна", "Симитли", "Якоруда", "Белица", "Момин проход",
            "Суходол", "Бояна", "Драгалевци", "Симеоново", "Княжево", "Овча купел", "Люлин",
            "Младост", "Дружба", "Надежда", "Лозенец", "Изгрев", "Красно село"
        }},
        {EntityType::Organization, {
            "Министерство на здравеопазването", "НЗОК", "РЗОК", "РЗИ", "НОИ", "МВР",
            "Пирогов", "Александровска", "Александровкса", "Токуда", "Софиямед",
            "Аджибадем", "Сити Клиник", "УниХоспитал", "Пълмед", "Царица Йоанна",
            "Царица Йоана", "Св. Иван Рилски", "Св.Иван Рилски", "Свети Иван Рилски",
            "Иван Рилски", "Св. Анна", "Св.Анна", "Света Анна", "Св. Георги", "Св.Георги",
            "Св. Марина", "Св.Марина", "Св. Екатерина", "Св.Екатерина", "Света Екатерина",
            "Св. Наум", "Св.Наум", "Свети Наум", "Св. Мина", "Св. Иван", "Св.Иван",
            "Курило", "Карлуково"
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

    std::sort(rawResults.begin(), rawResults.end(),
              [](const DetectedEntity& a, const DetectedEntity& b) {
                  if (a.start != b.start) return a.start < b.start;
                  return a.length > b.length;
              });
    std::vector<DetectedEntity> merged;
    for (const auto& e : rawResults) {
        if (e.length == 0) continue;
        if (!merged.empty() && e.start < merged.back().start + merged.back().length) {
            auto& last = merged.back();
            const std::size_t end = std::max(last.start + last.length, e.start + e.length);
            if (e.score > last.score) {
                last.type = e.type;
                last.score = e.score;
            }
            last.length = end - last.start;
            last.text = text.substr(last.start, last.length);
        } else {
            merged.push_back(e);
        }
    }
    return merged;
}

void EntityDetector::detectRegex(const std::string& text, std::vector<DetectedEntity>& results) const {
    const std::string lowered = lowerUtf8(text);

    auto atBoundary = [&](std::size_t offset) {
        return offset == text.size() ||
            (static_cast<unsigned char>(text[offset]) & 0xC0) != 0x80;
    };
    auto push = [&](EntityType type, std::size_t pos, std::size_t len, double conf) {
        if (len == 0) return;
        if (!atBoundary(pos) || !atBoundary(pos + len)) {
            throw std::runtime_error(
                "Detector regex split a UTF-8 character; type=" +
                std::to_string(static_cast<int>(type)) +
                " byte_start=" + std::to_string(pos) +
                " byte_length=" + std::to_string(len));
        }
        results.push_back({type, pos, len, text.substr(pos, len), conf});
    };

    for (const auto& [type, pattern] : regexes_) {
        const std::string& subject = (type == EntityType::Date) ? lowered : text;
        std::size_t from = 0;
        std::smatch m;
        while (from < subject.size()) {
            const auto flags = from > 0 ? std::regex_constants::match_prev_avail
                                        : std::regex_constants::match_default;
            if (!std::regex_search(subject.cbegin() + static_cast<std::ptrdiff_t>(from),
                                   subject.cend(), m, pattern, flags)) {
                break;
            }
            const std::size_t matchPos = from + static_cast<std::size_t>(m.position(0));
            const std::size_t matchLen = static_cast<std::size_t>(m.length(0));
            std::size_t pos = matchPos;
            std::size_t len = matchLen;

            bool ok = len > 0;
            if (ok && type == EntityType::Date) ok = acceptDate(lowered, m, from, pos, len);
            if (ok && type == EntityType::Location) ok = acceptLocation(text, pos, len);
            if (ok && type == EntityType::Phone) {
                if (pos > 0 && (isAsciiDigit(text[pos - 1]) || text[pos - 1] == '.' ||
                                text[pos - 1] == ',')) {
                    ok = false;
                } else {
                    len = validPhoneLength(text, pos, len);
                    ok = len > 0 && !chainedAfter(text, pos + len);
                }
            }
            if (ok && (type == EntityType::Location ||
                       type == EntityType::IDNumber ||
                       type == EntityType::Phone) &&
                isWordBefore(text, pos)) {
                ok = false;
            }
            if (ok && len > (type == EntityType::Location ? 512u : 80u)) ok = false;

            if (ok) {
                push(type, pos, len, 0.95);
                from = matchPos + matchLen;
            } else {
                from = matchPos + getUtf8SequenceLength(static_cast<unsigned char>(subject[matchPos]));
            }
        }
    }

    static const std::regex hospital(
        R"(((?:УМБАЛ|МБАЛ|УСБАЛ|СБАЛ|ДПБ|ЦПЗ)(?:\xD0[\x90-\xAF]){0,4}|ДКЦ|МДЦ|МЦ|ЦСМП|КОЦ|ВМА|ИСУЛ|НКБ|ОДПЗС))"
        R"((?!)" CYR_UP R"(|)" CYR_LO R"(|[A-Za-z]))"
        R"((?:[ \t]*(?:-|–)?[ \t]*(?:"[^"\n]{1,60}"|„[^"\n]{1,60}?(?:“|”|")|“[^"\n]{1,60}?”|)"
        R"((?:)" CYR_UP CYR_LO R"({0,2}\.[ \t]*){0,2})" CYR_UP CYR_LO R"(+)"
        R"((?:[ \t]+(?:)" CYR_UP CYR_LO R"({0,2}\.[ \t]*){0,2})" CYR_UP CYR_LO R"(+){0,2}))?)"
    );
    for (std::size_t from = 0; from < text.size();) {
        std::smatch hm;
        const auto flags = from > 0 ? std::regex_constants::match_prev_avail
                                    : std::regex_constants::match_default;
        if (!std::regex_search(text.cbegin() + static_cast<std::ptrdiff_t>(from), text.cend(),
                               hm, hospital, flags)) {
            break;
        }
        const std::size_t pos = from + static_cast<std::size_t>(hm.position(0));
        if (isWordBefore(text, pos)) {
            from = pos + getUtf8SequenceLength(static_cast<unsigned char>(text[pos]));
            continue;
        }
        auto len = static_cast<std::size_t>(hm.length(0));
        from = pos + std::max<std::size_t>(len, 1);
        const std::size_t acronymEnd = pos + static_cast<std::size_t>(hm.length(1));
        std::size_t nameStart = acronymEnd;
        while (nameStart < pos + len &&
               (text[nameStart] == ' ' || text[nameStart] == '\t' || text[nameStart] == '-' ||
                text[nameStart] == '"' || static_cast<unsigned char>(text[nameStart]) >= 0x80) &&
               nameWordLength(text, nameStart, false) == 0) {
            nameStart += getUtf8SequenceLength(static_cast<unsigned char>(text[nameStart]));
        }
        const std::size_t wl = nameWordLength(text, nameStart, false);
        if (wl > 0 && isStopWord(text.substr(nameStart, wl))) len = acronymEnd - pos;
        push(EntityType::Organization, pos, len, 0.93);
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

    for (const auto& h : findHeaders(text, lowered)) {
        if (h.townStart != npos) push(EntityType::Location, h.townStart, h.townEnd - h.townStart, 0.995);
        if (h.streetStart != npos) push(EntityType::Location, h.streetStart, h.streetEnd - h.streetStart, 0.995);
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
    static const std::regex uinRegex(R"(УИН[ \t]*(?:(?::|№|\.)[ \t]*)?(\d{10})(?![0-9]))", std::regex::icase);
    for (std::sregex_iterator it(text.begin(), text.end(), uinRegex), end; it != end; ++it) {
        if (isWordBefore(text, static_cast<std::size_t>(it->position()))) continue;
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
    const std::string lowered = lowerUtf8(text);
    auto scan = [&](EntityType type, const std::string& haystack, const std::string& needle,
                    const std::string& original) {
        std::size_t pos = haystack.find(needle);
        while (pos != npos) {
            const bool eponymPirogov = lowerUtf8(original) == "пирогов" && pos >= 5 &&
                                       lowered.compare(pos - 5, 5, "по ") == 0;
            if (!eponymPirogov &&
                !isWordBefore(text, pos) &&
                !isWordAfter(text, pos + needle.length())) {
                results.push_back({type, pos, needle.length(), text.substr(pos, needle.length()), 0.85});
            }
            pos = haystack.find(needle, pos + needle.length());
        }
    };

    for (const auto& [type, terms] : dictionaries_) {
        for (const auto& term : terms) {
            if (type == EntityType::Location) {
                scan(type, text, term, term);
                const std::string upper = upperUtf8(term);
                if (upper != term) scan(type, text, upper, term);
            } else {
                scan(type, lowered, lowerUtf8(term), term);
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

    static const std::regex labelled(R"((?:ЕГН|ЛНЧ|ЕНЧ)[ \t]*(?:(?::|№|-)[ \t]*)?(\d{10})(?![0-9]))");
    for (std::sregex_iterator it(text.begin(), text.end(), labelled), end; it != end; ++it) {
        if (isWordBefore(text, static_cast<std::size_t>(it->position()))) continue;
        results.push_back({
            EntityType::IDNumber,
            static_cast<std::size_t>(it->position(1)),
            static_cast<std::size_t>(it->length(1)),
            (*it)[1].str(),
            0.99
        });
    }
}

void EntityDetector::detectNER(const std::string& text, std::vector<DetectedEntity>& results) const {
    const std::string lowered = lowerUtf8(text);

    for (const auto& h : findHeaders(text, lowered)) {
        if (h.nameStart != npos) {
            results.push_back({EntityType::Person, h.nameStart, h.nameEnd - h.nameStart,
                               text.substr(h.nameStart, h.nameEnd - h.nameStart), 0.99});
        }
    }

    static const std::regex titleRe(R"((?:акад|проф|доц|гл\.[ \t]?ас|ас)\.|д(?:-|–|‐)р\.?|dr\.|г-жа|г-н)");

    for (std::sregex_iterator it(lowered.begin(), lowered.end(), titleRe), end; it != end; ++it) {
        const auto start = static_cast<std::size_t>(it->position());
        if (isWordBefore(text, start)) continue;
        std::size_t cursor = start + static_cast<std::size_t>(it->length());

        while (true) {
            const std::size_t q = skipSpaces(lowered, cursor);
            std::smatch more;
            if (q < lowered.size() &&
                std::regex_search(lowered.cbegin() + static_cast<std::ptrdiff_t>(q), lowered.cend(),
                                  more, titleRe, std::regex_constants::match_continuous)) {
                cursor = q + static_cast<std::size_t>(more.length());
            } else {
                break;
            }
        }

        std::size_t i = cursor;
        std::size_t lastEnd = npos;
        int words = 0;
        for (int token = 0; token < 6 && words < 3; ++token) {
            const std::size_t j = skipSpaces(text, i);
            std::size_t l = 0;
            const char32_t c = decodeAt(text, j, &l);
            if (isUpperLetter(c) && j + l < text.size() && text[j + l] == '.') {
                i = j + l + 1;
                lastEnd = i;
                continue;
            }
            const std::size_t wl = nameWordLength(text, j, true);
            if (wl == 0 || isStopWord(text.substr(j, wl))) break;
            i = j + wl;
            lastEnd = i;
            if (wl <= 4 && i < text.size() && text[i] == '.') {
                lastEnd = ++i;
                continue;
            }
            ++words;
        }
        if (lastEnd != npos) {
            results.push_back({EntityType::Person, start, lastEnd - start,
                               text.substr(start, lastEnd - start), 0.99});
        }
    }

    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t w1 = 0;
        if (!isWordBefore(text, i) && (w1 = nameWordLength(text, i, false)) > 0) {
            std::vector<std::pair<std::size_t, std::size_t>> seq;
            std::size_t p = i;
            std::size_t wl = w1;
            while (wl > 0 && seq.size() < 6) {
                const std::string w = text.substr(p, wl);
                if (isStopWord(w)) break;
                seq.emplace_back(p, p + wl);
                const std::size_t next = skipSpaces(text, p + wl);
                if (next == p + wl) break;
                p = next;
                wl = nameWordLength(text, p, false);
            }

            if (seq.size() >= 2) {
                std::size_t last = npos;
                for (std::size_t k = 0; k < seq.size(); ++k) {
                    const std::string w = text.substr(seq[k].first, seq[k].second - seq[k].first);
                    if (looksLikeSurname(w, k + 1 == seq.size())) last = k;
                }
                if (last != npos) {
                    std::size_t first = (last >= 2) ? last - 2 : 0;
                    std::size_t lastWord = last;
                    if (last == 0) lastWord = 1;
                    const std::size_t s = seq[first].first;
                    const std::size_t e = seq[lastWord].second;
                    if (e - s <= 160) {
                        results.push_back({EntityType::Person, s, e - s, text.substr(s, e - s), 0.88});
                    }
                }
                i = seq.back().second;
                continue;
            }
            if (!seq.empty()) {
                i = seq.back().second;
                continue;
            }
            i += w1;
            continue;
        }
        i += getUtf8SequenceLength(static_cast<unsigned char>(text[i]));
    }
}

#undef CYR_UP
#undef CYR_LO

} // namespace deid