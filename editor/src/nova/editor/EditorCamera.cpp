#include "nova/editor/EditorCamera.h"

#include "nova/platform/Input.h"

#include <cmath>

namespace nova::editor {

glm::vec3 EditorCamera::forward() const {
    return {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

glm::vec3 EditorCamera::right() const { return glm::normalize(glm::cross(forward(), glm::vec3(0, 1, 0))); }

bool EditorCamera::update(f32 dt, bool hovered) {
    const bool rmb = Input::isMouseButtonDown(MouseButton::Right);
    if (!(rmb && (hovered || m_active))) {
        m_active = false;
        return false;
    }
    m_active = true;
    glm::vec2 d = Input::mouseDelta() * lookSensitivity;
    yaw -= d.x;
    pitch = glm::clamp(pitch - d.y, -1.55f, 1.55f);

    const f32 s = speed * dt * (Input::isKeyDown(Key::LeftShift) ? 4.0f : 1.0f);
    if (Input::isKeyDown(Key::W)) position += forward() * s;
    if (Input::isKeyDown(Key::S)) position -= forward() * s;
    if (Input::isKeyDown(Key::D)) position += right() * s;
    if (Input::isKeyDown(Key::A)) position -= right() * s;
    if (Input::isKeyDown(Key::E)) position += glm::vec3(0.0f, s, 0.0f);
    if (Input::isKeyDown(Key::Q)) position -= glm::vec3(0.0f, s, 0.0f);
    if (f32 wheel = Input::scrollDelta(); wheel != 0.0f)
        speed = glm::clamp(speed * (wheel > 0 ? 1.2f : 1.0f / 1.2f), 0.5f, 200.0f);
    return true;
}

CameraData EditorCamera::data(f32 aspect) const {
    return CameraData::relativeLookAt(position, forward(), fovY, aspect, 0.05f, 1000.0f);
}

} // namespace nova::editor
