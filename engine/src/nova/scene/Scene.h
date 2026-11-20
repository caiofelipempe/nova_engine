#pragma once

/// @file
/// @brief Scene: ECS registry, gameplay update, physics integration and scene queries.

#include "nova/renderer/Renderer.h"
#include "nova/scene/Entity.h"
#include "nova/scene/Script.h"

#include <optional>
#include <string>

namespace nova {

/// @brief Result of a Scene raycast or sweep.
/// @see physics::QueryHit for how the three normals differ on edges and corners.
struct SceneHit {
    Entity entity;                      ///< Hit entity.
    glm::vec3 point{0.0f};              ///< Contact point on the hit entity.
    glm::vec3 normal{0.0f};             ///< Contact normal (separation direction), pointing towards the query.
    glm::vec3 hitSurfaceNormal{0.0f};   ///< Hit entity's surface normal at `point`.
    glm::vec3 sweptPoint{0.0f};         ///< Sweeps: contact point on the swept shape (zero for raycasts).
    glm::vec3 sweptSurfaceNormal{0.0f}; ///< Sweeps: swept shape's surface normal at `sweptPoint` (zero for raycasts).
    f32 distance = 0.0f;                ///< Distance travelled until contact (0 = overlapping at the start).
};
using SceneRaycastHit = SceneHit; ///< Alias kept for readability at raycast call sites.

/// @brief A world of entities (EnTT registry) plus its optional physics simulation.
///
/// Each frame, update() runs native scripts, built-in systems (RotatorComponent) and — while
/// simulating — physics with a fixed timestep, writing dynamic bodies back into their transforms.
/// buildRenderScene() turns the ECS into a RenderScene for the Renderer.
///
/// @code
/// nova::Scene scene;
/// auto box = scene.createEntity("Box");
/// box.add<nova::MeshComponent>(renderer.cube());
/// box.add<nova::RigidBodyComponent>();
/// scene.startSimulation();
/// scene.update(dt);
/// @endcode
class Scene : NonCopyable {
public:
    /// @param name Display name.
    explicit Scene(std::string name = "Scene");
    /// @brief Destroys every entity (running Script::onDestroy) and the physics world.
    ~Scene();

    /// @name Entities
    /// @{
    /// @brief Creates an entity with an IdComponent (new random UUID), a NameComponent and a TransformComponent.
    Entity createEntity(const std::string& name = "Entity");
    /// @brief Destroys an entity: runs its scripts' onDestroy and removes its physics body. Invalid entities are ignored.
    void destroyEntity(Entity entity);
    /// @brief First entity with the given name, or an invalid Entity.
    Entity findEntity(const std::string& name);
    /// @brief The entity with this IdComponent UUID, or an invalid Entity.
    Entity findEntityByUuid(u64 uuid);
    /// @brief Destroys every entity (the physics world, if any, keeps running empty).
    void clear();
    /// @brief First entity whose CameraComponent is primary, or an invalid Entity.
    Entity primaryCamera();
    /// @}

    /// @brief Runs scripts, built-in systems and (if simulating) physics.
    /// @param dt Seconds since the previous update.
    void update(f32 dt);

    /// @name Physics
    /// @{
    /// @brief Creates the physics world and a body for every entity with a RigidBodyComponent.
    ///
    /// While simulating, bodies are also created for entities that gain a RigidBodyComponent and
    /// removed when the component or entity goes away.
    void startSimulation(physics::Backend backend = physics::Backend::Jolt);
    /// @brief Destroys the physics world. Transforms keep their simulated values.
    void stopSimulation();
    /// @brief True between startSimulation() and stopSimulation().
    bool isSimulating() const { return m_physics != nullptr; }
    /// @brief The underlying physics world, or `nullptr` when not simulating.
    physics::PhysicsWorld* physics() { return m_physics.get(); }
    /// @brief Physics step length in seconds (at most 4 steps run per update).
    f32 fixedTimeStep = 1.0f / 60.0f;

    /// @brief Applies a force (N) to the entity's body during the next step. No-op without a body.
    void addForce(Entity e, const glm::vec3& force);
    /// @brief Applies an instantaneous impulse (N·s) to the entity's body. No-op without a body.
    void addImpulse(Entity e, const glm::vec3& impulse);
    /// @brief Sets the linear velocity (m/s) of the entity's body. No-op without a body.
    void setLinearVelocity(Entity e, const glm::vec3& velocity);
    /// @brief Linear velocity (m/s) of the entity's body; zero without a body.
    glm::vec3 linearVelocity(Entity e);
    /// @brief Teleports the body to the entity's current transform and zeroes its velocity
    ///        (e.g. after moving it with an editor gizmo).
    void syncTransformToPhysics(Entity e);
    /// @}

    /// @name Queries
    /// All queries return `std::nullopt` when nothing is hit or when the scene is not simulating.
    /// `direction` need not be normalized. `ignore` skips one entity (typically the caller).
    /// @{
    /// @brief Closest entity hit by a ray.
    std::optional<SceneHit> raycast(const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance = 1000.0f,
                                    Entity ignore = {});
    /// @brief Sweeps a sphere and returns the first entity it would touch.
    std::optional<SceneHit> sweepSphere(const glm::vec3& origin, f32 radius, const glm::vec3& direction,
                                        f32 maxDistance = 1000.0f, Entity ignore = {});
    /// @brief Sweeps an oriented box and returns the first entity it would touch.
    std::optional<SceneHit> sweepBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
                                     const glm::vec3& direction, f32 maxDistance = 1000.0f, Entity ignore = {});
    /// @brief Sweeps a capsule (axis = local Y of `rotation`) and returns the first entity it would touch.
    std::optional<SceneHit> sweepCapsule(const glm::vec3& center, f32 radius, f32 halfHeight,
                                         const glm::quat& rotation, const glm::vec3& direction,
                                         f32 maxDistance = 1000.0f, Entity ignore = {});
    /// @brief Sweeps any shape and returns the first entity it would touch.
    std::optional<SceneHit> sweep(const physics::ShapeDesc& shape, const glm::vec3& position,
                                  const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance = 1000.0f,
                                  Entity ignore = {});
    /// @brief Sweeps any shape and returns every entity along the way, closest first.
    std::vector<SceneHit> sweepAll(const physics::ShapeDesc& shape, const glm::vec3& position,
                                   const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance = 1000.0f,
                                   Entity ignore = {});
    /// @brief Sweeps the entity's own collider from where it is now, ignoring itself.
    ///
    /// Answers "if I move this way, what do I hit first?". Requires a RigidBodyComponent.
    std::optional<SceneHit> sweepEntity(Entity entity, const glm::vec3& direction, f32 maxDistance = 1000.0f);
    /// @}

    /// @brief Fills `out` with the camera, lights and meshes of the scene.
    /// @param out Reused draw list (cleared first).
    /// @param aspect Width / height of the render target.
    /// @param cameraOverride Use this camera instead of the primary CameraComponent (e.g. an editor camera).
    /// @return False if there is no camera (nothing should be rendered).
    bool buildRenderScene(RenderScene& out, f32 aspect, const CameraData* cameraOverride = nullptr);

    /// @brief The EnTT registry, for custom components and systems.
    entt::registry& registry() { return m_registry; }
    /// @brief Shortcut for `registry().view<C...>()`.
    /// @code
    /// for (auto [e, t, hp] : scene.view<nova::TransformComponent, Health>().each()) { ... }
    /// @endcode
    template <typename... C>
    auto view() { return m_registry.view<C...>(); }

    /// @brief Display name.
    const std::string& name() const { return m_name; }
    /// @brief Renames the scene.
    void setName(std::string name) { m_name = std::move(name); }
    /// @brief Accumulated update time in seconds.
    f32 time() const { return m_time; }

private:
    void stepPhysics(f32 dt);
    void createBody(entt::entity e);
    void onRigidBodyDestroyed(entt::registry& registry, entt::entity e);
    physics::BodyId bodyOf(Entity e);
    physics::QueryFilter filterIgnoring(Entity e);
    SceneHit toSceneHit(const physics::QueryHit& hit);

    std::string m_name;
    entt::registry m_registry;
    Scope<physics::PhysicsWorld> m_physics; // declared after the registry: destroyed first
    f32 m_time = 0.0f;
    f32 m_physicsAccumulator = 0.0f;
};

} // namespace nova
