#pragma once

#include "nova/renderer/Renderer.h"
#include "nova/scene/Entity.h"
#include "nova/scene/Script.h"

#include <optional>
#include <span>
#include <string>

namespace nova {

// Result of a scene raycast / sweep.
struct SceneHit {
    Entity entity;
    glm::vec3 point{0.0f};  // contact point on the hit entity
    glm::vec3 normal{0.0f}; // surface normal, pointing back towards the query
    f32 distance = 0.0f;    // distance travelled until contact (0 = overlapping at start)
};
using SceneRaycastHit = SceneHit;

class Scene : NonCopyable {
public:
    explicit Scene(std::string name = "Scene");
    ~Scene();

    // Every entity gets a NameComponent and a TransformComponent.
    Entity createEntity(const std::string& name = "Entity");
    void destroyEntity(Entity entity);
    Entity findEntity(const std::string& name);
    void clear();

    // Runs scripts, built-in systems and (if simulating) physics.
    void update(f32 dt);

    // ---- physics
    // Creates the physics world; bodies are created lazily for every entity with a RigidBodyComponent.
    void startSimulation(physics::Backend backend = physics::Backend::Jolt);
    void stopSimulation();
    bool isSimulating() const { return m_physics != nullptr; }
    physics::PhysicsWorld* physics() { return m_physics.get(); }
    f32 fixedTimeStep = 1.0f / 60.0f;

    void addForce(Entity e, const glm::vec3& force);
    void addImpulse(Entity e, const glm::vec3& impulse);
    void setLinearVelocity(Entity e, const glm::vec3& velocity);
    glm::vec3 linearVelocity(Entity e);
    // Pushes the ECS transform into the body (teleport), e.g. after editing it in a gizmo.
    void syncTransformToPhysics(Entity e);

    // ---- queries (require isSimulating()); `ignore` skips one entity, e.g. the caller
    std::optional<SceneHit> raycast(const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance = 1000.0f,
                                    Entity ignore = {});
    // Sweep tests: move a shape along `direction` and report what it would hit first.
    std::optional<SceneHit> sweepSphere(const glm::vec3& origin, f32 radius, const glm::vec3& direction,
                                        f32 maxDistance = 1000.0f, Entity ignore = {});
    std::optional<SceneHit> sweepBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
                                     const glm::vec3& direction, f32 maxDistance = 1000.0f, Entity ignore = {});
    std::optional<SceneHit> sweepCapsule(const glm::vec3& center, f32 radius, f32 halfHeight,
                                         const glm::quat& rotation, const glm::vec3& direction,
                                         f32 maxDistance = 1000.0f, Entity ignore = {});
    std::optional<SceneHit> sweep(const physics::ShapeDesc& shape, const glm::vec3& position,
                                  const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance = 1000.0f,
                                  Entity ignore = {});
    // Every entity along the sweep, closest first.
    std::vector<SceneHit> sweepAll(const physics::ShapeDesc& shape, const glm::vec3& position,
                                   const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance = 1000.0f,
                                   Entity ignore = {});
    // Sweeps the entity's own collider from where it is now ("if I move this way, what do I hit?").
    std::optional<SceneHit> sweepEntity(Entity entity, const glm::vec3& direction, f32 maxDistance = 1000.0f);

    // Sweeps tested only against the given entities (those without a rigid body are skipped).
    std::optional<SceneHit> sweepAgainst(const physics::ShapeDesc& shape, const glm::vec3& position,
                                         const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                         std::span<const Entity> targets);
    std::vector<SceneHit> sweepAllAgainst(const physics::ShapeDesc& shape, const glm::vec3& position,
                                          const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                          std::span<const Entity> targets);

    // Fills `out` from the ECS. Uses the primary CameraComponent unless `cameraOverride` is set
    // (e.g. an editor camera). Returns false if there is no camera.
    bool buildRenderScene(RenderScene& out, f32 aspect, const CameraData* cameraOverride = nullptr);

    Entity primaryCamera();

    entt::registry& registry() { return m_registry; }
    template <typename... C>
    auto view() { return m_registry.view<C...>(); }

    const std::string& name() const { return m_name; }
    f32 time() const { return m_time; }

private:
    void stepPhysics(f32 dt);
    void createBody(entt::entity e);
    void onRigidBodyDestroyed(entt::registry& registry, entt::entity e);
    physics::BodyId bodyOf(Entity e);
    physics::QueryFilter filterIgnoring(Entity e);
    SceneHit toSceneHit(const physics::QueryHit& hit);
    std::vector<physics::BodyId> bodiesOf(std::span<const Entity> entities);

    std::string m_name;
    entt::registry m_registry;
    Scope<physics::PhysicsWorld> m_physics; // declared after the registry: destroyed first
    f32 m_time = 0.0f;
    f32 m_physicsAccumulator = 0.0f;
};

} // namespace nova
