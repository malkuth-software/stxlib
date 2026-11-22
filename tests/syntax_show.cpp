// syntax_show: renders a source file as highlighted HTML with stxlib::syntax, and reports
// the engine's speed. A hand tool and the picture for reviews, not a ctest.
//
//   syntax_show FILE [LANGUAGE] > out.html     the page; speed on stderr
//
// Speed is measured two ways: the document's reset (every line's exit state, what opening a
// file costs) and a full pass that produces every token (what painting every line costs).

#include "stxlib/syntax.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace {

namespace syntax = stxlib::syntax;
using stxlib::usize;

const char* colour_of(const syntax::TokenKind kind) {
    switch (kind) {
    case syntax::TokenKind::comment: return "#7f8c98";
    case syntax::TokenKind::documentation: return "#8fa98f";
    case syntax::TokenKind::string: return "#e6a06a";
    case syntax::TokenKind::character: return "#e6a06a";
    case syntax::TokenKind::number: return "#d6c46a";
    case syntax::TokenKind::keyword: return "#e07aa8";
    case syntax::TokenKind::control: return "#c58ae0";
    case syntax::TokenKind::type: return "#6fc7d6";
    case syntax::TokenKind::constant: return "#d98c6a";
    case syntax::TokenKind::function: return "#7fb4f0";
    case syntax::TokenKind::macro: return "#f0b27f";
    case syntax::TokenKind::directive: return "#f08f7f";
    case syntax::TokenKind::attribute: return "#c9b07a";
    case syntax::TokenKind::property: return "#8fc7a0";
    case syntax::TokenKind::label: return "#c9b07a";
    case syntax::TokenKind::operator_symbol: return "#b8c0cc";
    case syntax::TokenKind::punctuation: return "#9aa3ae";
    case syntax::TokenKind::identifier: return "#dfe3e8";
    case syntax::TokenKind::invalid: return "#ff5f5f";
    case syntax::TokenKind::plain: return "#dfe3e8";
    }
    return "#dfe3e8";
}

void escaped(const char* const text, const usize count) {
    for (usize k = 0; k < count; ++k) {
        const char byte = text[k];
        if (byte == '<') {
            std::fputs("&lt;", stdout);
        } else if (byte == '>') {
            std::fputs("&gt;", stdout);
        } else if (byte == '&') {
            std::fputs("&amp;", stdout);
        } else {
            std::fputc(byte, stdout);
        }
    }
}

double seconds_since(const std::chrono::steady_clock::time_point start) {
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    return elapsed.count();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs("usage: syntax_show FILE [LANGUAGE]\n", stderr);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string file_name(argv[1]);
    syntax::Language language = syntax::language_for_file_name(stxlib::View<char>(file_name.data(), file_name.size()));
    if (argc >= 3) {
        const std::string name(argv[2]);
        language = syntax::language_named(stxlib::View<char>(name.data(), name.size()));
    }
    const stxlib::View<char> view(text.data(), text.size());
    stxlib::OperationResult<syntax::Document> made = syntax::Document::create(language);
    if (made.failed()) return 1;
    syntax::Document document = static_cast<stxlib::OperationResult<syntax::Document>&&>(made).take_value();

    // Speed: repeat until a tenth of a second has passed.
    usize resets = 0;
    const std::chrono::steady_clock::time_point reset_start = std::chrono::steady_clock::now();
    do {
        if (document.reset(view).failed()) return 1;
        resets = resets + 1;
    } while (seconds_since(reset_start) < 0.1);
    const double reset_seconds = seconds_since(reset_start) / static_cast<double>(resets);
    usize passes = 0;
    usize token_count = 0;
    const std::chrono::steady_clock::time_point token_start = std::chrono::steady_clock::now();
    do {
        token_count = 0;
        for (stxlib::u64 line = 0; line < document.line_count(); ++line) {
            const syntax::LineExtent extent = document.line_extent(view, line);
            syntax::LineCursor cursor = document.line_cursor(line);
            syntax::LineToken tokens[64];
            while (!cursor.done) {
                token_count = token_count + syntax::lex_line(language, stxlib::View<char>(text.data() + extent.start, extent.length - extent.terminator),
                                                             cursor, stxlib::MutableView<syntax::LineToken>(tokens, 64));
            }
        }
        passes = passes + 1;
    } while (seconds_since(token_start) < 0.1);
    const double token_seconds = seconds_since(token_start) / static_cast<double>(passes);
    const double megabytes = static_cast<double>(text.size()) / 1e6;
    const stxlib::TextView display = syntax::language_display_name(language);
    std::fprintf(stderr,
                 "%s: %.*s, %zu bytes, %llu lines, %zu tokens | reset %.3f ms (%.0f MB/s) | all tokens %.3f ms (%.0f MB/s, %.1f ns/token) | "
                 "state %zu bytes (%.1f a line)\n",
                 argv[1], static_cast<int>(display.count()), display.data(), text.size(), static_cast<unsigned long long>(document.line_count()),
                 token_count, reset_seconds * 1e3, megabytes / reset_seconds, token_seconds * 1e3, megabytes / token_seconds,
                 token_seconds * 1e9 / static_cast<double>(token_count == 0 ? 1 : token_count), document.storage_bytes(),
                 static_cast<double>(document.storage_bytes()) / static_cast<double>(document.line_count()));

    std::fputs("<!doctype html><meta charset=\"utf-8\"><title>stxlib::syntax</title>"
               "<body style=\"background:#1b1f24;color:#dfe3e8;margin:24px\">"
               "<pre style=\"font:13px/1.45 ui-monospace,Menlo,Consolas,monospace\">",
               stdout);
    for (stxlib::u64 line = 0; line < document.line_count(); ++line) {
        const syntax::LineExtent extent = document.line_extent(view, line);
        const char* const bytes = text.data() + extent.start;
        const usize length = extent.length - extent.terminator;
        syntax::LineCursor cursor = document.line_cursor(line);
        syntax::LineToken tokens[64];
        usize drawn = 0;
        while (!cursor.done) {
            const usize written = syntax::lex_line(language, stxlib::View<char>(bytes, length), cursor, stxlib::MutableView<syntax::LineToken>(tokens, 64));
            for (usize k = 0; k < written; ++k) {
                escaped(bytes + drawn, tokens[k].column - drawn);
                std::printf("<span style=\"color:%s\">", colour_of(tokens[k].kind));
                escaped(bytes + tokens[k].column, tokens[k].length);
                std::fputs("</span>", stdout);
                drawn = tokens[k].column + tokens[k].length;
            }
        }
        escaped(bytes + drawn, length - drawn);
        std::fputc('\n', stdout);
    }
    std::fputs("</pre></body>\n", stdout);
    return 0;
}
