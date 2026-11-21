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
        .field("rotation", &TransformComponent::rotation, {.speed = 0.5f, .degrees = true})
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

    type<RigidBodyComponent>("RigidBody")
        .field("type", &RigidBodyComponent::type)
        .field("mass", &RigidBodyComponent::mass, {.speed = 0.05f, .tooltip = "kg (dynamic bodies)"})
        .field("friction", &RigidBodyComponent::friction, {.min = 0.0f, .max = 1.0f})
        .field("restitution", &RigidBodyComponent::restitution, {.min = 0.0f, .max = 1.0f, .tooltip = "Bounciness"})
        .field("linearDamping", &RigidBodyComponent::linearDamping, {.min = 0.0f, .max = 1.0f})
        .field("angularDamping", &RigidBodyComponent::angularDamping, {.min = 0.0f, .max = 1.0f})
        .field("gravityFactor", &RigidBodyComponent::gravityFactor, {.speed = 0.01f})
        .field("isSensor", &RigidBodyComponent::isSensor, {.tooltip = "Reports contacts, no collision response", .label = "Sensor"})
        .field("body", &RigidBodyComponent::body, {.transient = true})
        .component();

    type<BoxColliderComponent>("BoxCollider")
        .field("halfExtents", &BoxColliderComponent::halfExtents, {.speed = 0.01f})
        .field("offset", &BoxColliderComponent::offset, {.speed = 0.01f})
        .component();
    type<SphereColliderComponent>("SphereCollider")
        .field("radius", &SphereColliderComponent::radius, {.speed = 0.01f})
        .field("offset", &SphereColliderComponent::offset, {.speed = 0.01f})
        .component();
    type<CapsuleColliderComponent>("CapsuleCollider")
        .field("radius", &CapsuleColliderComponent::radius, {.speed = 0.01f})
        .field("halfHeight", &CapsuleColliderComponent::halfHeight, {.speed = 0.01f})
        .field("offset", &CapsuleColliderComponent::offset, {.speed = 0.01f})
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
