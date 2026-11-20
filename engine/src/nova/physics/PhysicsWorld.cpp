#include "nova/physics/PhysicsWorld.h"

#include "nova/core/Log.h"

#if NOVA_PHYSICS_JOLT
    #include "nova/physics/jolt/JoltPhysicsWorld.h"
#endif

namespace nova::physics {

Scope<PhysicsWorld> PhysicsWorld::create(Backend backend, const WorldDesc& desc) {
    switch (backend) {
#if NOVA_PHYSICS_JOLT
        case Backend::Jolt: return makeScope<JoltPhysicsWorld>(desc);
#endif
        default: break;
    }
    NOVA_VERIFY(false, "Requested physics backend is not compiled in");
    return nullptr;
}

} // namespace nova::physics
