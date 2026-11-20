#pragma once

#include "nova/physics/PhysicsWorld.h"
#include "nova/renderer/Mesh.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <string>

namespace nova {

class Script;

struct NameComponent {
    std::string name;
};

struct TransformComponent {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f}; // Euler angles in radians (pitch, yaw, roll)
    glm::vec3 scale{1.0f};

    glm::quat orientation() const { return glm::quat(rotation); }
    glm::mat4 matrix() const {
        return glm::translate(glm::mat4(1.0f), position) * glm::toMat4(orientation()) *
               glm::scale(glm::mat4(1.0f), scale);
    }
    glm::vec3 forward() const { return orientation() * glm::vec3(0, 0, -1); }
    glm::vec3 right() const { return orientation() * glm::vec3(1, 0, 0); }
    glm::vec3 up() const { return orientation() * glm::vec3(0, 1, 0); }

    // Points -Z at `target` (yaw/pitch only, no roll — FPS-camera friendly).
    void lookAt(const glm::vec3& target) {
        glm::vec3 dir = glm::normalize(target - position);
        rotation = {std::asin(glm::clamp(dir.y, -1.0f, 1.0f)), std::atan2(-dir.x, -dir.z), 0.0f};
    }
};

struct MeshComponent {
    MeshHandle mesh;
    Material material;
    bool castShadows = true;
};

struct CameraComponent {
    f32 fovY = glm::radians(60.0f);
    f32 nearClip = 0.05f;
    f32 farClip = 500.0f;
    bool primary = true;
};

// Light travels along the entity's forward (-Z) axis.
struct DirectionalLightComponent {
    glm::vec3 color{1.0f, 0.96f, 0.9f};
    f32 intensity = 3.0f;
};

struct PointLightComponent {
    glm::vec3 color{1.0f};
    f32 intensity = 10.0f;
    f32 radius = 8.0f;
};

// ----------------------------------------------------------------- physics --
// A rigid body needs a collider component (Box/Sphere/Capsule). Without one, a box matching
// the entity's scale is used. Collider sizes are multiplied by the transform scale.
struct RigidBodyComponent {
    physics::BodyType type = physics::BodyType::Dynamic;
    f32 mass = 1.0f;
    f32 friction = 0.5f;
    f32 restitution = 0.1f;
    f32 linearDamping = 0.05f;
    f32 angularDamping = 0.05f;
    f32 gravityFactor = 1.0f;
    bool isSensor = false;

    physics::BodyId body; // runtime handle, valid while the scene is simulating
};

struct BoxColliderComponent {
    glm::vec3 halfExtents{0.5f};
    glm::vec3 offset{0.0f};
};

struct SphereColliderComponent {
    f32 radius = 0.5f;
    glm::vec3 offset{0.0f};
};

struct CapsuleColliderComponent {
    f32 radius = 0.5f;
    f32 halfHeight = 0.5f; // half length of the cylinder part, along local Y
    glm::vec3 offset{0.0f};
};

// Built-in behaviour: spins the entity around `axis`.
struct RotatorComponent {
    glm::vec3 axis{0.0f, 1.0f, 0.0f};
    f32 speed = 1.0f; // radians per second
};

} // namespace nova
