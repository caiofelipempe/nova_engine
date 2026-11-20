#pragma once

/// @file
/// @brief Minimal synchronous signal (observer list).

#include "nova/core/Base.h"

#include <functional>
#include <utility>
#include <vector>

namespace nova::editor {

/// @brief A list of callbacks invoked synchronously by emit().
///
/// @code
/// u32 id = ctx.selectionChanged.connect([](nova::Entity e) { NOVA_INFO("selected {}", e.name()); });
/// ctx.selectionChanged.disconnect(id);
/// @endcode
/// @tparam Args Arguments passed to the callbacks.
template <typename... Args>
class Signal {
public:
    using Slot = std::function<void(Args...)>; ///< Callback type.

    /// @brief Adds a callback. @return Id for disconnect().
    u32 connect(Slot slot) {
        m_slots.emplace_back(++m_nextId, std::move(slot));
        return m_nextId;
    }
    /// @brief Removes a callback (unknown ids are ignored).
    void disconnect(u32 id) {
        std::erase_if(m_slots, [id](const auto& s) { return s.first == id; });
    }
    /// @brief Calls every callback. Callbacks may connect/disconnect during emission.
    void emit(Args... args) const {
        auto slots = m_slots; // copy: tolerate (dis)connection from inside a callback
        for (auto& [id, slot] : slots) slot(args...);
    }
    /// @brief Number of connected callbacks.
    size_t size() const { return m_slots.size(); }

private:
    std::vector<std::pair<u32, Slot>> m_slots;
    u32 m_nextId = 0;
};

} // namespace nova::editor
