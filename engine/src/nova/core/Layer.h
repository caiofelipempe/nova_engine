#pragma once

#include "nova/core/Base.h"

#include <string>

namespace nova {

namespace rhi { class CommandList; }

// A Layer is a unit of application logic driven by the Application loop.
// Per frame, in order: onUpdate -> onImGui -> onRender.
class Layer {
public:
    explicit Layer(std::string name = "Layer") : m_name(std::move(name)) {}
    virtual ~Layer() = default;

    virtual void onAttach() {}
    virtual void onDetach() {}
    virtual void onUpdate(f32 /*dt*/) {}
    virtual void onImGui() {}
    // Record GPU work for this frame. The swapchain image is available via device().backbuffer().
    virtual void onRender(rhi::CommandList& /*cmd*/) {}

    const std::string& name() const { return m_name; }

private:
    std::string m_name;
};

} // namespace nova
