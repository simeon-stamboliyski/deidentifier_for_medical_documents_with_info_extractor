#include "deidentifier.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace deid {

namespace {

std::string toLowerUtf8(const std::string& str) {
    std::string result;
    result.reserve(str.size());

    for (std::size_t i = 0; i < str.size(); ++i) {
        const auto c1 = static_cast<unsigned char>(str[i]);

        if (c1 == 0xD0 && i + 1 < str.size()) {
            const auto c2 =
                static_cast<unsigned char>(str[i + 1]);

            if (c2 == 0x81) { // Ё -> ё
                result += "\xD1\x91";
                ++i;
                continue;
            }

            if (c2 >= 0x90 && c2 <= 0x9F) {
                // А-П -> а-п
                result += static_cast<char>(0xD0);
                result += static_cast<char>(c2 + 0x20);
                ++i;
                continue;
            }

            if (c2 >= 0xA0 && c2 <= 0xAF) {
                // Р-Я -> р-я
                result += static_cast<char>(0xD1);
                result += static_cast<char>(c2 - 0x20);
                ++i;
                continue;
            }
        }

        if (c1 < 128)
            result += static_cast<char>(std::tolower(c1));
        else
            result += str[i];
    }

    return result;
}

std::string entityTypeToString(EntityType type) {
    switch (type) {
    case EntityType::Email:        return "EMAIL";
    case EntityType::Phone:        return "PHONE";
    case EntityType::IPAddress:    return "IP";
    case EntityType::IDNumber:     return "ID";
    case EntityType::Date:         return "DATE";
    case EntityType::URLToken:     return "TOKEN";
    case EntityType::Person:       return "PERSON";
    case EntityType::Location:     return "LOCATION";
    case EntityType::Organization: return "ORG";
    }
    return "ENTITY";
}

// Strict UTF-8 validator.
bool validUtf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c =
            static_cast<unsigned char>(s[i]);

        std::size_t length = 0;

        if (c <= 0x7F) length = 1;
        else if (c >= 0xC2 && c <= 0xDF) length = 2;
        else if (c >= 0xE0 && c <= 0xEF) length = 3;
        else if (c >= 0xF0 && c <= 0xF4) length = 4;
        else return false;

        if (length > s.size() - i)
            return false;

        for (std::size_t j = 1; j < length; ++j) {
            if ((static_cast<unsigned char>(s[i + j])
                 & 0xC0) != 0x80)
                return false;
        }

        if (length == 3) {
            const auto c2 =
                static_cast<unsigned char>(s[i + 1]);

            if ((c == 0xE0 && c2 < 0xA0) ||
                (c == 0xED && c2 >= 0xA0))
                return false;
        }

        if (length == 4) {
            const auto c2 =
                static_cast<unsigned char>(s[i + 1]);

            if ((c == 0xF0 && c2 < 0x90) ||
                (c == 0xF4 && c2 > 0x8F))
                return false;
        }

        i += length;
    }

    return true;
}

// Ensure a replacement does not begin or end
// in the middle of a UTF-8 character.
bool utf8Boundary(
    const std::string& s,
    std::size_t position)
{
    return position == s.size() ||
        (static_cast<unsigned char>(s[position])
         & 0xC0) != 0x80;
}

// Translate an index in Unicode code points into a UTF-8 byte
// offset. This is valid only when the detector counts code points
// (not UTF-16 code units or words). The source text must be valid UTF-8.
bool codepointIndexToByteOffset(
    const std::string& text,
    std::size_t codepointIndex,
    std::size_t& byteOffset)
{
    byteOffset = 0;
    std::size_t index = 0;

    while (index < codepointIndex && byteOffset < text.size()) {
        const auto first =
            static_cast<unsigned char>(text[byteOffset]);

        if (first <= 0x7F) byteOffset += 1;
        else if (first <= 0xDF) byteOffset += 2;
        else if (first <= 0xEF) byteOffset += 3;
        else byteOffset += 4;

        ++index;
    }

    return index == codepointIndex;
}

// Verify that a detected entity actually matches the original
// UTF-8 input at a byte offset, without reading outside the string.
bool matchesByteSpan(
    const std::string& text,
    std::size_t start,
    const std::string& needle)
{
    return !needle.empty() &&
           start <= text.size() &&
           needle.size() <= text.size() - start &&
           utf8Boundary(text, start) &&
           utf8Boundary(text, start + needle.size()) &&
           text.compare(start, needle.size(), needle) == 0;
}

std::size_t countPattern(
    const std::string& s,
    const std::string& pattern)
{
    std::size_t count = 0;
    std::size_t pos = 0;

    while ((pos = s.find(pattern, pos)) !=
           std::string::npos) {
        ++count;
        pos += pattern.size();
    }

    return count;
}

// True if the UTF-8 text includes characters in the
// Cyrillic block U+0400-U+04FF (including Bulgarian).
// Called only after validUtf8(text) succeeds.
bool containsCyrillic(const std::string& text) {
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        const auto first =
            static_cast<unsigned char>(text[i]);
        const auto second =
            static_cast<unsigned char>(text[i + 1]);

        // U+0400-U+04FF encodes as D0..D3 + continuation.
        if (first >= 0xD0 && first <= 0xD3 &&
            (second & 0xC0) == 0x80) {
            return true;
        }
    }
    return false;
}

// Cyrillic-specific mojibake *heuristic*.
// Examples: София -> Ð¡Ð¾Ñ„Ð¸Ñ or РЎРѕС„РёСЏ.
// Look for NEW suspicious sequences; don't fail merely
// because an existing input already contains one.
bool introducesCyrillicMojibake(
    const std::string& before,
    const std::string& after)
{
    // Keep this source file encoded in UTF-8.
    // Ð / Ñ: UTF-8 lead bytes for Cyrillic misread as
    //         Latin-1/Windows-1252.
    // Р... / С...: UTF-8 Cyrillic misread as Windows-1251.
    const std::array<std::string, 17> patterns = {
        "\xEF\xBF\xBD", // replacement character U+FFFD
        "Ð", "Ñ",
        "Рџ", "РЎ", "Рѕ", "Рё", "Рµ", "Р°", "РІ",
        "СЂ", "С‚", "С„", "СЏ", "Рђ", "Рґ", "Сѓ"
    };

    for (const auto& pattern : patterns) {
        if (countPattern(after, pattern) >
            countPattern(before, pattern)) {
            return true;
        }
    }
    return false;
}

void writeDebug(
    const std::string& filename,
    const std::string& content)
{
#ifdef DEID_DEBUG_DUMPS
    std::ofstream out(filename, std::ios::binary);

    if (!out)
        throw std::runtime_error(
            "Cannot open debug file: " + filename);

    out.write(
        content.data(),
        static_cast<std::streamsize>(content.size()));
#else
    (void)filename;
    (void)content;
#endif
}

} // namespace

DeidentificationResult Deidentifier::deidentifyText(
    const std::string& text)
{
    if (!validUtf8(text)) {
        throw std::runtime_error(
            "Input is not valid UTF-8");
    }

    // Decide once whether Cyrillic mojibake checks apply.
    const bool cyrillicInput = containsCyrillic(text);

    // Detect all entities ONCE.
    auto detected = detector_.detectAll(text);

    // Convert detector offsets to verified UTF-8 byte spans.
    // Detectors may count Unicode code points instead of bytes,
    // particularly when Bulgarian/Cyrillic text precedes an ID.
    // Every corrected span is checked against the original input.
    for (auto& e : detected) {
        const bool byteMatch =
            matchesByteSpan(text, e.start, e.text);

        std::size_t convertedStart = 0;
        const bool cpOffsetExists =
            codepointIndexToByteOffset(text, e.start, convertedStart);
        const bool codepointMatch = cpOffsetExists &&
            matchesByteSpan(text, convertedStart, e.text);

        if (!byteMatch && codepointMatch) {
            std::cerr
                << "[OFFSET FIX] " << entityTypeToString(e.type)
                << ": codepoint offset " << e.start
                << " -> UTF-8 byte offset " << convertedStart
                << '\n';
            e.start = convertedStart;
        } else if (!byteMatch) {
            // Diagnostic metadata only; do NOT print e.text,
            // because identifiers may be sensitive.
            const std::size_t firstMatch = text.find(e.text);
            const bool existsElsewhere =
                !e.text.empty() && firstMatch != std::string::npos;

            std::cerr
                << "[INVALID SPAN] type="
                << entityTypeToString(e.type)
                << " start=" << e.start
                << " reported_length=" << e.length
                << " actual_utf8_bytes=" << e.text.size()
                << " input_bytes=" << text.size()
                << " exact_text_elsewhere="
                << (existsElsewhere ? "yes" : "no")
                << '\n';

            throw std::runtime_error(
                "Detector returned an invalid byte span for " +
                entityTypeToString(e.type));
        }

        // The validated text determines the correct byte length.
        // Some detectors report e.length in Unicode characters.
        if (e.length != e.text.size()) {
            std::cerr
                << "[LENGTH FIX] " << entityTypeToString(e.type)
                << ": reported length " << e.length
                << " -> UTF-8 byte length " << e.text.size()
                << '\n';
            e.length = e.text.size();
        }
    }

    // Prevent conflicting replacements.
    const auto entities =
        resolveOverlaps(std::move(detected));

    const std::array<EntityType, 9> order = {
        EntityType::Email,
        EntityType::Phone,
        EntityType::IPAddress,
        EntityType::IDNumber,
        EntityType::Date,
        EntityType::URLToken,
        EntityType::Person,
        EntityType::Location,
        EntityType::Organization
    };

    struct Replacement {
        std::size_t start;
        std::size_t length;
        std::string pseudonym;
    };

    std::vector<Replacement> committed;
    std::string currentText = text;

    // Rebuild every intermediate result from the
    // original text, preserving original offsets.
    auto render = [&](std::vector<Replacement> replacements) {
        std::sort(
            replacements.begin(),
            replacements.end(),
            [](const Replacement& a, const Replacement& b) {
                return a.start < b.start;
            });

        std::string output;
        std::size_t cursor = 0;

        for (const auto& r : replacements) {
            if (r.start < cursor ||
                r.start > text.size() ||
                r.length > text.size() - r.start) {
                throw std::runtime_error(
                    "Overlapping or invalid replacement");
            }

            output.append(
                text,
                cursor,
                r.start - cursor);

            output += r.pseudonym;
            cursor = r.start + r.length;
        }

        output.append(text, cursor, std::string::npos);

        return output;
    };

    // Preserve pseudonym state if processing fails.
    const auto oldPseudonyms = pseudonyms_;

    try {
        // Process exactly ONE TYPE at a time.
        for (EntityType type : order) {
            const std::string name =
                entityTypeToString(type);

            std::cout << "[START] " << name << '\n';

            // Stage previously accepted replacements.
            auto proposed = committed;

            // Add replacements belonging to this type.
            for (const auto& e : entities) {
                if (e.type != type)
                    continue;

                proposed.push_back({
                    e.start,
                    e.length,
                    getPseudonym(e.text, e.type)
                });
            }

            // Build candidate WITHOUT committing it.
            std::string candidate = render(proposed);

            // Check the entire candidate after this type.
            const bool utf8OK = validUtf8(candidate);

            // Only run Cyrillic mojibake heuristics when
            // the original input contains Cyrillic text.
            // UTF-8 validity is checked for ALL input.
            const bool mojibake =
                cyrillicInput &&
                introducesCyrillicMojibake(
                    currentText,
                    candidate);

            if (!utf8OK || mojibake) {
                writeDebug(
                    "debug_FAILED_" + name + ".txt",
                    candidate);

                std::cerr
                    << "[FAILED] " << name
                    << " | Valid UTF-8: " << utf8OK
                    << " | New Cyrillic mojibake: " << mojibake
                    << '\n';

                // Stop rather than return text containing
                // identifiers that might not be redacted.
                throw std::runtime_error(
                    "Deidentification failed at type: " +
                    name);
            }

            // Passed validation: commit this type.
            committed = std::move(proposed);
            currentText = std::move(candidate);

            writeDebug(
                "debug_after_" + name + ".txt",
                currentText);

            std::cout
                << "[OK] " << name
                << " | Total redactions: "
                << committed.size()
                << '\n';
        }
    }
    catch (...) {
        pseudonyms_ = oldPseudonyms;
        throw;
    }

    DeidentificationResult result;
    result.text = std::move(currentText);
    result.redactions = committed.size();

    return result;
}

std::vector<DetectedEntity> Deidentifier::resolveOverlaps(
    std::vector<DetectedEntity> entities) const
{
    if (entities.empty())
        return {};

    std::sort(
        entities.begin(),
        entities.end(),
        [](const DetectedEntity& a,
           const DetectedEntity& b) {
            if (a.start != b.start)
                return a.start < b.start;

            if (a.score != b.score)
                return a.score > b.score;

            return a.length > b.length;
        });

    std::vector<DetectedEntity> resolved;
    std::size_t lastEnd = 0;

    for (const auto& entity : entities) {
        if (entity.start >= lastEnd) {
            resolved.push_back(entity);
            lastEnd = entity.start + entity.length;
        }
    }

    return resolved;
}

std::string Deidentifier::getPseudonym(
    const std::string& entityText,
    EntityType type)
{
    // Include type in the key to avoid collisions.
    const std::string key =
        entityTypeToString(type) + ":" +
        toLowerUtf8(entityText);

    const auto existing = pseudonyms_.find(key);

    if (existing != pseudonyms_.end())
        return existing->second;

    const std::string prefix =
        entityTypeToString(type);

    const std::string pseudonym =
        "[" + prefix + "_" +
        std::to_string(pseudonyms_.size() + 1) + "]";

    pseudonyms_[key] = pseudonym;

    return pseudonym;
}

} // namespace deid
