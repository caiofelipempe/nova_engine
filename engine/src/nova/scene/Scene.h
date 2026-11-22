#pragma once

/// @file
/// @brief Scene: ECS registry, gameplay update, physics integration and scene queries.

#include "nova/math/Time.h"
#include "nova/renderer/Renderer.h"
#include "nova/scene/Entity.h"
#include "nova/scene/Script.h"

#include <optional>
#include <set>
#include <tuple>
#include <map>
#include <functional>
#include <unordered_map>
#include <string>

namespace nova {

/// @brief Result of a Scene raycast or sweep.
/// @see physics::QueryHit for how the three normals differ on edges and corners.
struct SceneHit {
    Entity entity;                      ///< Hit entity.
    GlobalPosition point;               ///< Contact point on the hit entity.
    glm::vec3 normal{0.0f};             ///< Contact normal (separation direction), pointing towards the query.
    glm::vec3 hitSurfaceNormal{0.0f};   ///< Hit entity's surface normal at `point`.
    GlobalPosition sweptPoint;          ///< Sweeps: contact point on the swept shape (the query origin for raycasts).
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
    /// @brief The entity an EntityRef points to, or an invalid Entity.
    Entity resolve(EntityRef ref) { return findEntityByUuid(ref.uuid); }
    /// @brief A saved reference to `entity` (none for an invalid entity).
    static EntityRef refOf(Entity entity);
    /// @brief Destroys every entity (the physics world, if any, keeps running empty).
    void clear();
    /// @brief clear() and also destroys every component *storage* of the ECS registry.
    ///
    /// Component pools are created by whichever module first uses a component type, and their
    /// code (vtables) lives in that module: call this before unloading a game DLL.
    void resetStorage();
    /// @brief First entity whose CameraComponent is primary, or an invalid Entity.
    Entity primaryCamera();
    /// @}

    /// @brief Runs scripts, built-in systems and (if simulating) physics.
    /// @param dt Seconds since the previous update.
    void update(f32 dt) { update(Duration::fromSeconds(dt)); }
    /// @brief Same, with an exact frame duration (GameLayer passes Application::frameDuration()).
    void update(Duration dt);

    /// @name Physics
    /// @{
    /// @brief Creates the physics world and a body for every entity with a RigidBodyComponent.
    ///
    /// While simulating, bodies are also created for entities that gain a RigidBodyComponent and
    /// removed when the component or entity goes away.
    void startSimulation(physics::Backend backend = physics::Backend::Jolt);
    /// @brief Where mesh colliders (convex hull, triangle mesh) read vertices from.
    using MeshSource = std::function<const MeshData*(MeshHandle)>;
    /// @brief Sets this scene's mesh source (falls back to the default one when unset).
    void setMeshSource(MeshSource source) { m_meshSource = std::move(source); }
    /// @brief Default mesh source of every scene; Application sets it to Renderer::meshData.
    static void setDefaultMeshSource(MeshSource source);
    /// @brief Same, with a physics world you created — e.g. your own engine implementing
    ///        physics::PhysicsWorld in global positions natively.
    void startSimulation(Scope<physics::PhysicsWorld> world);
    /// @brief Destroys the physics world. Transforms keep their simulated values.
    void stopSimulation();
    /// @brief True between startSimulation() and stopSimulation().
    bool isSimulating() const { return m_physics != nullptr; }
    /// @brief The physics world (global positions), or `nullptr` when not simulating.
    physics::PhysicsWorld* physics() { return m_physics.get(); }
    /// @brief Physics step length (at most 4 steps run per update). Exact: the accumulator counts
    ///        whole flicks, so the simulation never drifts from the scene clock.
    Duration fixedTimeStep = Duration::perSecond(60);

    /// @brief Applies a force (N) to the entity's body during the next step. No-op without a body.
    void addForce(Entity e, const ForceVec& force);
    /// @brief Applies an instantaneous impulse (N·s) to the entity's body. No-op without a body.
    void addImpulse(Entity e, const ForceVec& impulse);
    /// @brief Sets the linear velocity (m/s) of the entity's body. No-op without a body.
    void setLinearVelocity(Entity e, const SpeedVec& velocity);
    /// @brief Linear velocity (m/s) of the entity's body; zero without a body.
    SpeedVec linearVelocity(Entity e);
    /// @brief Teleports the body to the entity's current transform and zeroes its velocity
    ///        (e.g. after moving it with an editor gizmo).
    void syncTransformToPhysics(Entity e);

    /// @brief Where the action is (e.g. the player): the physics keeps the seam of the torus away
    ///        from it. Without a focus, the primary camera is the focus.
    void setPhysicsFocus(const GlobalPosition& focus) {
        m_physicsFocus = focus;
        m_hasPhysicsFocus = true;
    }
    /// @brief Goes back to following the primary camera.
    void clearPhysicsFocus() { m_hasPhysicsFocus = false; }
    /// @brief What the physics engine's float origin is on the torus (it follows the focus).
    GlobalPosition physicsOrigin() const { return m_physics ? m_physics->origin() : GlobalPosition(); }
    /// @brief Number of physics bodies.
    u32 physicsBodyCount() const { return m_physics ? m_physics->bodyCount() : 0; }
    /// @brief Gravity, collision layers and their names (saved with the scene).
    const PhysicsSettings& physicsSettings() const { return m_physicsSettings; }
    /// @brief Changes the physics settings (applied to the running simulation right away).
    void setPhysicsSettings(const PhysicsSettings& settings);
    /// @}

    /// @name World edges
    /// The world's size and edges per axis (WorldBounds): the whole 16 km torus by default, or a
    /// smaller world whose edges wrap around (circular) or are barriers (walls). Wraps act on entity
    /// centers outside the physics engine; barriers are static walls while simulating, plus a
    /// stop on centers for what they miss. Saved with the scene.
    /// @{
    /// @brief The world's size and edges.
    const WorldBounds& worldBounds() const { return m_worldBounds; }
    /// @brief Changes them (sizes are brought into range: WorldBounds::valid()); the walls of a
    ///        running simulation are rebuilt.
    void setWorldBounds(const WorldBounds& bounds);

    /// @brief Hash of the simulation state: every entity's exact position, rotation, scale and body
    ///        velocities, in UUID order. Two runs of the same scene with the same inputs give the same
    ///        hash step after step (lockstep / replay desync checks).
    u64 stateHash();
    /// @}

    /// @name Queries
    /// All queries return `std::nullopt` / nothing when nothing is hit or when the scene is not
    /// simulating. `direction` need not be normalized. `ignore` skips one entity (typically the
    /// caller); `layerMask` keeps only bodies of those layers (physics::layerBit(),
    /// PhysicsSettings::maskOf()). Sensors are never reported.
    /// @{
    /// @brief Closest entity hit by a ray.
    std::optional<SceneHit> raycast(const GlobalPosition& origin, const glm::vec3& direction, f32 maxDistance = 1000.0f,
                                    Entity ignore = {}, u32 layerMask = physics::kAllLayers);
    /// @brief Sweeps a sphere and returns the first entity it would touch.
    std::optional<SceneHit> sweepSphere(const GlobalPosition& origin, f32 radius, const glm::vec3& direction,
                                        f32 maxDistance = 1000.0f, Entity ignore = {}, u32 layerMask = physics::kAllLayers);
    /// @brief Sweeps an oriented box and returns the first entity it would touch.
    std::optional<SceneHit> sweepBox(const GlobalPosition& center, const glm::vec3& halfExtents, const glm::quat& rotation,
                                     const glm::vec3& direction, f32 maxDistance = 1000.0f, Entity ignore = {},
                                     u32 layerMask = physics::kAllLayers);
    /// @brief Sweeps a capsule (axis = local Y of `rotation`) and returns the first entity it would touch.
    std::optional<SceneHit> sweepCapsule(const GlobalPosition& center, f32 radius, f32 halfHeight,
                                         const glm::quat& rotation, const glm::vec3& direction,
                                         f32 maxDistance = 1000.0f, Entity ignore = {},
                                         u32 layerMask = physics::kAllLayers);
    /// @brief Sweeps any shape and returns the first entity it would touch.
    std::optional<SceneHit> sweep(const physics::ShapeDesc& shape, const GlobalPosition& position,
                                  const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance = 1000.0f,
                                  Entity ignore = {}, u32 layerMask = physics::kAllLayers);
    /// @brief Sweeps any shape and returns every entity along the way, closest first.
    std::vector<SceneHit> sweepAll(const physics::ShapeDesc& shape, const GlobalPosition& position,
                                   const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance = 1000.0f,
                                   Entity ignore = {}, u32 layerMask = physics::kAllLayers);
    /// @brief Sweeps the entity's own collider from where it is now, ignoring itself.
    ///
    /// Answers "if I move this way, what do I hit first?". Requires a RigidBodyComponent.
    std::optional<SceneHit> sweepEntity(Entity entity, const glm::vec3& direction, f32 maxDistance = 1000.0f,
                                        u32 layerMask = physics::kAllLayers);
    /// @brief Every entity whose collider overlaps `shape` at this pose ("what is inside this area?").
    /// @param includeSensors Report sensor bodies too.
    std::vector<Entity> overlap(const physics::ShapeDesc& shape, const GlobalPosition& position,
                                const glm::quat& rotation, Entity ignore = {}, u32 layerMask = physics::kAllLayers,
                                bool includeSensors = false);
    /// @brief Every entity whose collider overlaps a sphere.
    std::vector<Entity> overlapSphere(const GlobalPosition& center, f32 radius, Entity ignore = {},
                                      u32 layerMask = physics::kAllLayers, bool includeSensors = false);
    /// @brief Every entity whose collider overlaps an oriented box.
    std::vector<Entity> overlapBox(const GlobalPosition& center, const glm::vec3& halfExtents, const glm::quat& rotation,
                                   Entity ignore = {}, u32 layerMask = physics::kAllLayers, bool includeSensors = false);
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
    /// @brief Scene clock: exact sum of every update's duration.
    TimePoint time() const { return m_time; }
    /// @brief Period of the time given to shaders (`nova_time()`): it restarts every hour so a float
    ///        keeps sub-millisecond precision however long the game runs.
    static constexpr Duration kShaderTimeCycle = Duration::hours(1);

private:
    void stepPhysics(Duration dt);
    GlobalPosition currentFocus();
    void pushPhysicsFocus();
    void createBody(entt::entity e);
    physics::BodyDesc bodyDescFor(entt::entity e);
    void syncBodies();
    void syncJoints();
    void syncCharacters();
    void readCharacters();
    void createCharacter(entt::entity e);
    void onCharacterDestroyed(entt::registry& registry, entt::entity e);
    void createJoint(entt::entity e);
    void destroyJoint(JointComponent& jc);
    void onJointDestroyed(entt::registry& registry, entt::entity e);
    void onRigidBodyDestroyed(entt::registry& registry, entt::entity e);
    void connectSignals();
    physics::BodyId bodyOf(Entity e);
    const MeshData* meshData(MeshHandle mesh) const;
    std::shared_ptr<const physics::ShapeGeometry> meshGeometry(MeshHandle mesh, const glm::vec3& scale, bool triangles);
    physics::QueryFilter filterIgnoring(Entity e, u32 layerMask = physics::kAllLayers) const;
    void dispatchContacts();
    SceneHit toSceneHit(const physics::QueryHit& hit);
    void constrainEntities();
    void constrainSimulated();
    void recordEdgeHits(entt::entity e, u32 changed, const std::array<WorldEdgeHit, 3>& hits);
    void dispatchEdgeHits();
    void buildEdgeWalls();

    std::string m_name;
    entt::registry m_registry;
    // Physics, in global positions (declared after the registry: destroyed first).
    Scope<physics::PhysicsWorld> m_physics;
    MeshSource m_meshSource;
    // Collider geometry per (mesh, scale, kind): bodies sharing a mesh share one geometry.
    std::map<std::tuple<u32, u32, u32, u32, bool>, std::shared_ptr<const physics::ShapeGeometry>> m_geometry;
    TimePoint m_time;
    PhysicsSettings m_physicsSettings;
    std::set<entt::entity> m_warnedMeshOnDynamic, m_warnedNoVertices; // warn once per entity
    GlobalPosition m_physicsFocus;
    bool m_hasPhysicsFocus = false;
    Duration m_physicsAccumulator;
    WorldBounds m_worldBounds;
    std::vector<physics::BodyId> m_edgeWalls; // barrier walls while simulating
    std::vector<std::pair<entt::entity, WorldEdgeHit>> m_edgeHits; // reported after the update
};

} // namespace nova
