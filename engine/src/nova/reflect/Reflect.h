#pragma once

/// @file
/// @brief Lightweight runtime reflection for components, scripts, structs and enums.
///
/// Describe a type once and the engine/editor derive the rest from it: the editor inspector
/// (widgets, "Add Component"), scene serialization (save/load, editor Play/Stop) and copy tools.
/// @code
/// struct Health { float current = 100; float max = 100; bool invulnerable = false; };
///
/// nova::reflect::type<Health>("Health")
///     .field("current", &Health::current, {.min = 0, .max = 1000})
///     .field("max", &Health::max, {.min = 1, .max = 1000})
///     .field("invulnerable", &Health::invulnerable, {.tooltip = "Ignores all damage"})
///     .component();
/// @endcode
/// Register before the first use (e.g. at the start of `main` or in EditorApp::onSetup).

#include "nova/core/Module.h"
#include "nova/math/GlobalPosition.h"
#include "nova/renderer/Mesh.h"
#include "nova/scene/Script.h"

#include <glm/glm.hpp>

#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <vector>

/// @brief Runtime reflection (see Reflect.h).
namespace nova::reflect {

/// @brief How a field is stored, which drives its widget and its serialized form.
enum class FieldKind {
    Bool,   ///< `bool`
    Int,    ///< `i32` / `int`
    UInt,   ///< `u32`
    UInt64, ///< `u64` (e.g. UUIDs)
    Float,  ///< `f32`
    Vec2,   ///< `glm::vec2`
    Vec3,   ///< `glm::vec3`
    Vec4,   ///< `glm::vec4`
    String, ///< `std::string`
    Enum,   ///< any enum registered with enumeration()
    Mesh,   ///< MeshHandle (serialized by mesh name/asset or id)
    Texture, ///< rhi::TextureHandle (serialized by texture asset or id)
    Shader, ///< ShaderHandle (serialized by shader asset or id)
    Position, ///< GlobalPosition (meters when exact, raw integer steps otherwise)
    Quat,   ///< `glm::quat` rotation (serialized as [x, y, z, w]; edited as Euler degrees)
    Struct, ///< any struct registered with type() (nested object)
};

/// @brief Per-field metadata for editors and serialization (all optional).
///
/// Designed for designated initializers: `{.min = 0, .max = 1, .tooltip = "..."}`.
struct FieldAttrs {
    f32 min = 0.0f;                ///< Lower bound shown/enforced by editors (with `max`; both 0 = unbounded).
    f32 max = 0.0f;                ///< Upper bound (if `max > min`, numbers use a slider).
    f32 speed = 0.0f;              ///< Drag speed for numeric widgets (0 = default).
    bool degrees = false;          ///< Stored in radians, edited in degrees (min/max are in degrees).
    bool color = false;            ///< Edit a vec3/vec4 with a color picker.
    bool readOnly = false;         ///< Shown but not editable.
    bool hidden = false;           ///< Serialized but not shown in the inspector.
    bool transient = false;        ///< Runtime state: neither serialized nor shown.
    const char* tooltip = nullptr; ///< Hover help in the inspector.
    const char* label = nullptr;   ///< Display label (default: derived from the field name).
};

struct TypeInfo;

/// @brief One reflected data member.
struct FieldInfo {
    std::string name;                     ///< Identifier used in files (stable: renaming breaks old files).
    std::string label;                    ///< Display label.
    FieldKind kind = FieldKind::Struct;   ///< Storage kind.
    FieldAttrs attrs;                     ///< Metadata.
    std::type_index type{typeid(void)};   ///< C++ type of the member (Enum/Struct: resolve with nestedType()).
    std::function<void*(void*)> access;   ///< Object pointer → member pointer.
    std::function<i64(const void*)> getEnum;    ///< Enum fields: read the value as an integer.
    std::function<void(void*, i64)> setEnum;    ///< Enum fields: write an integer value.

    /// @brief The member inside `object` (pointer to the owning type).
    void* in(void* object) const { return access(object); }
    /// @brief Typed access to the member inside `object`. @tparam F Exact member type.
    template <typename F>
    F& as(void* object) const { return *static_cast<F*>(access(object)); }
    /// @brief The registered TypeInfo of an Enum/Struct field (`nullptr` if not registered).
    const TypeInfo* nestedType() const;
};

/// @brief Name/value pair of a reflected enum.
struct EnumValue {
    std::string name; ///< Name used in files and editors.
    i64 value = 0;    ///< Underlying value.
};

/// @brief Everything known about a reflected type.
struct TypeInfo {
    std::string name;                   ///< Identifier used in files (e.g. "RigidBody").
    std::string displayName;            ///< Shown in editors (default: name with spaces, "Rigid Body").
    std::type_index type{typeid(void)}; ///< The C++ type.
    std::vector<FieldInfo> fields;      ///< Reflected members, in declaration order.
    std::vector<EnumValue> enumValues;  ///< Enums only.
    bool isEnum = false;                ///< Registered with enumeration().
    bool hideInInspector = false;       ///< Components the editor should not list (e.g. Id, Name).
    ModuleId module = 0;                ///< Code module that registered it (see Module.h).

    /// @name Component operations (set by TypeBuilder::component)
    /// @{
    bool isComponent = false;
    std::function<bool(Entity)> has;     ///< Entity has the component.
    std::function<void*(Entity)> get;    ///< Pointer to the entity's component.
    std::function<void(Entity)> add;     ///< Adds a default-constructed component.
    std::function<void(Entity)> remove;  ///< Removes the component.
    /// @}

    /// @name Script operations (set by TypeBuilder::script)
    /// @{
    bool isScript = false;
    std::function<Scope<Script>()> createScript; ///< New default-constructed instance.
    /// @}

    /// @brief Field by name, or `nullptr`.
    const FieldInfo* field(std::string_view fieldName) const;
    /// @brief Name of an enum value, or `nullptr` if unknown.
    const char* enumName(i64 value) const;
    /// @brief Value of an enum name, or `std::nullopt` if unknown.
    std::optional<i64> enumValue(std::string_view valueName) const;
};

/// @brief All reflected types. Engine types are registered on first access.
class TypeRegistry {
public:
    /// @brief Stores (or replaces, if the C++ type is already registered) a type, tagged with currentModule().
    TypeInfo& add(TypeInfo info);
    /// @brief Type by C++ type, or `nullptr`.
    const TypeInfo* find(std::type_index type) const;
    /// @brief Type by C++ type, or `nullptr`.
    template <typename T>
    const TypeInfo* find() const { return find(std::type_index(typeid(T))); }
    /// @brief Type by registered name, or `nullptr`.
    const TypeInfo* find(std::string_view name) const;
    /// @brief Component types, in registration order.
    std::vector<const TypeInfo*> components() const;
    /// @brief Script types, in registration order.
    std::vector<const TypeInfo*> scripts() const;
    /// @brief Every registered type, in registration order.
    std::vector<const TypeInfo*> all() const;
    /// @brief Removes every type registered by module (before unloading its code). @return How many.
    size_t removeModule(ModuleId module);

private:
    std::vector<std::unique_ptr<TypeInfo>> m_types; // TypeInfo addresses stay valid
};

/// @brief The global type registry (engine types are registered on the first call).
TypeRegistry& types();

/// @brief "linearDamping" → "Linear Damping", "RigidBody" → "Rigid Body".
std::string prettifyName(std::string_view identifier);

/// @brief Storage kind for a C++ member type.
template <typename F>
constexpr FieldKind kindOf() {
    if constexpr (std::is_same_v<F, bool>) return FieldKind::Bool;
    else if constexpr (std::is_same_v<F, i32>) return FieldKind::Int;
    else if constexpr (std::is_same_v<F, u32>) return FieldKind::UInt;
    else if constexpr (std::is_same_v<F, u64>) return FieldKind::UInt64;
    else if constexpr (std::is_same_v<F, f32>) return FieldKind::Float;
    else if constexpr (std::is_same_v<F, glm::vec2>) return FieldKind::Vec2;
    else if constexpr (std::is_same_v<F, glm::vec3>) return FieldKind::Vec3;
    else if constexpr (std::is_same_v<F, glm::vec4>) return FieldKind::Vec4;
    else if constexpr (std::is_same_v<F, std::string>) return FieldKind::String;
    else if constexpr (std::is_enum_v<F>) return FieldKind::Enum;
    else if constexpr (std::is_same_v<F, MeshHandle>) return FieldKind::Mesh;
    else if constexpr (std::is_same_v<F, rhi::TextureHandle>) return FieldKind::Texture;
    else if constexpr (std::is_same_v<F, ShaderHandle>) return FieldKind::Shader;
    else if constexpr (std::is_same_v<F, GlobalPosition>) return FieldKind::Position;
    else if constexpr (std::is_same_v<F, glm::quat>) return FieldKind::Quat;
    else return FieldKind::Struct;
}

/// @brief Fluent registration of a struct, component or script. Created by type().
template <typename T>
class TypeBuilder {
public:
    explicit TypeBuilder(TypeInfo& info) : m_info(info) {}

    /// @brief Adds a reflected member.
    /// @param name Identifier used in files (keep it stable).
    /// @param member Pointer to member, e.g. `&Health::current`.
    /// @param attrs Optional metadata.
    template <typename F>
    TypeBuilder& field(std::string name, F T::*member, FieldAttrs attrs = {}) {
        FieldInfo f;
        f.label = attrs.label ? attrs.label : prettifyName(name);
        f.name = std::move(name);
        f.kind = kindOf<F>();
        f.attrs = attrs;
        f.type = std::type_index(typeid(F));
        f.access = [member](void* obj) -> void* { return &(static_cast<T*>(obj)->*member); };
        if constexpr (std::is_enum_v<F>) {
            f.getEnum = [](const void* p) { return static_cast<i64>(*static_cast<const F*>(p)); };
            f.setEnum = [](void* p, i64 v) { *static_cast<F*>(p) = static_cast<F>(v); };
        }
        m_info.fields.push_back(std::move(f));
        return *this;
    }

    /// @brief Marks the type as an ECS component (enables inspector + serialization of entities).
    TypeBuilder& component() {
        static_assert(std::is_default_constructible_v<T>, "Reflected components must be default-constructible");
        m_info.isComponent = true;
        m_info.has = [](Entity e) { return e.has<T>(); };
        m_info.get = [](Entity e) -> void* { return &e.get<T>(); };
        m_info.add = [](Entity e) { e.add<T>(); };
        m_info.remove = [](Entity e) { e.remove<T>(); };
        return *this;
    }

    /// @brief Marks the type as a native Script that can be created by name (files, editor).
    TypeBuilder& script() {
        static_assert(std::is_base_of_v<Script, T>, "script() requires a type derived from nova::Script");
        static_assert(std::is_default_constructible_v<T>, "Reflected scripts must be default-constructible");
        m_info.isScript = true;
        m_info.createScript = []() -> Scope<Script> { return makeScope<T>(); };
        return *this;
    }

    /// @brief Overrides the display name (default: prettified type name).
    TypeBuilder& displayName(std::string name) {
        m_info.displayName = std::move(name);
        return *this;
    }

    /// @brief Hides the component from editor listings (still serialized).
    TypeBuilder& hideInInspector() {
        m_info.hideInInspector = true;
        return *this;
    }

    /// @brief The registered description.
    TypeInfo& info() { return m_info; }

private:
    TypeInfo& m_info;
};

/// @brief Registers (or re-registers) type `T` under `name` and returns a builder for its fields.
/// @param name Identifier used in files; must be unique among reflected types.
template <typename T>
TypeBuilder<T> type(std::string name) {
    TypeInfo info;
    info.displayName = prettifyName(name);
    info.name = std::move(name);
    info.type = std::type_index(typeid(T));
    return TypeBuilder<T>(types().add(std::move(info)));
}

/// @brief Fluent registration of an enum. Created by enumeration().
template <typename E>
class EnumBuilder {
public:
    explicit EnumBuilder(TypeInfo& info) : m_info(info) {}
    /// @brief Adds a named value.
    EnumBuilder& value(std::string name, E v) {
        m_info.enumValues.push_back({std::move(name), static_cast<i64>(v)});
        return *this;
    }

private:
    TypeInfo& m_info;
};

/// @brief Registers enum `E` under `name`; add its values with EnumBuilder::value.
template <typename E>
EnumBuilder<E> enumeration(std::string name) {
    static_assert(std::is_enum_v<E>);
    TypeInfo info;
    info.displayName = prettifyName(name);
    info.name = std::move(name);
    info.type = std::type_index(typeid(E));
    info.isEnum = true;
    return EnumBuilder<E>(types().add(std::move(info)));
}

namespace detail {
/// @brief Registers the engine's built-in types (called once by types()).
void registerEngineTypes();
} // namespace detail

} // namespace nova::reflect
