#pragma once

/// @file
/// @brief Logging (spdlog) and assertion macros.

#include "nova/core/Base.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace nova {

/// @brief One formatted log message, as kept in the in-memory ring buffer.
struct LogEntry {
    spdlog::level::level_enum level; ///< Severity.
    std::string text;                ///< Formatted message (`[time] [logger] text`), no trailing newline.
};

/// @brief Engine-wide logging built on spdlog.
///
/// Two loggers share the same sinks: the console (colored), the file `nova.log` (next to the
/// working directory) and a 1024-entry in-memory ring buffer (see recent()).
/// - `core()`   — engine messages, tagged `NOVA`; use the `NOVA_CORE_*` macros.
/// - `client()` — application/game messages, tagged `APP`; use the `NOVA_*` macros.
///
/// Messages of level `warn` and above are flushed immediately; the rest every second.
class Log {
public:
    /// @brief Creates the loggers. Called by Application; call it yourself in tools without one.
    static void init();
    /// @brief Flushes and destroys the loggers.
    static void shutdown();

    /// @brief Engine logger. @pre init() was called.
    static spdlog::logger& core() { return *s_core; }
    /// @brief Application logger. @pre init() was called.
    static spdlog::logger& client() { return *s_client; }

    /// @brief The most recent formatted messages, oldest first (e.g. for an in-editor console).
    /// @param max Maximum number of entries to return.
    static std::vector<LogEntry> recent(size_t max = 512);

private:
    static std::shared_ptr<spdlog::logger> s_core;
    static std::shared_ptr<spdlog::logger> s_client;
};

} // namespace nova

/// @name Engine log macros (fmt-style formatting)
/// @{
#define NOVA_CORE_TRACE(...)    ::nova::Log::core().trace(__VA_ARGS__)
#define NOVA_CORE_INFO(...)     ::nova::Log::core().info(__VA_ARGS__)
#define NOVA_CORE_WARN(...)     ::nova::Log::core().warn(__VA_ARGS__)
#define NOVA_CORE_ERROR(...)    ::nova::Log::core().error(__VA_ARGS__)
#define NOVA_CORE_CRITICAL(...) ::nova::Log::core().critical(__VA_ARGS__)
/// @}

/// @name Application log macros (fmt-style formatting)
/// @code
/// NOVA_INFO("Player {} picked up {} coins", name, count);
/// @endcode
/// @{
#define NOVA_TRACE(...)    ::nova::Log::client().trace(__VA_ARGS__)
#define NOVA_INFO(...)     ::nova::Log::client().info(__VA_ARGS__)
#define NOVA_WARN(...)     ::nova::Log::client().warn(__VA_ARGS__)
#define NOVA_ERROR(...)    ::nova::Log::client().error(__VA_ARGS__)
#define NOVA_CRITICAL(...) ::nova::Log::client().critical(__VA_ARGS__)
/// @}

/// @brief Always-on check, also in Release builds.
///
/// If `cond` is false, logs a critical message with the expression, location and `msg`, then
/// breaks into the debugger (Debug) or aborts (Release).
/// @param cond Condition expected to be true.
/// @param msg Extra context (anything formattable by fmt).
#define NOVA_VERIFY(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            NOVA_CORE_CRITICAL("Check '{}' failed at {}:{}: {}", #cond, __FILE__, __LINE__, msg);  \
            ::nova::Log::core().flush();                                                           \
            NOVA_BREAK_OR_ABORT();                                                                 \
        }                                                                                          \
    } while (0)

#ifdef NOVA_DEBUG
    /// @brief Debug break in Debug builds, `std::abort()` otherwise.
    #define NOVA_BREAK_OR_ABORT() NOVA_DEBUGBREAK()
    /// @brief Like NOVA_VERIFY, but compiled out of Release builds.
    #define NOVA_ASSERT(cond, msg) NOVA_VERIFY(cond, msg)
#else
    #define NOVA_BREAK_OR_ABORT() std::abort()
    #define NOVA_ASSERT(cond, msg) ((void)0)
#endif
