#pragma once

/// @file
/// @brief Base class for units of application logic.

#include "nova/core/Base.h"

#include <string>

namespace nova {

namespace rhi { class CommandList; }

/// @brief A unit of application logic driven by the Application loop.
///
/// Override only what you need. Per frame the Application calls, for every layer in push order:
/// onUpdate() → onImGui() → onRender().
///
/// @see GameLayer for a ready-made layer that owns and renders a Scene.
class Layer {
public:
    /// @param name Debug name of the layer.
    explicit Layer(std::string name = "Layer") : m_name(std::move(name)) {}
    virtual ~Layer() = default;

    /// @brief Called once when the layer is pushed (subsystems are already initialized).
    virtual void onAttach() {}
    /// @brief Called once when the application shuts down, in reverse push order.
    virtual void onDetach() {}
    /// @brief Game/application logic.
    /// @param dt Seconds since the previous frame.
    virtual void onUpdate(f32 /*dt*/) {}
    /// @brief Build Dear ImGui windows. Only called when the application has ImGui enabled.
    virtual void onImGui() {}
    /// @brief Record GPU work for this frame.
    /// @param cmd Command list of the current frame. The swapchain image is `Application::get().device().backbuffer()`.
    virtual void onRender(rhi::CommandList& /*cmd*/) {}

    /// @brief The layer's debug name.
    const std::string& name() const { return m_name; }

private:
    std::string m_name;
};

} // namespace nova
