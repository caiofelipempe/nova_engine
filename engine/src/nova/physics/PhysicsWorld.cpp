#include "nova/physics/PhysicsWorld.h"

#include "nova/core/Log.h"

// Frontend side of the contract. Creating worlds binds it to a backend: backend/Backends.cpp.

namespace nova::physics {

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
        case ShapeType::Compound: {
            ok = s.children && !s.children->empty();
            for (size_t i = 0; ok && i < s.children->size(); ++i) {
                BodyDesc part = desc;
                part.shape = (*s.children)[i];
                ok = part.shape.type != ShapeType::Compound && isValidBody(part);
            }
            if (!ok) NOVA_CORE_ERROR("[physics] invalid compound shape (empty, nested or with an invalid part)");
            return ok;
        }
    }
    if (!ok) NOVA_CORE_ERROR("[physics] invalid collision shape (type {})", static_cast<int>(s.type));
    return ok;
}

} // namespace nova::physics
