#include "nova/core/Log.h"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <chrono>

namespace nova {

std::shared_ptr<spdlog::logger> Log::s_core;
std::shared_ptr<spdlog::logger> Log::s_client;

namespace {
std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> g_ring;
}

void Log::init() {
    if (s_core) return;

    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console->set_pattern("%^[%T] [%n] %v%$");

    auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>("nova.log", true);
    file->set_pattern("[%T.%e] [%l] [%n] %v");

    g_ring = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(1024);
    g_ring->set_pattern("[%T] [%n] %v");

    std::vector<spdlog::sink_ptr> sinks{console, file, g_ring};
    s_core = std::make_shared<spdlog::logger>("NOVA", sinks.begin(), sinks.end());
    s_client = std::make_shared<spdlog::logger>("APP", sinks.begin(), sinks.end());

    for (auto& l : {s_core, s_client}) {
        l->set_level(spdlog::level::trace);
        l->flush_on(spdlog::level::warn);
    }
    spdlog::flush_every(std::chrono::seconds(1));
}

void Log::shutdown() {
    if (s_core) s_core->flush();
    if (s_client) s_client->flush();
    s_core.reset();
    s_client.reset();
    g_ring.reset();
}

std::vector<LogEntry> Log::recent(size_t max) {
    std::vector<LogEntry> out;
    if (!g_ring) return out;
    auto raw = g_ring->last_raw(max);
    auto formatted = g_ring->last_formatted(max);
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size() && i < formatted.size(); ++i) {
        std::string text = formatted[i];
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        out.push_back({raw[i].level, std::move(text)});
    }
    return out;
}

} // namespace nova
