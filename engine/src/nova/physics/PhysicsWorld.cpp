#include "nova/physics/PhysicsWorld.h"

#include "nova/core/Log.h"
#include "nova/physics/FloatingOriginWorld.h"
#include "nova/physics/skeleton/SkeletonPhysicsWorld.h"

#if NOVA_PHYSICS_JOLT
    #include "nova/physics/jolt/JoltPhysicsWorld.h"
#endif

namespace nova::physics {

Scope<local::World> local::World::create(Backend backend, const WorldDesc& desc) {
    switch (backend) {
#if NOVA_PHYSICS_JOLT
        case Backend::Jolt: return makeScope<JoltPhysicsWorld>(desc);
#endif
        default: break;
    }
    NOVA_VERIFY(false, "Requested physics backend is not compiled in");
    return nullptr;
}

bool PhysicsWorld::isValidBody(const BodyDesc& desc) {
    const ShapeDesc& s = desc.shape;
    bool ok = false;
    switch (s.type) {
        case ShapeType::Box: ok = s.halfExtents.x > 0 && s.halfExtents.y > 0 && s.halfExtents.z > 0; break;
        case ShapeType::Sphere: ok = s.radius > 0; break;
        case ShapeType::Capsule:
        case ShapeType::Cylinder: ok = s.radius > 0 && s.halfHeight >= 0; break;
        case ShapeType::ConvexHull: ok = s.geometry && !s.geometry->points.empty(); break;
        case ShapeType::Mesh:
            if (desc.type == BodyType::Dynamic) {
                NOVA_CORE_ERROR("[physics] mesh shapes cannot be dynamic (use a convex hull)");
                return false;
            }
            ok = s.geometry && !s.geometry->points.empty() && !s.geometry->indices.empty() &&
                 s.geometry->indices.size() % 3 == 0;
            break;
    }
    if (!ok) NOVA_CORE_ERROR("[physics] invalid collision shape (type {})", static_cast<int>(s.type));
    return ok;
}

Scope<PhysicsWorld> PhysicsWorld::create(Backend backend, const WorldDesc& desc) {
    switch (backend) {
        // Float engines get a floating origin; backends working in global positions natively are
        // returned directly.
        case Backend::Jolt: return makeScope<FloatingOriginWorld>(backend, desc);
        case Backend::Skeleton: return makeScope<SkeletonPhysicsWorld>(desc);
    }
    NOVA_VERIFY(false, "Unknown physics backend");
    return nullptr;
}

} // namespace nova::physics
