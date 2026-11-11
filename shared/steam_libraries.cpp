// SPDX-License-Identifier: GPL-2.0-or-later

#include "steam_libraries.h"

#include "ascii_case.h"

#include <algorithm>
#include <format>

namespace wallpaperx::steam {

namespace {

// block() recurses once per level; this bounds the stack on a malformed file.
constexpr int kMaxDepth = 32;

class Parser {
public:
    explicit Parser(std::string_view text) : m_text(text) {}

    std::expected<std::vector<KeyValue>, std::string> document() { return block(0); }

private:
    enum class Kind { End, Open, Close, Word };

    struct Token {
        Kind kind = Kind::End;
        std::string word;
    };

    std::unexpected<std::string> fail(std::string_view what) const {
        return std::unexpected(std::format("{} {} (at byte {}).", kLibraryListName, what, m_at));
    }

    void skipBlank() {
        while (m_at < m_text.size()) {
            const char c = m_text[m_at];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++m_at;
            } else if (m_text.substr(m_at, 2) == "//") {
                const std::size_t end = m_text.find('\n', m_at);
                m_at = end == std::string_view::npos ? m_text.size() : end;
            } else {
                return;
            }
        }
    }

    std::expected<Token, std::string> next() {
        skipBlank();
        if (m_at >= m_text.size()) return Token{};
        const char c = m_text[m_at];
        if (c == '{' || c == '}') {
            ++m_at;
            return Token{c == '{' ? Kind::Open : Kind::Close, {}};
        }
        Token token{Kind::Word, {}};
        if (c != '"') {
            while (m_at < m_text.size()) {
                const char w = m_text[m_at];
                if (w == ' ' || w == '\t' || w == '\r' || w == '\n' || w == '{' || w == '}' || w == '"') break;
                token.word += w;
                ++m_at;
            }
            return token;
        }
        const std::size_t opened = m_at++;
        while (m_at < m_text.size() && m_text[m_at] != '"') {
            char w = m_text[m_at++];
            if (w == '\\' && m_at < m_text.size()) {
                switch (const char e = m_text[m_at++]) {
                case '\\':
                case '"':
                    w = e;
                    break;
                case 'n':
                    w = '\n';
                    break;
                case 't':
                    w = '\t';
                    break;
                default:
                    token.word += '\\';
                    w = e;
                }
            }
            token.word += w;
        }
        if (m_at >= m_text.size()) {
            m_at = opened;
            return fail("has a quoted string that never ends");
        }
        ++m_at;
        return token;
    }

    // Up to the "}" closing it, or the end of the text at depth 0.
    std::expected<std::vector<KeyValue>, std::string> block(int depth) {
        std::vector<KeyValue> entries;
        for (;;) {
            auto key = next();
            if (!key) return std::unexpected(key.error());
            if (key->kind == Kind::End) {
                if (depth > 0) return fail("ends inside a block");
                return entries;
            }
            if (key->kind == Kind::Close) {
                if (depth == 0) return fail("has a \"}\" with no block open");
                return entries;
            }
            if (key->kind == Kind::Open) return fail("has a block with no key");
            auto value = next();
            if (!value) return std::unexpected(value.error());
            if (value->kind == Kind::Word) {
                entries.push_back(KeyValue{std::move(key->word), std::move(value->word)});
            } else if (value->kind == Kind::Open) {
                if (depth + 1 > kMaxDepth) return fail(std::format("nests more than {} blocks deep", kMaxDepth));
                auto inner = block(depth + 1);
                if (!inner) return inner;
                entries.push_back(KeyValue{std::move(key->word), std::move(*inner)});
            } else {
                return fail("has a key with no value");
            }
        }
    }

    std::string_view m_text;
    std::size_t m_at = 0;
};

} // namespace

std::expected<std::vector<KeyValue>, std::string> parseKeyValues(std::string_view text) {
    return Parser(text).document();
}

std::expected<std::vector<std::string>, std::string> libraryFolders(std::string_view vdf) {
    auto document = parseKeyValues(vdf);
    if (!document) return std::unexpected(std::move(document.error()));
    using Block = std::vector<KeyValue>;
    const auto top = std::ranges::find_if(*document, [](const KeyValue& kv) {
        return std::holds_alternative<Block>(kv.value) && equalsIgnoringAsciiCase(kv.key, "libraryfolders");
    });
    if (top == document->end()) {
        return std::unexpected(std::format("{} has no \"libraryfolders\" block.", kLibraryListName));
    }
    std::vector<std::string> paths;
    for (const KeyValue& library : std::get<Block>(top->value)) {
        // The older layout: the number straight to the path.
        if (const std::string* direct = std::get_if<std::string>(&library.value)) {
            const bool numbered = !library.key.empty() && std::ranges::all_of(library.key, [](char c) { return c >= '0' && c <= '9'; });
            if (numbered && direct->starts_with('/')) {
                paths.push_back(*direct);
            }
            continue;
        }
        for (const KeyValue& field : std::get<Block>(library.value)) {
            const std::string* path = std::get_if<std::string>(&field.value);
            if (path != nullptr && equalsIgnoringAsciiCase(field.key, "path") && path->starts_with('/')) {
                paths.push_back(*path);
                break;
            }
        }
    }
    return paths;
}

} // namespace wallpaperx::steam
