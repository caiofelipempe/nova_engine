#pragma once

#include "nova/core/Base.h"

#include <vector>

namespace nova::rhi {

// Generational slot map: handles stay small (index + generation) and stale handles are
// detected instead of aliasing a newer resource. Shared by all RHI backends.
template <typename T, typename H>
class HandlePool {
public:
    H insert(T value) {
        u32 index;
        if (!m_free.empty()) {
            index = m_free.back();
            m_free.pop_back();
        } else {
            index = static_cast<u32>(m_slots.size());
            m_slots.emplace_back();
        }
        Slot& s = m_slots[index];
        s.value = std::move(value);
        s.alive = true;
        ++m_alive;
        return H{index, s.generation};
    }

    T* get(H h) {
        if (h.index >= m_slots.size()) return nullptr;
        Slot& s = m_slots[h.index];
        return (s.alive && s.generation == h.generation) ? &s.value : nullptr;
    }
    const T* get(H h) const { return const_cast<HandlePool*>(this)->get(h); }

    bool remove(H h) {
        if (!get(h)) return false;
        Slot& s = m_slots[h.index];
        s.alive = false;
        s.value = T{};
        if (++s.generation == 0) s.generation = 1;
        m_free.push_back(h.index);
        --m_alive;
        return true;
    }

    template <typename F>
    void forEach(F&& f) {
        for (u32 i = 0; i < m_slots.size(); ++i)
            if (m_slots[i].alive) f(H{i, m_slots[i].generation}, m_slots[i].value);
    }

    u32 size() const { return m_alive; }

private:
    struct Slot {
        T value{};
        u32 generation = 1;
        bool alive = false;
    };
    std::vector<Slot> m_slots;
    std::vector<u32> m_free;
    u32 m_alive = 0;
};

} // namespace nova::rhi
