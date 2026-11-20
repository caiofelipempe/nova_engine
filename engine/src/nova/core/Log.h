#pragma once

#include "nova/core/Base.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace nova {

struct LogEntry {
    spdlog::level::level_enum level;
    std::string text;
};

class Log {
public:
    static void init();
    static void shutdown();

    static spdlog::logger& core() { return *s_core; }
    static spdlog::logger& client() { return *s_client; }

    // Last N formatted messages (for an in-editor console).
    static std::vector<LogEntry> recent(size_t max = 512);

private:
    static std::shared_ptr<spdlog::logger> s_core;
    static std::shared_ptr<spdlog::logger> s_client;
};

} // namespace nova

#define NOVA_CORE_TRACE(...)    ::nova::Log::core().trace(__VA_ARGS__)
#define NOVA_CORE_INFO(...)     ::nova::Log::core().info(__VA_ARGS__)
#define NOVA_CORE_WARN(...)     ::nova::Log::core().warn(__VA_ARGS__)
#define NOVA_CORE_ERROR(...)    ::nova::Log::core().error(__VA_ARGS__)
#define NOVA_CORE_CRITICAL(...) ::nova::Log::core().critical(__VA_ARGS__)

#define NOVA_TRACE(...)    ::nova::Log::client().trace(__VA_ARGS__)
#define NOVA_INFO(...)     ::nova::Log::client().info(__VA_ARGS__)
#define NOVA_WARN(...)     ::nova::Log::client().warn(__VA_ARGS__)
#define NOVA_ERROR(...)    ::nova::Log::client().error(__VA_ARGS__)
#define NOVA_CRITICAL(...) ::nova::Log::client().critical(__VA_ARGS__)

// Always-on check: logs and aborts (breaks into the debugger in debug builds).
#define NOVA_VERIFY(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            NOVA_CORE_CRITICAL("Check '{}' failed at {}:{}: {}", #cond, __FILE__, __LINE__, msg);  \
            ::nova::Log::core().flush();                                                           \
            NOVA_BREAK_OR_ABORT();                                                                 \
        }                                                                                          \
    } while (0)

#ifdef NOVA_DEBUG
    #define NOVA_BREAK_OR_ABORT() NOVA_DEBUGBREAK()
    #define NOVA_ASSERT(cond, msg) NOVA_VERIFY(cond, msg)
#else
    #define NOVA_BREAK_OR_ABORT() std::abort()
    #define NOVA_ASSERT(cond, msg) ((void)0)
#endif
