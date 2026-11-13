#include "extractor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#endif

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

const std::vector<std::string> EXTRA_COLUMNS = {"Изходен файл", "№ в изходния файл"};

enum Field {
    F_DIAGNOSIS = 0, F_COMORBID, F_ANAMNESIS, F_PRIOR_TESTS, F_PAST_DISEASES, F_HOME_THERAPY,
    F_FAMILY, F_ALLERGIES, F_OBJECTIVE, F_NEURO, F_PSYCH, F_TESTS, F_THERAPY, F_COURSE,
    F_CONSULTS, F_NEUROPSYCH, F_SHORT_MEMORY, F_BENTON, F_ISAACS, F_STROOP, F_CONCLUSION,
    F_COMPLICATIONS, F_OUTCOME, F_DISCUSSION, F_FOLLOWUP, F_RECOMMENDATIONS, F_GP,
    F_TERMINATOR = -1
};

std::size_t spaceLengthAt(const std::string& s, std::size_t pos) {
    const auto c = static_cast<unsigned char>(s[pos]);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 1;
    if (c == 0xC2 && pos + 1 < s.size() && static_cast<unsigned char>(s[pos + 1]) == 0xA0) return 2;
    if (c == 0xE2 && pos + 2 < s.size() && static_cast<unsigned char>(s[pos + 1]) == 0x80 &&
        (static_cast<unsigned char>(s[pos + 2]) == 0xA8 || static_cast<unsigned char>(s[pos + 2]) == 0xA9)) {
        return 3;
    }
    return 0;
}

std::string trim(const std::string& str) {
    std::size_t start = 0;
    std::size_t end = str.size();
    while (start < end) {
        const std::size_t l = spaceLengthAt(str, start);
        if (l == 0) break;
        start += l;
    }
    while (end > start) {
        std::size_t p = end - 1;
        while (p > start && (static_cast<unsigned char>(str[p]) & 0xC0) == 0x80) --p;
        const std::size_t l = spaceLengthAt(str, p);
        if (l == 0 || p + l != end) break;
        end = p;
    }
    return str.substr(start, end - start);
}

std::size_t utf8Length(unsigned char lead) {
    if ((lead & 0x80) == 0) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

char32_t decodeAt(const std::string& s, std::size_t pos, std::size_t* len = nullptr) {
    if (pos >= s.size()) {
        if (len) *len = 0;
        return 0;
    }
    const auto b0 = static_cast<unsigned char>(s[pos]);
    std::size_t n = utf8Length(b0);
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
    return decodeAt(s, p);
}

bool isUpperLetter(char32_t c) { return (c >= 'A' && c <= 'Z') || (c >= 0x400 && c <= 0x42F); }
bool isLowerLetter(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 0x430 && c <= 0x45F); }
bool isLetter(char32_t c) { return isUpperLetter(c) || isLowerLetter(c); }
bool isInlineSpace(char32_t c) { return c == ' ' || c == '\t' || c == 0xA0; }
bool isLineBreak(char32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }

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

struct TitlePos {
    std::size_t start;
    std::size_t end;
};

std::vector<TitlePos> findEpicrisisPositions(const std::string& content) {
    static const char32_t upper[8] = {0x415, 0x41F, 0x418, 0x41A, 0x420, 0x418, 0x417, 0x410};
    static const char32_t lower[8] = {0x435, 0x43F, 0x438, 0x43A, 0x440, 0x438, 0x437, 0x430};
    std::vector<TitlePos> positions;

    std::size_t i = 0;
    while (i < content.size()) {
        std::size_t len = 0;
        const char32_t c = decodeAt(content, i, &len);
        if (c != upper[0] || isLetter(prevCodepoint(content, i))) {
            i += len ? len : 1;
            continue;
        }

        std::size_t j = i;
        int matched = 0;
        while (j < content.size() && matched < 8) {
            std::size_t l = 0;
            const char32_t d = decodeAt(content, j, &l);
            if (matched > 0 && isInlineSpace(d)) { j += l; continue; }
            if (d != upper[matched]) break;
            ++matched;
            j += l;
        }
        bool ok = matched == 8 && !isLetter(decodeAt(content, j));

        if (!ok) {
            j = i;
            matched = 0;
            while (j < content.size() && matched < 8) {
                std::size_t l = 0;
                const char32_t d = decodeAt(content, j, &l);
                if (d != (matched == 0 ? upper[0] : lower[matched])) break;
                ++matched;
                j += l;
            }
            if (matched == 8 && !isLetter(decodeAt(content, j))) {
                std::size_t b = i;
                bool aloneBefore = true;
                while (b > 0) {
                    std::size_t p = b - 1;
                    while (p > 0 && (static_cast<unsigned char>(content[p]) & 0xC0) == 0x80) --p;
                    const char32_t d = decodeAt(content, p);
                    if (isLineBreak(d)) break;
                    if (!isInlineSpace(d)) { aloneBefore = false; break; }
                    b = p;
                }
                std::size_t a = j;
                bool aloneAfter = true;
                while (a < content.size()) {
                    std::size_t l = 0;
                    const char32_t d = decodeAt(content, a, &l);
                    if (isLineBreak(d)) break;
                    if (!isInlineSpace(d)) { aloneAfter = false; break; }
                    a += l;
                }
                ok = aloneBefore && aloneAfter;
            }
        }

        if (ok) {
            positions.push_back({i, j});
            i = j;
        } else {
            i += len ? len : 1;
        }
    }
    return positions;
}

#define LBL_WS R"((?:[ \t]|\xC2\xA0))"
#define LBL_S LBL_WS "+"
#define LBL_LO R"((?:\xD0[\xB0-\xBF]|\xD1[\x80-\x9F]))"

struct LabelSpec {
    int field;
    std::string pattern;
    bool allowDash;
    bool separatorOptional;
};

const std::vector<LabelSpec>& labelSpecs() {
    static const std::vector<LabelSpec> specs = {
        {F_DIAGNOSIS, "окончателна" LBL_S "диагноза", false, false},
        {F_COMORBID, "придружаващи" LBL_S "заболявания", false, false},
        {F_ANAMNESIS, "анамнеза", false, false},
        {F_PRIOR_TESTS, "проведени" LBL_S "изследвания", true, false},
        {F_PAST_DISEASES, "минали" LBL_S "(?:и" LBL_S ")?придружава" LBL_LO "{0,4}" LBL_S "заболявания", true, false},
        {F_HOME_THERAPY, "приеман(?:а|и)" LBL_S "(?:терапия|медикаменти)(?:" LBL_S "към" LBL_S "момента(?:" LBL_S "на" LBL_S
                         "хоспитализ" LBL_LO "{0,8})?)?", true, false},
        {F_FAMILY, "фамилна" LBL_S "(?:анамнеза|обремененост)", true, false},
        {F_ALLERGIES, "алерги(?:и|я)?", true, false},
        {F_OBJECTIVE, "обективно" LBL_S "състояние", false, false},
        {F_NEURO, "неврологичен" LBL_S "статус", true, false},
        {F_PSYCH, "(?:психичен" LBL_S "|психологичен" LBL_S "|псих\\." LBL_WS "*|псих" LBL_S ")статус", true, false},
        {F_TESTS, "изследвания", false, false},
        {F_THERAPY, "терапия", false, false},
        {F_COURSE, "ход" LBL_S "на" LBL_S "заболяването", false, false},
        {F_CONSULTS, "консултативни" LBL_S "прегледи", false, false},
        {F_NEUROPSYCH, "к-т" LBL_S "по" LBL_S "невропсихология|(?:невро)?психологично" LBL_S "изследване", false, false},
        {F_SHORT_MEMORY, "краткосрочна" LBL_S "(?:слухова" LBL_S ")?памет", true, false},
        {F_BENTON, "benton" LBL_S "visual" LBL_S "retention" LBL_S "test", true, false},
        {F_ISAACS, "isaacs(?:’|'|‘|`)?" LBL_WS "*set" LBL_S "test", true, true},
        {F_STROOP, "(?:струп" LBL_S "тес(?:т)?|stroop" LBL_S "test)(?:" LBL_WS "*(?:/|,)" LBL_WS "*(?:frontal" LBL_S
                   "assessment" LBL_S "battery|clock" LBL_S "drawing" LBL_S "test|digit" LBL_S "span" LBL_S "test))?",
         true, false},
        {F_CONCLUSION, "заключение", false, false},
        {F_COMPLICATIONS, "настъпили" LBL_S "усложнения", false, false},
        {F_OUTCOME, "изход" LBL_S "от" LBL_S "заболяването", false, false},
        {F_DISCUSSION, "обсъждане", false, false},
        {F_FOLLOWUP, "контролни" LBL_S "прегледи", false, false},
        {F_RECOMMENDATIONS, "препоръки" LBL_S "и" LBL_S "назначения", false, false},
        {F_GP, "препоръки" LBL_S "към" LBL_S "(?:опл|общо" LBL_S "практикуващ" LBL_S "лекар(?:" LBL_WS "*\\(опл\\))?)", false, false},
        {F_TERMINATOR, "описание" LBL_S "на" LBL_S "документите", false, true},
        {F_TERMINATOR, "лекуващ" LBL_S "лекар", false, true},
        {F_TERMINATOR, "началник" LBL_S "отделение", false, true},
        {F_TERMINATOR, "началник" LBL_S "клиника", false, true},
        {F_TERMINATOR, "издаден" LBL_S "болничен" LBL_S "лист", false, true},
    };
    return specs;
}

#undef LBL_WS
#undef LBL_S
#undef LBL_LO

struct CompiledSpec {
    const LabelSpec* spec;
    std::regex re;
};

const std::vector<CompiledSpec>& compiledSpecs() {
    static const std::vector<CompiledSpec> compiled = [] {
        std::vector<CompiledSpec> out;
        for (const auto& spec : labelSpecs()) {
            const std::string sep = spec.allowDash ? "(?::|-|–|—)" : ":";
            const std::string re = "(" + spec.pattern + ")(?:[ \\t]|\\xC2\\xA0)*" +
                                   "(?:" + sep + ")" + (spec.separatorOptional ? "?" : "") +
                                   "(?:[ \\t]|\\xC2\\xA0)*";
            out.push_back({&spec, std::regex(re)});
        }
        return out;
    }();
    return compiled;
}

struct Occurrence {
    int field;
    std::size_t start;
    std::size_t labelEnd;
    std::size_t contentStart;
};

std::vector<Occurrence> findLabels(const std::string& text, const std::string& lowered) {
    std::vector<Occurrence> all;
    for (const auto& cs : compiledSpecs()) {
        for (std::sregex_iterator it(lowered.begin(), lowered.end(), cs.re), end; it != end; ++it) {
            const auto start = static_cast<std::size_t>(it->position());
            const std::size_t labelEnd = start + static_cast<std::size_t>(it->length(1));
            const std::size_t contentStart = start + static_cast<std::size_t>(it->length());
            if (!isUpperLetter(decodeAt(text, start))) continue;
            if (isUpperLetter(prevCodepoint(text, start))) continue;
            if (isLetter(decodeAt(text, labelEnd))) continue;
            all.push_back({cs.spec->field, start, labelEnd, contentStart});
        }
    }
    std::vector<Occurrence> kept;
    for (const auto& a : all) {
        bool inside = false;
        for (const auto& b : all) {
            if (&a == &b) continue;
            if (b.start <= a.start && a.labelEnd <= b.labelEnd &&
                (b.labelEnd - b.start) > (a.labelEnd - a.start)) {
                inside = true;
                break;
            }
        }
        if (!inside) kept.push_back(a);
    }
    std::sort(kept.begin(), kept.end(),
              [](const Occurrence& x, const Occurrence& y) { return x.start < y.start; });
    return kept;
}

const std::vector<int>& mainOrder() {
    static const std::vector<int> order = {
        F_DIAGNOSIS, F_COMORBID, F_ANAMNESIS, F_OBJECTIVE, F_TESTS, F_THERAPY, F_COURSE,
        F_CONSULTS, F_COMPLICATIONS, F_OUTCOME, F_DISCUSSION, F_FOLLOWUP, F_RECOMMENDATIONS, F_GP
    };
    return order;
}

int parentOf(int field) {
    switch (field) {
        case F_PRIOR_TESTS: case F_PAST_DISEASES: case F_HOME_THERAPY: case F_FAMILY: case F_ALLERGIES:
            return F_ANAMNESIS;
        case F_NEURO: case F_PSYCH:
            return F_OBJECTIVE;
        case F_NEUROPSYCH: case F_SHORT_MEMORY: case F_BENTON: case F_ISAACS: case F_STROOP: case F_CONCLUSION:
            return F_CONSULTS;
        default:
            return -1;
    }
}

std::string cleanContent(const std::string& raw) {
    std::string s = trim(raw);
    while (!s.empty()) {
        if (s[0] == '-' || s[0] == ':' || s[0] == '.' || s[0] == ',' || s[0] == ';') {
            s = trim(s.substr(1));
            continue;
        }
        if (s.compare(0, 3, "–") == 0 || s.compare(0, 3, "—") == 0) {
            s = trim(s.substr(3));
            continue;
        }
        break;
    }
    return s;
}

struct ParsedEpicrisis {
    std::map<std::string, std::string> data;
    std::string header;
};

ParsedEpicrisis parseEpicrisis(const std::string& text) {
    ParsedEpicrisis result;
    for (const auto& field : FIELDS) result.data[field] = "";

    const std::string lowered = lowerUtf8(text);
    const std::vector<Occurrence> occs = findLabels(text, lowered);
    if (occs.empty()) return result;
    result.header = trim(text.substr(0, occs.front().start));

    const int nFields = static_cast<int>(FIELDS.size());
    std::vector<int> chosen(static_cast<std::size_t>(nFields), -1);
    auto occsOf = [&](int field) {
        std::vector<int> v;
        for (int k = 0; k < static_cast<int>(occs.size()); ++k)
            if (occs[static_cast<std::size_t>(k)].field == field) v.push_back(k);
        return v;
    };

    std::size_t lastMainStart = 0;
    bool haveMain = false;
    for (int f : mainOrder()) {
        const auto v = occsOf(f);
        if (v.empty()) continue;
        int pick = v.front();
        if (v.size() > 1 && haveMain) {
            for (int k : v) {
                if (occs[static_cast<std::size_t>(k)].start >= lastMainStart) { pick = k; break; }
            }
        }
        chosen[static_cast<std::size_t>(f)] = pick;
        lastMainStart = occs[static_cast<std::size_t>(pick)].start;
        haveMain = true;
    }

    std::vector<std::size_t> mainStarts;
    for (int f : mainOrder()) {
        const int k = chosen[static_cast<std::size_t>(f)];
        if (k >= 0) mainStarts.push_back(occs[static_cast<std::size_t>(k)].start);
    }
    std::sort(mainStarts.begin(), mainStarts.end());
    auto rangeOf = [&](int parent, std::size_t& from, std::size_t& to) {
        const int k = chosen[static_cast<std::size_t>(parent)];
        if (k < 0) return false;
        from = occs[static_cast<std::size_t>(k)].contentStart;
        to = text.size();
        for (std::size_t s : mainStarts) {
            if (s > occs[static_cast<std::size_t>(k)].start) { to = s; break; }
        }
        return true;
    };

    for (int f = 0; f < nFields; ++f) {
        const int parent = parentOf(f);
        if (parent < 0) continue;
        const auto v = occsOf(f);
        std::size_t from = 0, to = 0;
        const bool hasParent = rangeOf(parent, from, to);

        int pick = -1;
        if (hasParent) {
            std::size_t anchor = from;
            if (f == F_CONCLUSION) {
                for (int t : {F_NEUROPSYCH, F_SHORT_MEMORY, F_BENTON, F_ISAACS, F_STROOP}) {
                    const int k = chosen[static_cast<std::size_t>(t)];
                    if (k >= 0) {
                        const std::size_t s = occs[static_cast<std::size_t>(k)].start;
                        if (s >= from && s < to) anchor = std::max(anchor, s);
                    }
                }
            }
            for (int k : v) {
                const std::size_t s = occs[static_cast<std::size_t>(k)].start;
                if (s >= anchor && s < to) { pick = k; break; }
            }
            if (pick < 0 && f == F_CONCLUSION) {
                for (int k : v) {
                    const std::size_t s = occs[static_cast<std::size_t>(k)].start;
                    if (s >= from && s < to) { pick = k; break; }
                }
            }
            if (pick < 0 && f == F_HOME_THERAPY) {
                for (int k : occsOf(F_THERAPY)) {
                    const std::size_t s = occs[static_cast<std::size_t>(k)].start;
                    if (k != chosen[F_THERAPY] && s >= from && s < to) { pick = k; break; }
                }
            }
        }
        if (pick < 0 && f == F_CONCLUSION) {
            bool haveAnchor = false;
            std::size_t anchor = 0;
            for (int t : {F_NEUROPSYCH, F_SHORT_MEMORY, F_BENTON, F_ISAACS, F_STROOP}) {
                const int k = chosen[static_cast<std::size_t>(t)];
                if (k >= 0) {
                    anchor = std::max(anchor, occs[static_cast<std::size_t>(k)].start);
                    haveAnchor = true;
                }
            }
            if (haveAnchor) {
                std::size_t limit = text.size();
                for (std::size_t s : mainStarts) {
                    if (s > anchor) { limit = s; break; }
                }
                for (int k : v) {
                    const std::size_t s = occs[static_cast<std::size_t>(k)].start;
                    if (s > anchor && s < limit) { pick = k; break; }
                }
            }
        }
        if (pick < 0 && f != F_CONCLUSION && !v.empty()) pick = v.front();
        chosen[static_cast<std::size_t>(f)] = pick;
    }

    std::vector<std::size_t> bounds;
    for (int f = 0; f < nFields; ++f) {
        const int k = chosen[static_cast<std::size_t>(f)];
        if (k >= 0) bounds.push_back(occs[static_cast<std::size_t>(k)].start);
    }
    for (const auto& o : occs) {
        if (o.field == F_TERMINATOR) bounds.push_back(o.start);
    }
    std::sort(bounds.begin(), bounds.end());

    for (int f = 0; f < nFields; ++f) {
        const int k = chosen[static_cast<std::size_t>(f)];
        if (k < 0) continue;
        const Occurrence& o = occs[static_cast<std::size_t>(k)];
        std::size_t end = text.size();
        for (std::size_t b : bounds) {
            if (b > o.start) { end = b; break; }
        }
        if (end > o.contentStart) {
            result.data[FIELDS[static_cast<std::size_t>(f)]] =
                cleanContent(text.substr(o.contentStart, end - o.contentStart));
        }
    }
    return result;
}

std::string pathToUtf8(const fs::path& p) {
#if defined(__cpp_char8_t)
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
#else
    return p.u8string();
#endif
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

int runCommand(const std::string& command) {
#ifdef _WIN32
    const std::string full = command + " >NUL 2>NUL";
    const int n = MultiByteToWideChar(CP_UTF8, 0, full.c_str(), -1, nullptr, 0);
    if (n <= 0) return -1;
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, full.c_str(), -1, &wide[0], n);
    return _wsystem(wide.c_str());
#else
    const std::string full = command + " >/dev/null 2>&1";
    return std::system(full.c_str());
#endif
}

std::string readWholeFile(const fs::path& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return "";
    std::stringstream bufferStream;
    bufferStream << file.rdbuf();
    return bufferStream.str();
}

fs::path makeTempPath(const std::string& suffix) {
    static unsigned long counter = 0;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           fs::path("ext_temp_" + std::to_string(now) + "_" + std::to_string(++counter) + suffix);
}

std::string convertToTextFile(const std::string& commandPrefix, const fs::path& filePath,
                              const std::string& outputOption) {
    const fs::path tempTxt = makeTempPath(".txt");
    const std::string command = commandPrefix + " " + outputOption + " " +
                                quoteArg(pathToUtf8(tempTxt)) + " " + quoteArg(pathToUtf8(filePath));
    std::string content;
    std::error_code ec;
    if (runCommand(command) == 0 && fs::exists(tempTxt, ec)) content = readWholeFile(tempTxt);
    fs::remove(tempTxt, ec);
    return content;
}

std::string convertWithLibreOffice(const fs::path& filePath) {
    const fs::path tempDir = makeTempPath("");
    std::error_code ec;
    fs::create_directories(tempDir, ec);
    if (ec) return "";
    const std::string command = "soffice --headless --convert-to \"txt:Text (encoded):UTF8\" --outdir " +
                                quoteArg(pathToUtf8(tempDir)) + " " + quoteArg(pathToUtf8(filePath));
    std::string content;
    if (runCommand(command) == 0) {
        for (const auto& entry : fs::directory_iterator(tempDir, ec)) {
            if (entry.path().extension() == ".txt") {
                content = readWholeFile(entry.path());
                break;
            }
        }
    }
    fs::remove_all(tempDir, ec);
    return content;
}

std::string readFileContent(const fs::path& filePath, const std::string& ext) {
    if (ext == ".docx" || ext == ".doc" || ext == ".odt" || ext == ".rtf") {
#ifdef __APPLE__
        return convertToTextFile("textutil -convert txt -encoding utf-8", filePath, "-output");
#else
        std::string content;
        if (ext != ".doc") content = convertToTextFile("pandoc -t plain --wrap=none", filePath, "-o");
        if (content.empty()) content = convertWithLibreOffice(filePath);
        return content;
#endif
    }
    return readWholeFile(filePath);
}

std::string escapeCsvField(const std::string& field) {
    std::string escaped = "\"";
    for (std::size_t i = 0; i < field.size(); ++i) {
        const char c = field[i];
        if (c == '"') {
            escaped += "\"\"";
        } else if (c == '\r' || c == '\n') {
            escaped += " ";
        } else if (static_cast<unsigned char>(c) == 0xE2 && i + 2 < field.size() &&
                   static_cast<unsigned char>(field[i + 1]) == 0x80 &&
                   (static_cast<unsigned char>(field[i + 2]) == 0xA8 ||
                    static_cast<unsigned char>(field[i + 2]) == 0xA9)) {
            escaped += " ";
            i += 2;
        } else {
            escaped += c;
        }
    }
    escaped += "\"";
    return escaped;
}

std::string normalizeSpaces(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = true;
        } else {
            if (space && !out.empty()) out += ' ';
            out += c;
            space = false;
        }
    }
    return out;
}

} // namespace

void processDocuments(const fs::path& folderPath, const fs::path& outputDir) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    int epicrisisCount = 0;
    int fileIndex = 1;
    int skippedNoDiagnosis = 0;
    int skippedDuplicates = 0;
    std::unordered_set<std::string> seenHeaders;

    fs::path finalOutputDir = outputDir.empty() ? folderPath : outputDir;

    fs::create_directories(finalOutputDir);

    std::ofstream csvFile;

    auto openNewCsv = [&](int index) {
        if (csvFile.is_open()) csvFile.close();

        fs::path outFilePath = finalOutputDir / ("epicrisies_info_" + std::to_string(index) + ".csv");
        csvFile.open(outFilePath, std::ios::out | std::ios::binary);

        unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        csvFile.write(reinterpret_cast<char*>(bom), sizeof(bom));

        for (std::size_t i = 0; i < FIELDS.size(); ++i) {
            csvFile << "\"" << FIELDS[i] << "\",";
        }
        for (std::size_t i = 0; i < EXTRA_COLUMNS.size(); ++i) {
            csvFile << "\"" << EXTRA_COLUMNS[i] << "\"";
            if (i + 1 < EXTRA_COLUMNS.size()) csvFile << ",";
        }
        csvFile << "\n";
    };

    openNewCsv(fileIndex);

    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(folderPath)) {
        if (entry.is_regular_file()) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    for (const auto& path : files) {
        if (pathToUtf8(path.filename()).rfind(".", 0) == 0) continue;

        std::string ext = pathToUtf8(path.extension());
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (ext != ".txt" && ext != ".md" && ext != ".csv" && ext != ".docx" && ext != ".doc" &&
            ext != ".odt" && ext != ".rtf") {
            std::cerr << "Пропуснат (неподдържан формат): " << pathToUtf8(path.filename()) << "\n";
            continue;
        }

        const std::string filename = pathToUtf8(path.filename());
        if (filename.rfind("epicrisies_info_", 0) == 0 || filename.rfind("epicrisies_export_", 0) == 0) {
            continue;
        }
        if (filename.rfind("~$", 0) == 0) continue;

        std::string content = readFileContent(path, ext);
        if (content.empty()) {
            std::cerr << "ВНИМАНИЕ: не може да се прочете или е празен: " << filename << "\n";
            continue;
        }

        if (content.length() >= 3 &&
            static_cast<unsigned char>(content[0]) == 0xEF &&
            static_cast<unsigned char>(content[1]) == 0xBB &&
            static_cast<unsigned char>(content[2]) == 0xBF) {
            content.erase(0, 3);
        }

        const std::vector<TitlePos> positions = findEpicrisisPositions(content);
        if (positions.empty()) {
            std::cerr << "ВНИМАНИЕ: няма заглавие \"ЕПИКРИЗА\" в " << filename << "\n";
            continue;
        }

        for (std::size_t i = 0; i < positions.size(); ++i) {
            const std::size_t start = positions[i].end;
            const std::size_t stop = (i + 1 < positions.size()) ? positions[i + 1].start : content.length();
            const std::string singleEpicrisis = content.substr(start, stop - start);

            ParsedEpicrisis parsed = parseEpicrisis(singleEpicrisis);
            auto& parsedData = parsed.data;

            if (parsedData["Окончателна диагноза"].empty()) {
                ++skippedNoDiagnosis;
                std::cerr << "ВНИМАНИЕ: " << filename << ", епикриза № " << (i + 1)
                          << " - няма \"Окончателна диагноза\", пропусната\n";
                continue;
            }

            const std::string key = normalizeSpaces(parsed.header) + "|" +
                                    normalizeSpaces(parsedData["Окончателна диагноза"]);
            if (!parsed.header.empty() && !seenHeaders.insert(key).second) {
                ++skippedDuplicates;
                std::cerr << "Дубликат: " << filename << ", епикриза № " << (i + 1) << " - пропусната\n";
                continue;
            }

            if (epicrisisCount > 0 && epicrisisCount % 100 == 0) {
                fileIndex++;
                openNewCsv(fileIndex);
            }

            for (std::size_t j = 0; j < FIELDS.size(); ++j) {
                csvFile << escapeCsvField(parsedData[FIELDS[j]]) << ",";
            }
            csvFile << escapeCsvField(filename) << "," << escapeCsvField(std::to_string(i + 1)) << "\n";
            epicrisisCount++;
        }
    }

    if (csvFile.is_open()) {
        csvFile.close();
    }

    std::cerr << "Записани епикризи: " << epicrisisCount
              << "; пропуснати без диагноза: " << skippedNoDiagnosis
              << "; пропуснати дубликати: " << skippedDuplicates << "\n";
}

} // namespace extractor