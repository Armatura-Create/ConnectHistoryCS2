#include "core/util/chat_format.h"

#include <cctype>

namespace ch {
namespace chat {
namespace {

constexpr char kDefault = '\x01';

struct ColorTag {
    const char* tag;
    char code;
};

// Коды — это управляющие байты самого движка CS2, а не изобретение фреймворка.
// Значения сверены с таблицами CounterStrikeSharp.ChatColors и SwiftlyS2.Helper:
// один и тот же Messages.json обязан одинаково работать под всеми тремя целями.
constexpr ColorTag kTags[] = {
    {"{DEFAULT}", '\x01'},
    {"{LIGHTBLUE}", '\x0B'},
    {"{GREEN}", '\x04'},
    {"{GOLD}", '\x10'},
    {"{GREY}", '\x08'},
    {"{RED}", '\x07'},
    {"{BLUE}", '\x0B'},
    {"{PURPLE}", '\x0E'},
    {"{ORANGE}", '\x10'},
    {"{OLIVE}", '\x05'},
};

bool EqualsIgnoreCaseAt(const std::string& text, size_t position, const char* needle) {
    size_t i = 0;
    for (; needle[i] != '\0'; ++i) {
        if (position + i >= text.size()) return false;
        if (std::tolower(static_cast<unsigned char>(text[position + i])) !=
            std::tolower(static_cast<unsigned char>(needle[i]))) {
            return false;
        }
    }
    return true;
}

std::string ReplaceAllIgnoreCase(const std::string& text, const std::string& needle,
                                 const std::string& replacement) {
    if (needle.empty()) return text;

    std::string out;
    out.reserve(text.size());

    size_t i = 0;
    while (i < text.size()) {
        if (EqualsIgnoreCaseAt(text, i, needle.c_str())) {
            out += replacement;
            i += needle.size();
        } else {
            out.push_back(text[i]);
            ++i;
        }
    }
    return out;
}

bool ContainsColorCode(const std::string& text) {
    for (const char c : text) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte >= 0x01 && byte <= 0x10) return true;
    }
    return false;
}

}  // namespace

std::string Render(const std::string& templateText,
                   const std::map<std::string, std::string>& values) {
    if (templateText.empty()) return std::string();

    std::string result = templateText;

    for (const auto& pair : values) {
        result = ReplaceAllIgnoreCase(result, pair.first, pair.second);
    }

    for (const ColorTag& tag : kTags) {
        result = ReplaceAllIgnoreCase(result, tag.tag, std::string(1, tag.code));
    }

    return result;
}

std::string EnsureChatColorPrefix(const std::string& input) {
    if (input.empty()) return input;

    // Без цветов чинить нечего — не приписываем пробел обычному тексту
    if (!ContainsColorCode(input)) return input;

    std::string body = input;

    // Снимаем то, что могли поставить сами или что уже есть в шаблоне,
    // чтобы результат не зависел от того, сколько раз сюда зашли
    if (!body.empty() && body[0] == kDefault) body.erase(0, 1);
    if (!body.empty() && body[0] == ' ') body.erase(0, 1);

    return std::string(1, kDefault) + " " + body;
}

}  // namespace chat
}  // namespace ch
