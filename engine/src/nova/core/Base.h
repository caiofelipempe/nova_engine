#pragma once

/// @file
/// @brief Fundamental types, ownership aliases and helper macros used across the engine.

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace nova {

/// @name Fixed-width numeric aliases
/// @{
using u8 = std::uint8_t;   ///< Unsigned 8-bit integer.
using u16 = std::uint16_t; ///< Unsigned 16-bit integer.
using u32 = std::uint32_t; ///< Unsigned 32-bit integer.
using u64 = std::uint64_t; ///< Unsigned 64-bit integer.
using i8 = std::int8_t;    ///< Signed 8-bit integer.
using i16 = std::int16_t;  ///< Signed 16-bit integer.
using i32 = std::int32_t;  ///< Signed 32-bit integer.
using i64 = std::int64_t;  ///< Signed 64-bit integer.
using f32 = float;         ///< 32-bit floating point.
using f64 = double;        ///< 64-bit floating point.
/// @}

/// @brief Unique ownership pointer (alias of `std::unique_ptr`).
template <typename T>
using Scope = std::unique_ptr<T>;

/// @brief Creates a Scope (alias of `std::make_unique`).
/// @tparam T Type to construct.
/// @param args Constructor arguments.
template <typename T, typename... Args>
constexpr Scope<T> makeScope(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}

/// @brief Shared ownership pointer (alias of `std::shared_ptr`).
template <typename T>
using Ref = std::shared_ptr<T>;

/// @brief Creates a Ref (alias of `std::make_shared`).
/// @tparam T Type to construct.
/// @param args Constructor arguments.
template <typename T, typename... Args>
constexpr Ref<T> makeRef(Args&&... args) {
    return std::make_shared<T>(std::forward<Args>(args)...);
}

/// @brief Width × height in pixels.
struct Extent2D {
    u32 width = 0;  ///< Width in pixels.
    u32 height = 0; ///< Height in pixels.
    bool operator==(const Extent2D&) const = default;
    /// @brief True if either dimension is 0 (e.g. a minimized window).
    bool isZero() const { return width == 0 || height == 0; }
};

/// @brief Base class that deletes copy construction and copy assignment.
class NonCopyable {
public:
    NonCopyable() = default;
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
};

} // namespace nova

/// @brief Defines `|`, `&`, `|=` and `any()` for an `enum class` used as a bit set.
/// @param E The enum type (must be declared in the current namespace).
#define NOVA_ENUM_FLAGS(E)                                                                         \
    constexpr E operator|(E a, E b) {                                                              \
        using U = std::underlying_type_t<E>;                                                       \
        return static_cast<E>(static_cast<U>(a) | static_cast<U>(b));                              \
    }                                                                                              \
    constexpr E operator&(E a, E b) {                                                              \
        using U = std::underlying_type_t<E>;                                                       \
        return static_cast<E>(static_cast<U>(a) & static_cast<U>(b));                              \
    }                                                                                              \
    constexpr E& operator|=(E& a, E b) { return a = a | b; }                                       \
    constexpr bool any(E a) { return static_cast<std::underlying_type_t<E>>(a) != 0; }

/// @def NOVA_API
/// @brief Marks engine *data* that code outside the engine library accesses directly (static
///        members used by inline functions). Functions and classes are exported automatically
///        when the engine is a shared library (NOVA_SHARED); data needs `__declspec(dllimport)`.
#if defined(_WIN32) && defined(NOVA_SHARED)
    #if defined(NOVA_BUILDING_ENGINE)
        #define NOVA_API __declspec(dllexport)
    #else
        #define NOVA_API __declspec(dllimport)
    #endif
#else
    #define NOVA_API
#endif

/// @def NOVA_EDITOR_API
/// @brief Like NOVA_API, for data of the editor library (Nova::Editor).
#if defined(_WIN32) && defined(NOVA_SHARED)
    #if defined(NOVA_BUILDING_EDITOR)
        #define NOVA_EDITOR_API __declspec(dllexport)
    #else
        #define NOVA_EDITOR_API __declspec(dllimport)
    #endif
#else
    #define NOVA_EDITOR_API
#endif

#if defined(_MSC_VER)
    /// @brief Breaks into an attached debugger (crashes without one).
    #define NOVA_DEBUGBREAK() __debugbreak()
#else
    #include <csignal>
    /// @brief Breaks into an attached debugger (crashes without one).
    #define NOVA_DEBUGBREAK() std::raise(SIGTRAP)
#endif
