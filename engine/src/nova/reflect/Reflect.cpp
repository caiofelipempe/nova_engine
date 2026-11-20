#include "nova/reflect/Reflect.h"

#include <cctype>

namespace nova::reflect {

const TypeInfo* FieldInfo::nestedType() const { return types().find(type); }

const FieldInfo* TypeInfo::field(std::string_view fieldName) const {
    for (const auto& f : fields)
        if (f.name == fieldName) return &f;
    return nullptr;
}

const char* TypeInfo::enumName(i64 value) const {
    for (const auto& v : enumValues)
        if (v.value == value) return v.name.c_str();
    return nullptr;
}

std::optional<i64> TypeInfo::enumValue(std::string_view valueName) const {
    for (const auto& v : enumValues)
        if (v.name == valueName) return v.value;
    return std::nullopt;
}

TypeInfo& TypeRegistry::add(TypeInfo info) {
    for (auto& t : m_types)
        if (t.type == info.type) return t = std::move(info);
    return m_types.emplace_back(std::move(info));
}

const TypeInfo* TypeRegistry::find(std::type_index type) const {
    for (const auto& t : m_types)
        if (t.type == type) return &t;
    return nullptr;
}

const TypeInfo* TypeRegistry::find(std::string_view name) const {
    for (const auto& t : m_types)
        if (t.name == name) return &t;
    return nullptr;
}

std::vector<const TypeInfo*> TypeRegistry::components() const {
    std::vector<const TypeInfo*> out;
    for (const auto& t : m_types)
        if (t.isComponent) out.push_back(&t);
    return out;
}

std::vector<const TypeInfo*> TypeRegistry::scripts() const {
    std::vector<const TypeInfo*> out;
    for (const auto& t : m_types)
        if (t.isScript) out.push_back(&t);
    return out;
}

TypeRegistry& types() {
    static TypeRegistry registry;
    static bool initialized = false;
    if (!initialized) {
        initialized = true; // set first: registration calls types() recursively
        detail::registerEngineTypes();
    }
    return registry;
}

std::string prettifyName(std::string_view id) {
    std::string out;
    for (size_t i = 0; i < id.size(); ++i) {
        char c = id[i];
        if (c == '_') {
            if (!out.empty() && out.back() != ' ') out += ' ';
            continue;
        }
        const bool upper = std::isupper(static_cast<unsigned char>(c)) != 0;
        const bool prevLower = i > 0 && std::islower(static_cast<unsigned char>(id[i - 1]));
        const bool prevDigit = i > 0 && std::isdigit(static_cast<unsigned char>(id[i - 1]));
        if (upper && (prevLower || prevDigit) && !out.empty() && out.back() != ' ') out += ' ';
        out += out.empty() || out.back() == ' ' ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
    }
    return out;
}

} // namespace nova::reflect
