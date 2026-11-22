#include "nova/physics/PhysicsWorld.h"

#include "nova/core/Log.h"
#include "nova/physics/FloatingOriginWorld.h"

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

Scope<PhysicsWorld> PhysicsWorld::create(Backend backend, const WorldDesc& desc) {
    switch (backend) {
        // Float engines get a floating origin; a backend working in global positions natively would
        // be returned here directly (makeScope<MyIntegerWorld>(desc)).
        case Backend::Jolt: return makeScope<FloatingOriginWorld>(backend, desc);
    }
    NOVA_VERIFY(false, "Unknown physics backend");
    return nullptr;
}

} // namespace nova::physics
