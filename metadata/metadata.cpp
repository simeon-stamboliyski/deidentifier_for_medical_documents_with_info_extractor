#include "metadata.hpp"
#include <algorithm>
#include <cctype>
#include <regex>

namespace deid {

std::string MetadataProcessor::stripMetadata(const std::string& text, const std::filesystem::path& path) {
    std::string result = text;
    
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (ext == ".md" || ext == ".txt") {
        // Strip YAML frontmatter at document start
        static const std::regex yamlFrontmatter(R"(^---\s*[\s\S]*?---\s*)");
        result = std::regex_replace(result, yamlFrontmatter, "");

        // Strip HTML comments (multiline safe)
        static const std::regex htmlComments(R"(<!--[\s\S]*?-->)");
        result = std::regex_replace(result, htmlComments, "");

        // Match English and Bulgarian metadata header lines anywhere in the document:
        // Works safely without Perl inline (?i) flags or multiline mode.
        static const std::regex metadataLines(
            R"((?:^|\r?\n)(?:Author|Created|Revision|Owner|Автор|Създаден|Версия|Оператор)\s*:\s*[^\r\n]*)",
            std::regex::icase
        );
        result = std::regex_replace(result, metadataLines, "");
    }

    return result;
}

} // namespace deid