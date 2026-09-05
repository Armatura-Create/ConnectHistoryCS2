#include "core/util/sql_sanitizer.h"

#include "core/logger.h"

#include <cctype>

namespace ch {
namespace {

char Lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool MatchesAt(const std::string& text, size_t position, const char* word) {
    size_t i = 0;
    for (; word[i] != '\0'; ++i) {
        if (position + i >= text.size()) return false;
        if (Lower(text[position + i]) != word[i]) return false;
    }

    // Граница слова слева: "mypassword=" паролем не является
    if (position > 0) {
        const char previous = text[position - 1];
        if (std::isalnum(static_cast<unsigned char>(previous)) || previous == '_') return false;
    }
    return true;
}

}  // namespace

std::string MaskSecrets(const std::string& text) {
    if (text.empty()) return std::string();

    static const char* const kKeys[] = {"password", "pwd"};

    std::string out;
    out.reserve(text.size());

    size_t i = 0;
    while (i < text.size()) {
        const char* matched = nullptr;
        size_t matchedLength = 0;

        for (const char* key : kKeys) {
            if (MatchesAt(text, i, key)) {
                matched = key;
                matchedLength = std::char_traits<char>::length(key);
                break;
            }
        }

        if (matched == nullptr) {
            out.push_back(text[i]);
            ++i;
            continue;
        }

        // Ключ найден, но за ним должно идти '=' (возможно, после пробелов).
        // Иначе это просто слово "password" в тексте — его не трогаем.
        size_t cursor = i + matchedLength;
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) ++cursor;

        if (cursor >= text.size() || text[cursor] != '=') {
            out.append(text, i, matchedLength);
            i += matchedLength;
            continue;
        }

        ++cursor;
        while (cursor < text.size() && text[cursor] != ';') ++cursor;

        out.append("password=***");
        i = cursor;
    }

    return out;
}

std::string SanitizePrefix(const std::string& prefix, ILogger* logger) {
    bool ok = prefix.size() <= 16;

    if (ok) {
        for (const char c : prefix) {
            const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                 (c >= '0' && c <= '9') || c == '_';
            if (!allowed) {
                ok = false;
                break;
            }
        }
    }

    if (ok) return prefix;

    if (logger != nullptr) {
        logger->Error("[DB] TablePrefix \"" + prefix +
                      "\" содержит недопустимые символы (разрешены латиница, цифры и '_', "
                      "до 16 символов). Использую \"ch_\"");
    }
    return "ch_";
}

}  // namespace ch
