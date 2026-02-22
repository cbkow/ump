#pragma once

#include <string>
#include <vector>

namespace qcview {
namespace Annotations {

// Convert basic markdown to HTML.
// Supports: # headings, **bold**, *italic*, `code`,
// unordered lists (- / *), ordered lists (1.), blank-line paragraphs.
std::string MarkdownToHtml(const std::string& text);

// Escape note text for safe use inside a markdown table cell.
// Replaces newlines with <br>, pipes with \|.
std::string EscapeForMarkdownTable(const std::string& text);

// Structured block for PDF and other plain-text renderers.
struct MarkdownBlock {
    enum class Type { Heading, Paragraph, UnorderedListItem, OrderedListItem };
    Type type;
    int heading_level; // 1-6 for headings, 0 otherwise
    std::string text;      // plain text with inline markers stripped
    std::string raw_text;  // text with backtick markers preserved (bold/italic stripped)
};

// Parse markdown into blocks with inline formatting stripped.
// Each block carries its type so renderers can choose font/indent/spacing.
std::vector<MarkdownBlock> ParseMarkdownBlocks(const std::string& text);

// Strip inline markdown markers (**bold** -> bold, *italic* -> italic, `code` -> code).
std::string StripInlineFormatting(const std::string& text);

// Strip bold/italic markers but preserve backtick code markers.
std::string StripInlineFormattingKeepCode(const std::string& text);

// Inline text segment for rendering with mixed fonts (code vs regular).
struct InlineSegment {
    std::string text;
    bool is_code = false;
};

// Parse text into code/non-code segments based on backtick markers.
std::vector<InlineSegment> ParseInlineCodeSegments(const std::string& text);

} // namespace Annotations
} // namespace qcview
