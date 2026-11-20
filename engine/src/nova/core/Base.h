#pragma once

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace nova {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

template <typename T>
using Scope = std::unique_ptr<T>;

template <typename T, typename... Args>
constexpr Scope<T> makeScope(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}

template <typename T>
using Ref = std::shared_ptr<T>;

template <typename T, typename... Args>
constexpr Ref<T> makeRef(Args&&... args) {
    return std::make_shared<T>(std::forward<Args>(args)...);
}

struct Extent2D {
    u32 width = 0;
    u32 height = 0;
    bool operator==(const Extent2D&) const = default;
    bool isZero() const { return width == 0 || height == 0; }
};

class NonCopyable {
public:
    NonCopyable() = default;
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
};

} // namespace nova

// Bitwise operators for `enum class` flag types.
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

#if defined(_MSC_VER)
    #define NOVA_DEBUGBREAK() __debugbreak()
#else
    #include <csignal>
    #define NOVA_DEBUGBREAK() std::raise(SIGTRAP)
#endif
