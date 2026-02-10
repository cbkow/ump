#include "markdown_parser.h"
#include <sstream>
#include <vector>
#include <regex>

namespace ump {
namespace Annotations {

// Apply inline formatting: bold/italic combo, bold, italic, code
static std::string ProcessInlineFormatting(const std::string& text) {
    std::string result = text;

    // `code` - process first to protect contents from further formatting
    // We'll do a manual pass for code spans to avoid nested formatting inside them
    std::string code_processed;
    size_t pos = 0;
    while (pos < result.size()) {
        size_t tick = result.find('`', pos);
        if (tick == std::string::npos) {
            code_processed += result.substr(pos);
            break;
        }
        code_processed += result.substr(pos, tick - pos);
        size_t end_tick = result.find('`', tick + 1);
        if (end_tick == std::string::npos) {
            // No closing backtick, keep literal
            code_processed += result.substr(tick);
            break;
        }
        code_processed += "<code>" + result.substr(tick + 1, end_tick - tick - 1) + "</code>";
        pos = end_tick + 1;
    }
    result = code_processed;

    // ***text*** or ___text___ -> <strong><em>text</em></strong>
    result = std::regex_replace(result, std::regex(R"(\*\*\*(.+?)\*\*\*)"), "<strong><em>$1</em></strong>");
    result = std::regex_replace(result, std::regex(R"(___(.+?)___)"), "<strong><em>$1</em></strong>");

    // **text** or __text__ -> <strong>text</strong>
    result = std::regex_replace(result, std::regex(R"(\*\*(.+?)\*\*)"), "<strong>$1</strong>");
    result = std::regex_replace(result, std::regex(R"(__(.+?)__)"), "<strong>$1</strong>");

    // *text* or _text_ -> <em>text</em>
    result = std::regex_replace(result, std::regex(R"(\*(.+?)\*)"), "<em>$1</em>");
    result = std::regex_replace(result, std::regex(R"(\b_(.+?)_\b)"), "<em>$1</em>");

    return result;
}

// Check if a line is an ordered list item (e.g. "1. text")
static bool IsOrderedListItem(const std::string& line, std::string& content) {
    size_t i = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
        i++;
    }
    if (i > 0 && i < line.size() && line[i] == '.' && i + 1 < line.size() && line[i + 1] == ' ') {
        content = line.substr(i + 2);
        return true;
    }
    return false;
}

// Split text into lines, stripping \r
static std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::string MarkdownToHtml(const std::string& text) {
    if (text.empty()) return "";

    auto lines = SplitLines(text);

    std::ostringstream html;

    enum class BlockState { None, Paragraph, UnorderedList, OrderedList };
    BlockState state = BlockState::None;
    bool first_para_line = true;

    auto close_block = [&]() {
        switch (state) {
            case BlockState::Paragraph:
                html << "</p>\n";
                break;
            case BlockState::UnorderedList:
                html << "</ul>\n";
                break;
            case BlockState::OrderedList:
                html << "</ol>\n";
                break;
            default:
                break;
        }
        state = BlockState::None;
        first_para_line = true;
    };

    for (size_t i = 0; i < lines.size(); i++) {
        const std::string& ln = lines[i];

        // Blank line - close current block
        if (ln.empty()) {
            close_block();
            continue;
        }

        // Headings: # -> h4, ## -> h5, ### - ###### -> h6 (downsized since notes sit under h3)
        if (ln.size() >= 2 && ln[0] == '#') {
            int level = 0;
            while (level < (int)ln.size() && ln[level] == '#') level++;
            if (level <= 6 && level < (int)ln.size() && ln[level] == ' ') {
                close_block();
                int html_level = level + 3; // # -> h4, ## -> h5, ### -> h6
                if (html_level > 6) html_level = 6;
                std::string heading_text = ln.substr(level + 1);
                html << "<h" << html_level << ">" << ProcessInlineFormatting(heading_text) << "</h" << html_level << ">\n";
                continue;
            }
            // Not a valid heading syntax, fall through to paragraph
        }

        // Unordered list: - or *
        if (ln.size() >= 2 && (ln[0] == '-' || ln[0] == '*') && ln[1] == ' ') {
            if (state != BlockState::UnorderedList) {
                close_block();
                html << "<ul>\n";
                state = BlockState::UnorderedList;
            }
            std::string item_text = ln.substr(2);
            html << "<li>" << ProcessInlineFormatting(item_text) << "</li>\n";
            continue;
        }

        // Ordered list: N.
        std::string ol_content;
        if (IsOrderedListItem(ln, ol_content)) {
            if (state != BlockState::OrderedList) {
                close_block();
                html << "<ol>\n";
                state = BlockState::OrderedList;
            }
            html << "<li>" << ProcessInlineFormatting(ol_content) << "</li>\n";
            continue;
        }

        // Regular text - paragraph
        if (state != BlockState::Paragraph) {
            close_block();
            html << "<p>";
            state = BlockState::Paragraph;
            first_para_line = true;
        }

        if (!first_para_line) {
            html << "<br>";
        }
        html << ProcessInlineFormatting(ln);
        first_para_line = false;
    }

    close_block();
    return html.str();
}

std::string StripInlineFormatting(const std::string& text) {
    std::string result = text;

    // Strip backtick code spans (keep inner text)
    std::string code_stripped;
    size_t pos = 0;
    while (pos < result.size()) {
        size_t tick = result.find('`', pos);
        if (tick == std::string::npos) {
            code_stripped += result.substr(pos);
            break;
        }
        code_stripped += result.substr(pos, tick - pos);
        size_t end_tick = result.find('`', tick + 1);
        if (end_tick == std::string::npos) {
            code_stripped += result.substr(tick + 1); // skip orphan backtick
            break;
        }
        code_stripped += result.substr(tick + 1, end_tick - tick - 1);
        pos = end_tick + 1;
    }
    result = code_stripped;

    // Strip bold+italic, bold, italic markers (keep inner text)
    result = std::regex_replace(result, std::regex(R"(\*\*\*(.+?)\*\*\*)"), "$1");
    result = std::regex_replace(result, std::regex(R"(___(.+?)___)"), "$1");
    result = std::regex_replace(result, std::regex(R"(\*\*(.+?)\*\*)"), "$1");
    result = std::regex_replace(result, std::regex(R"(__(.+?)__)"), "$1");
    result = std::regex_replace(result, std::regex(R"(\*(.+?)\*)"), "$1");
    result = std::regex_replace(result, std::regex(R"(\b_(.+?)_\b)"), "$1");

    return result;
}

std::vector<MarkdownBlock> ParseMarkdownBlocks(const std::string& text) {
    std::vector<MarkdownBlock> blocks;
    if (text.empty()) return blocks;

    auto lines = SplitLines(text);
    std::string para_accum; // accumulates paragraph lines

    auto flush_paragraph = [&]() {
        if (!para_accum.empty()) {
            blocks.push_back({MarkdownBlock::Type::Paragraph, 0,
                              StripInlineFormatting(para_accum),
                              StripInlineFormattingKeepCode(para_accum)});
            para_accum.clear();
        }
    };

    for (const auto& ln : lines) {
        // Blank line - flush paragraph
        if (ln.empty()) {
            flush_paragraph();
            continue;
        }

        // Headings: # through ######
        if (ln.size() >= 2 && ln[0] == '#') {
            int level = 0;
            while (level < (int)ln.size() && ln[level] == '#') level++;
            if (level <= 6 && level < (int)ln.size() && ln[level] == ' ') {
                flush_paragraph();
                std::string heading_text = ln.substr(level + 1);
                blocks.push_back({MarkdownBlock::Type::Heading, level,
                                  StripInlineFormatting(heading_text),
                                  StripInlineFormattingKeepCode(heading_text)});
                continue;
            }
        }

        // Unordered list: - or *
        if (ln.size() >= 2 && (ln[0] == '-' || ln[0] == '*') && ln[1] == ' ') {
            flush_paragraph();
            std::string item_text = ln.substr(2);
            blocks.push_back({MarkdownBlock::Type::UnorderedListItem, 0,
                              StripInlineFormatting(item_text),
                              StripInlineFormattingKeepCode(item_text)});
            continue;
        }

        // Ordered list: N.
        std::string ol_content;
        if (IsOrderedListItem(ln, ol_content)) {
            flush_paragraph();
            blocks.push_back({MarkdownBlock::Type::OrderedListItem, 0,
                              StripInlineFormatting(ol_content),
                              StripInlineFormattingKeepCode(ol_content)});
            continue;
        }

        // Regular text - accumulate into paragraph (space-separated for word wrap)
        if (!para_accum.empty()) para_accum += ' ';
        para_accum += ln;
    }

    flush_paragraph();
    return blocks;
}

std::string StripInlineFormattingKeepCode(const std::string& text) {
    std::string result = text;

    // Strip bold+italic, bold, italic markers (keep inner text) but leave backticks alone
    result = std::regex_replace(result, std::regex(R"(\*\*\*(.+?)\*\*\*)"), "$1");
    result = std::regex_replace(result, std::regex(R"(___(.+?)___)"), "$1");
    result = std::regex_replace(result, std::regex(R"(\*\*(.+?)\*\*)"), "$1");
    result = std::regex_replace(result, std::regex(R"(__(.+?)__)"), "$1");
    result = std::regex_replace(result, std::regex(R"(\*(.+?)\*)"), "$1");
    result = std::regex_replace(result, std::regex(R"(\b_(.+?)_\b)"), "$1");

    return result;
}

std::vector<InlineSegment> ParseInlineCodeSegments(const std::string& text) {
    std::vector<InlineSegment> segments;
    size_t pos = 0;

    while (pos < text.size()) {
        size_t tick = text.find('`', pos);
        if (tick == std::string::npos) {
            // No more backticks — rest is regular text
            std::string rest = text.substr(pos);
            if (!rest.empty()) {
                segments.push_back({rest, false});
            }
            break;
        }

        // Add regular text before the backtick
        if (tick > pos) {
            segments.push_back({text.substr(pos, tick - pos), false});
        }

        // Find closing backtick
        size_t end_tick = text.find('`', tick + 1);
        if (end_tick == std::string::npos) {
            // No closing backtick — treat rest as regular text
            segments.push_back({text.substr(tick + 1), false});
            break;
        }

        // Code span
        std::string code_text = text.substr(tick + 1, end_tick - tick - 1);
        if (!code_text.empty()) {
            segments.push_back({code_text, true});
        }
        pos = end_tick + 1;
    }

    return segments;
}

std::string EscapeForMarkdownTable(const std::string& text) {
    std::string result;
    result.reserve(text.size());

    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (c == '\n') {
            result += "<br>";
        } else if (c == '\r') {
            // Skip \r (will be followed by \n on Windows)
            continue;
        } else if (c == '|') {
            result += "\\|";
        } else if (c == '#' && (i == 0 || text[i - 1] == '\n')) {
            // Strip leading # header markers at start of line
            while (i < text.size() && text[i] == '#') i++;
            // Skip the space after #
            if (i < text.size() && text[i] == ' ') i++;
            i--; // Will be incremented by for loop
        } else {
            result += c;
        }
    }

    return result;
}

} // namespace Annotations
} // namespace ump
