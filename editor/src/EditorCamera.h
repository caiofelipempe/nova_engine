#pragma once

#include <nova/Nova.h>

#include <glm/gtc/matrix_transform.hpp>

// Unreal/Unity-style fly camera: hold RMB to look, WASD/QE to move, wheel changes speed.
class EditorCamera {
public:
    glm::vec3 position{0.0f, 4.0f, 12.0f};
    float yaw = 0.0f;    // radians, 0 looks down -Z
    float pitch = -0.3f; // radians
    float fovY = glm::radians(60.0f);
    float speed = 6.0f;

    glm::vec3 forward() const {
        return {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
    }
    glm::vec3 right() const { return glm::normalize(glm::cross(forward(), glm::vec3(0, 1, 0))); }

    // Returns true while the camera is being controlled (captures the mouse).
    bool update(float dt, bool viewportHovered) {
        using nova::Input;
        bool rmb = Input::isMouseButtonDown(nova::MouseButton::Right);
        if (rmb && (viewportHovered || m_active)) {
            m_active = true;
            glm::vec2 d = Input::mouseDelta() * 0.0025f;
            yaw -= d.x;
            pitch = glm::clamp(pitch - d.y, -1.55f, 1.55f);

            float s = speed * dt * (Input::isKeyDown(nova::Key::LeftShift) ? 4.0f : 1.0f);
            if (Input::isKeyDown(nova::Key::W)) position += forward() * s;
            if (Input::isKeyDown(nova::Key::S)) position -= forward() * s;
            if (Input::isKeyDown(nova::Key::D)) position += right() * s;
            if (Input::isKeyDown(nova::Key::A)) position -= right() * s;
            if (Input::isKeyDown(nova::Key::E)) position.y += s;
            if (Input::isKeyDown(nova::Key::Q)) position.y -= s;
            float wheel = Input::scrollDelta();
            if (wheel != 0.0f) speed = glm::clamp(speed * (wheel > 0 ? 1.2f : 1.0f / 1.2f), 0.5f, 200.0f);
        } else {
            m_active = false;
        }
        return m_active;
    }

    void focus(const glm::vec3& target, float distance = 6.0f) { position = target - forward() * distance; }

    nova::CameraData data(float aspect) const {
        return nova::CameraData::perspective(position, position + forward(), fovY, aspect, 0.05f, 1000.0f);
    }

private:
    bool m_active = false;
};
