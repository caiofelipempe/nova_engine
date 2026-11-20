#include "nova/editor/Command.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace nova::editor {

namespace {

struct NamedKey {
    const char* name;
    Key key;
};

// Named (non single-character) keys. The first name of a key is its canonical spelling.
constexpr std::array<NamedKey, 22> kNamedKeys{{
    {"Space", Key::Space},       {"Enter", Key::Enter},         {"Escape", Key::Escape},
    {"Esc", Key::Escape},        {"Tab", Key::Tab},             {"Backspace", Key::Backspace},
    {"Delete", Key::Delete},     {"Del", Key::Delete},          {"Insert", Key::Insert},
    {"Home", Key::Home},         {"End", Key::End},             {"PageUp", Key::PageUp},
    {"PageDown", Key::PageDown}, {"Up", Key::Up},               {"Down", Key::Down},
    {"Left", Key::Left},         {"Right", Key::Right},         {"Minus", Key::Minus},
    {"Equal", Key::Equal},       {"Comma", Key::Comma},         {"Period", Key::Period},
    {"Slash", Key::Slash},
}};

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

Key parseKey(std::string_view token) {
    const std::string t = lower(token);
    if (t.size() == 1) {
        char c = t[0];
        if (c >= 'a' && c <= 'z') return static_cast<Key>(static_cast<i32>(Key::A) + (c - 'a'));
        if (c >= '0' && c <= '9') return static_cast<Key>(static_cast<i32>(Key::D0) + (c - '0'));
    }
    if (t.size() >= 2 && t.size() <= 3 && t[0] == 'f') {
        int n = 0;
        for (size_t i = 1; i < t.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(t[i]))) return Key::Unknown;
            n = n * 10 + (t[i] - '0');
        }
        if (n >= 1 && n <= 12) return static_cast<Key>(static_cast<i32>(Key::F1) + n - 1);
        return Key::Unknown;
    }
    for (const NamedKey& k : kNamedKeys)
        if (lower(k.name) == t) return k.key;
    return Key::Unknown;
}

std::string keyName(Key key) {
    const i32 k = static_cast<i32>(key);
    if (k >= static_cast<i32>(Key::A) && k <= static_cast<i32>(Key::Z)) return std::string(1, static_cast<char>('A' + (k - static_cast<i32>(Key::A))));
    if (k >= static_cast<i32>(Key::D0) && k <= static_cast<i32>(Key::D9)) return std::string(1, static_cast<char>('0' + (k - static_cast<i32>(Key::D0))));
    if (k >= static_cast<i32>(Key::F1) && k <= static_cast<i32>(Key::F12)) return "F" + std::to_string(k - static_cast<i32>(Key::F1) + 1);
    for (const NamedKey& n : kNamedKeys)
        if (n.key == key) return n.name;
    return {};
}

} // namespace

std::optional<KeyChord> KeyChord::parse(std::string_view text) {
    KeyChord chord;
    text = trim(text);
    if (text.empty()) return std::nullopt;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('+', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view token = trim(text.substr(start, end - start));
        if (token.empty()) return std::nullopt;
        const std::string t = lower(token);
        if (t == "ctrl" || t == "control") chord.ctrl = true;
        else if (t == "shift") chord.shift = true;
        else if (t == "alt") chord.alt = true;
        else {
            if (chord.key != Key::Unknown) return std::nullopt; // two main keys
            chord.key = parseKey(token);
            if (chord.key == Key::Unknown) return std::nullopt;
        }
        start = end + 1;
    }
    if (chord.key == Key::Unknown) return std::nullopt; // modifiers only
    return chord;
}

std::string KeyChord::toString() const {
    if (!valid()) return {};
    std::string s;
    if (ctrl) s += "Ctrl+";
    if (shift) s += "Shift+";
    if (alt) s += "Alt+";
    return s + keyName(key);
}

} // namespace nova::editor
