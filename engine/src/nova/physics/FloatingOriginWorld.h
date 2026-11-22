#pragma once

/// @file
/// @brief Global-position physics on top of float engines: floating origins, regions, dormancy.

#include "nova/physics/LocalPhysicsWorld.h"
#include "nova/physics/PhysicsWorld.h"

#include <vector>

namespace nova::physics {

/// @brief Implements the global PhysicsWorld contract with one or more float local::World (Jolt).
///
/// Float engines are precise only near their origin, so this adapter is where global positions
/// become floats — and nowhere else:
/// - **Regions:** foci closer than 2 × SimulationFocus::radius share a region, a local world whose
///   origin follows the centroid of its foci (rebased when they drift past `rebaseDistance`, by an
///   amount floats represent exactly). Far-apart foci get separate regions. Radius 0 or no foci:
///   one region holding everything.
/// - **Bodies:** a dynamic/kinematic body lives in the region of its nearest focus if within reach;
///   otherwise it is **dormant** (no local body, pose and velocity kept) until a focus comes back.
///   Static bodies join every region they reach. Moving between regions keeps the velocity.
/// - **Barriers:** barrier edges of the active WorldSpace near a region become static walls;
///   contacts/hits against them carry kWorldBoundaryUserData.
/// - Queries run in the region around their origin; results are converted back to global.
/// All decisions follow body index and focus order, so the simulation stays deterministic.
class FloatingOriginWorld final : public PhysicsWorld {
public:
    /// @param backend Float backend creating the local worlds.
    /// @param desc Parameters of every local world.
    FloatingOriginWorld(Backend backend, const WorldDesc& desc);
    ~FloatingOriginWorld() override;

    PhysicsCaps caps() const override;

    BodyId createBody(const BodyDesc& desc) override;
    void destroyBody(BodyId body) override;
    u32 bodyCount() const override;
    void setTransform(BodyId body, const GlobalPosition& position, const glm::quat& rotation) override;
    void getTransform(BodyId body, GlobalPosition& position, glm::quat& rotation) const override;
    void moveKinematic(BodyId body, const GlobalPosition& position, const glm::quat& rotation, Duration dt) override;

    glm::vec3 linearVelocity(BodyId body) const override;
    void setLinearVelocity(BodyId body, const glm::vec3& velocity) override;
    glm::vec3 angularVelocity(BodyId body) const override;
    void setAngularVelocity(BodyId body, const glm::vec3& velocity) override;
    void addForce(BodyId body, const glm::vec3& force) override;
    void addImpulse(BodyId body, const glm::vec3& impulse) override;
    void addTorque(BodyId body, const glm::vec3& torque) override;
    bool isActive(BodyId body) const override;

    void setGravity(const glm::vec3& gravity) override;
    glm::vec3 gravity() const override { return m_gravity; }
    void step(Duration dt) override;

    void setFocus(const SimulationFocus& focus) override;
    bool isDormant(BodyId body) const override;
    u32 regionCount() const override { return static_cast<u32>(m_regions.size()); }
    GlobalPosition regionOrigin(u32 region) const override;

    bool raycast(const GlobalPosition& origin, const glm::vec3& direction, f32 maxDistance, RaycastHit& hit,
                 const QueryFilter& filter) const override;
    bool sweep(const ShapeDesc& shape, const GlobalPosition& position, const glm::quat& rotation,
               const glm::vec3& direction, f32 maxDistance, SweepHit& hit, const QueryFilter& filter) const override;
    std::vector<SweepHit> sweepAll(const ShapeDesc& shape, const GlobalPosition& position, const glm::quat& rotation,
                                   const glm::vec3& direction, f32 maxDistance,
                                   const QueryFilter& filter) const override;
    bool sweepBody(BodyId body, const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                   const QueryFilter& filter) const override;
    std::vector<ContactEvent> drainContactEvents() override;

private:
    struct Region {
        u32 id = 0;
        Scope<local::World> world;
        GlobalPosition origin;
        std::vector<GlobalPosition> foci;
        std::vector<BodyId> walls; // local ids
        glm::vec3 toLocal(const GlobalPosition& p) const { return (p - origin).toVec3(); }
        GlobalPosition toGlobal(const glm::vec3& p) const { return origin + p; }
    };
    struct Instance {
        u32 region = 0;
        BodyId local;
    };
    struct Body {
        bool alive = false;
        BodyDesc desc;                // pose = last known (kept while dormant)
        std::vector<Instance> instances; // dynamic/kinematic: at most one; static: any
        glm::vec3 savedLinear{0.0f};  // velocity kept while dormant / between regions
        glm::vec3 savedAngular{0.0f};
    };

    Region& createRegion(const GlobalPosition& origin);
    void destroyRegion(size_t index);
    void rebaseRegion(Region& region, const GlobalPosition& target);
    void rebuildWalls(Region& region);
    void rewrapBodies();
    void updateRegions();
    void assign(u32 index);
    void removeInstance(u32 index, size_t instance);
    void createInstance(u32 index, Region& region);
    Region* findRegion(u32 id);
    const Region* findRegion(u32 id) const;
    const Region* regionAt(const GlobalPosition& p) const;
    const Body* body(BodyId id) const;
    Body* body(BodyId id);
    const Instance* home(const Body& b) const { return b.instances.empty() ? nullptr : &b.instances.front(); }
    GlobalPosition currentPosition(u32 index) const;
    QueryHit toGlobal(const local::QueryHit& hit, const Region& region, const GlobalPosition& queryOrigin,
                      bool ray) const;
    QueryFilter toLocal(const QueryFilter& filter, const Region& region) const;

    Backend m_backend;
    WorldDesc m_desc;
    glm::vec3 m_gravity;
    SimulationFocus m_focus;
    bool m_bounded = false;
    std::vector<Region> m_regions;
    u32 m_nextRegionId = 1;
    std::vector<Body> m_bodies;
    std::vector<u32> m_freeBodies;
    std::vector<ContactEvent> m_events;
};

} // namespace nova::physics
