// Reflection of the engine's built-in types. Field names are the identifiers stored in scene
// files: renaming one breaks existing files.
#include "nova/reflect/Reflect.h"

#include "nova/scene/Components.h"

namespace nova::reflect::detail {

void registerEngineTypes() {
    enumeration<physics::BodyType>("BodyType")
        .value("Static", physics::BodyType::Static)
        .value("Kinematic", physics::BodyType::Kinematic)
        .value("Dynamic", physics::BodyType::Dynamic);

    enumeration<physics::JointType>("JointType")
        .value("Fixed", physics::JointType::Fixed)
        .value("Point", physics::JointType::Point)
        .value("Hinge", physics::JointType::Hinge)
        .value("Slider", physics::JointType::Slider)
        .value("Distance", physics::JointType::Distance);
    enumeration<physics::GroundState>("GroundState")
        .value("OnGround", physics::GroundState::OnGround)
        .value("OnSteepGround", physics::GroundState::OnSteepGround)
        .value("NotSupported", physics::GroundState::NotSupported)
        .value("InAir", physics::GroundState::InAir);
    enumeration<physics::JointMotorMode>("JointMotorMode")
        .value("Off", physics::JointMotorMode::Off)
        .value("Velocity", physics::JointMotorMode::Velocity)
        .value("Position", physics::JointMotorMode::Position);

    type<Material>("Material")
        .field("baseColor", &Material::baseColor, {.color = true})
        .field("metallic", &Material::metallic, {.min = 0.0f, .max = 1.0f})
        .field("roughness", &Material::roughness, {.min = 0.0f, .max = 1.0f})
        .field("emissive", &Material::emissive, {.color = true})
        .field("emissiveStrength", &Material::emissiveStrength, {.min = 0.0f, .max = 100.0f})
        .field("checker", &Material::checker, {.label = "Checker Pattern"})
        .field("baseColorTexture", &Material::baseColorTexture, {.label = "Base Color Texture"})
        .field("shader", &Material::shader, {.tooltip = "Game material shader (nova/material.glsl); empty = default shading"})
        .field("customParams", &Material::customParams, {.speed = 0.01f, .tooltip = "Surface::custom in the material shader"})
        .field("customTexture", &Material::customTexture, {.tooltip = "nova_customTexture(uv) in the material shader"});

    // Identity and name: always present, edited specially (name at the top of the inspector).
    type<IdComponent>("Id").field("uuid", &IdComponent::uuid, {.readOnly = true}).component().hideInInspector();
    type<NameComponent>("Name").field("name", &NameComponent::name).component().hideInInspector();

    type<TransformComponent>("Transform")
        .field("position", &TransformComponent::position)
        .field("rotation", &TransformComponent::rotation, {.speed = 0.5f})
        .field("scale", &TransformComponent::scale, {.speed = 0.02f})
        .component();

    type<MeshComponent>("Mesh")
        .field("mesh", &MeshComponent::mesh)
        .field("castShadows", &MeshComponent::castShadows)
        .field("material", &MeshComponent::material)
        .component();

    type<CameraComponent>("Camera")
        .field("fovY", &CameraComponent::fovY, {.min = 10.0f, .max = 120.0f, .degrees = true, .label = "Field of View"})
        .field("nearClip", &CameraComponent::nearClip, {.min = 0.001f, .max = 10.0f})
        .field("farClip", &CameraComponent::farClip, {.speed = 1.0f})
        .field("primary", &CameraComponent::primary)
        .component();

    type<DirectionalLightComponent>("DirectionalLight")
        .field("color", &DirectionalLightComponent::color, {.color = true})
        .field("intensity", &DirectionalLightComponent::intensity, {.speed = 0.05f})
        .component();

    type<PointLightComponent>("PointLight")
        .field("color", &PointLightComponent::color, {.color = true})
        .field("intensity", &PointLightComponent::intensity, {.speed = 0.1f})
        .field("radius", &PointLightComponent::radius, {.speed = 0.05f})
        .component();

    type<RigidBodyLocks>("RigidBodyLocks")
        .field("positionX", &RigidBodyLocks::positionX)
        .field("positionY", &RigidBodyLocks::positionY)
        .field("positionZ", &RigidBodyLocks::positionZ)
        .field("rotationX", &RigidBodyLocks::rotationX)
        .field("rotationY", &RigidBodyLocks::rotationY)
        .field("rotationZ", &RigidBodyLocks::rotationZ);

    type<RigidBodyComponent>("RigidBody")
        .field("type", &RigidBodyComponent::type)
        .field("mass", &RigidBodyComponent::mass, {.speed = 0.05f, .tooltip = "kg (dynamic bodies)"})
        .field("friction", &RigidBodyComponent::friction, {.min = 0.0f, .max = 1.0f})
        .field("restitution", &RigidBodyComponent::restitution, {.min = 0.0f, .max = 1.0f, .tooltip = "Bounciness"})
        .field("linearDamping", &RigidBodyComponent::linearDamping, {.min = 0.0f, .max = 1.0f})
        .field("angularDamping", &RigidBodyComponent::angularDamping, {.min = 0.0f, .max = 1.0f})
        .field("gravityFactor", &RigidBodyComponent::gravityFactor, {.speed = 0.01f})
        .field("isSensor", &RigidBodyComponent::isSensor, {.tooltip = "Reports overlaps (onTriggerEnter/Exit), no collision response", .label = "Sensor"})
        .field("layer", &RigidBodyComponent::layer, {.tooltip = "Collision layer (Physics settings say which collide)"})
        .field("continuousCollision", &RigidBodyComponent::continuousCollision,
               {.tooltip = "Fast bodies (bullets) never tunnel through thin walls", .label = "Continuous Collision"})
        .field("locks", &RigidBodyComponent::locks, {.tooltip = "Dynamic bodies: axes they may not move or turn along"})
        .field("body", &RigidBodyComponent::body, {.transient = true})
        .field("appliedShape", &RigidBodyComponent::appliedShape, {.transient = true})
        .field("appliedProperties", &RigidBodyComponent::appliedProperties, {.transient = true})
        .field("appliedPose", &RigidBodyComponent::appliedPose, {.transient = true})
        .component();

    type<BoxColliderComponent>("BoxCollider")
        .field("halfExtents", &BoxColliderComponent::halfExtents, {.speed = 0.01f})
        .field("offset", &BoxColliderComponent::offset, {.speed = 0.01f})
        .field("rotation", &BoxColliderComponent::rotation, {.speed = 0.5f})
        .component();
    type<SphereColliderComponent>("SphereCollider")
        .field("radius", &SphereColliderComponent::radius, {.speed = 0.01f})
        .field("offset", &SphereColliderComponent::offset, {.speed = 0.01f})
        .component();
    type<CylinderColliderComponent>("CylinderCollider")
        .field("radius", &CylinderColliderComponent::radius, {.speed = 0.01f})
        .field("halfHeight", &CylinderColliderComponent::halfHeight, {.speed = 0.01f})
        .field("offset", &CylinderColliderComponent::offset, {.speed = 0.01f})
        .field("rotation", &CylinderColliderComponent::rotation, {.speed = 0.5f})
        .component();
    type<ConvexHullColliderComponent>("ConvexHullCollider")
        .field("mesh", &ConvexHullColliderComponent::mesh, {.tooltip = "Empty = the entity's mesh"})
        .field("offset", &ConvexHullColliderComponent::offset, {.speed = 0.01f})
        .field("rotation", &ConvexHullColliderComponent::rotation, {.speed = 0.5f})
        .component();
    type<MeshColliderComponent>("MeshCollider")
        .field("mesh", &MeshColliderComponent::mesh, {.tooltip = "Empty = the entity's mesh. Static/kinematic bodies only"})
        .component();
    type<CapsuleColliderComponent>("CapsuleCollider")
        .field("radius", &CapsuleColliderComponent::radius, {.speed = 0.01f})
        .field("halfHeight", &CapsuleColliderComponent::halfHeight, {.speed = 0.01f})
        .field("offset", &CapsuleColliderComponent::offset, {.speed = 0.01f})
        .field("rotation", &CapsuleColliderComponent::rotation, {.speed = 0.5f})
        .component();

    type<JointComponent>("Joint")
        .field("type", &JointComponent::type)
        .field("connected", &JointComponent::connected, {.tooltip = "The other body (needs a RigidBody); none = the world"})
        .field("anchor", &JointComponent::anchor, {.speed = 0.01f, .tooltip = "Pivot, in this entity's local space"})
        .field("axis", &JointComponent::axis, {.speed = 0.01f, .tooltip = "Hinge axis / slider direction (local)"})
        .field("connectedAnchor", &JointComponent::connectedAnchor,
               {.speed = 0.01f, .tooltip = "Distance: attachment on the other entity (local), or offset from here to the world point"})
        .field("limitsEnabled", &JointComponent::limitsEnabled, {.label = "Limits"})
        .field("limitMin", &JointComponent::limitMin, {.speed = 0.1f, .tooltip = "Hinge: degrees; slider: meters"})
        .field("limitMax", &JointComponent::limitMax, {.speed = 0.1f, .tooltip = "Hinge: degrees; slider: meters"})
        .field("minDistance", &JointComponent::minDistance, {.speed = 0.01f, .tooltip = "Distance joints (m); -1 = at start"})
        .field("maxDistance", &JointComponent::maxDistance, {.speed = 0.01f, .tooltip = "Distance joints (m); -1 = at start"})
        .field("springFrequency", &JointComponent::springFrequency, {.speed = 0.05f, .tooltip = "Distance joints: softness (Hz; 0 = rigid)"})
        .field("springDamping", &JointComponent::springDamping, {.min = 0.0f, .max = 1.0f})
        .field("motorMode", &JointComponent::motorMode)
        .field("motorTarget", &JointComponent::motorTarget, {.speed = 0.1f, .tooltip = "Hinge: deg/s or degrees; slider: m/s or meters"})
        .field("motorMaxForce", &JointComponent::motorMaxForce, {.speed = 1.0f, .tooltip = "Hinge: N·m; slider: N; 0 = unlimited"})
        .field("breakForce", &JointComponent::breakForce, {.speed = 1.0f, .tooltip = "N; 0 = unbreakable"})
        .field("breakTorque", &JointComponent::breakTorque, {.speed = 1.0f, .tooltip = "N·m; 0 = unbreakable"})
        .field("broken", &JointComponent::broken, {.tooltip = "Clear to build the joint again"})
        .field("joint", &JointComponent::joint, {.transient = true})
        .field("connectedBody", &JointComponent::connectedBody, {.transient = true})
        .field("appliedKey", &JointComponent::appliedKey, {.transient = true})
        .field("appliedMotor", &JointComponent::appliedMotor, {.transient = true})
        .component();

    type<CharacterControllerComponent>("CharacterController")
        .field("radius", &CharacterControllerComponent::radius, {.speed = 0.01f})
        .field("height", &CharacterControllerComponent::height, {.speed = 0.01f, .tooltip = "Total height, radius included"})
        .field("offset", &CharacterControllerComponent::offset, {.speed = 0.01f, .tooltip = "Capsule center from the entity origin (its feet)"})
        .field("maxSlope", &CharacterControllerComponent::maxSlope, {.min = 0.0f, .max = 89.0f, .tooltip = "Degrees"})
        .field("stepHeight", &CharacterControllerComponent::stepHeight, {.speed = 0.01f})
        .field("stickToFloor", &CharacterControllerComponent::stickToFloor, {.speed = 0.01f})
        .field("mass", &CharacterControllerComponent::mass, {.speed = 0.5f})
        .field("maxPushForce", &CharacterControllerComponent::maxPushForce, {.speed = 1.0f, .tooltip = "N"})
        .field("gravityFactor", &CharacterControllerComponent::gravityFactor, {.speed = 0.01f})
        .field("layer", &CharacterControllerComponent::layer)
        .field("velocity", &CharacterControllerComponent::velocity, {.speed = 0.05f})
        .field("grounded", &CharacterControllerComponent::grounded, {.transient = true})
        .field("groundState", &CharacterControllerComponent::groundState, {.transient = true})
        .field("groundNormal", &CharacterControllerComponent::groundNormal, {.transient = true})
        .field("ground", &CharacterControllerComponent::ground, {.transient = true})
        .field("character", &CharacterControllerComponent::character, {.transient = true})
        .field("appliedKey", &CharacterControllerComponent::appliedKey, {.transient = true})
        .field("appliedPose", &CharacterControllerComponent::appliedPose, {.transient = true})
        .component();

    type<RotatorComponent>("Rotator")
        .field("axis", &RotatorComponent::axis, {.speed = 0.01f})
        .field("speed", &RotatorComponent::speed, {.speed = 0.01f, .tooltip = "Radians per second"})
        .component();

    type<PrefabInstanceComponent>("PrefabInstance")
        .field("asset", &PrefabInstanceComponent::asset, {.readOnly = true, .tooltip = "GUID of the source asset"})
        .component()
        .displayName("Prefab Instance");
}

} // namespace nova::reflect::detail
