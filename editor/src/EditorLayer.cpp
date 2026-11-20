#include "EditorLayer.h"

#include <imgui_internal.h> // DockBuilder
#include <ImGuizmo.h>

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

using namespace nova;

namespace {

bool drawVec3(const char* label, glm::vec3& v, float speed = 0.05f, float resetValue = 0.0f) {
    bool changed = false;
    ImGui::PushID(label);
    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, 90.0f);
    ImGui::TextUnformatted(label);
    ImGui::NextColumn();
    const float w = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
    const char* axes[3] = {"X", "Y", "Z"};
    const ImVec4 colors[3] = {{0.75f, 0.2f, 0.2f, 1}, {0.25f, 0.65f, 0.25f, 1}, {0.2f, 0.35f, 0.8f, 1}};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Border, colors[i]);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::SetNextItemWidth(w);
        char id[8];
        std::snprintf(id, sizeof(id), "##%s", axes[i]);
        changed |= ImGui::DragFloat(id, &v[i], speed, 0.0f, 0.0f, "%.2f");
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            v[i] = resetValue;
            changed = true;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    ImGui::Columns(1);
    ImGui::PopID();
    return changed;
}

template <typename T, typename F>
void componentSection(Entity e, const char* title, F&& body) {
    if (!e.has<T>()) return;
    ImGui::PushID(title);
    bool open = ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 10.0f);
    bool remove = false;
    if (ImGui::SmallButton("x")) remove = true;
    if (open && !remove) body(e.get<T>());
    if (remove) e.remove<T>();
    ImGui::PopID();
}

void materialEditor(Material& m) {
    ImGui::ColorEdit4("Base color", glm::value_ptr(m.baseColor));
    ImGui::SliderFloat("Metallic", &m.metallic, 0.0f, 1.0f);
    ImGui::SliderFloat("Roughness", &m.roughness, 0.0f, 1.0f);
    ImGui::ColorEdit3("Emissive", glm::value_ptr(m.emissive));
    ImGui::DragFloat("Emissive strength", &m.emissiveStrength, 0.1f, 0.0f, 100.0f);
    ImGui::Checkbox("Checker pattern", &m.checker);
}

} // namespace

// ------------------------------------------------------------------ setup --
void EditorLayer::onAttach() {
    m_view = renderer().createView({1280, 720});
    buildDemoScene();
    m_camera.position = {0.0f, 4.5f, 13.0f};
    m_camera.pitch = -0.28f;
    NOVA_INFO("Editor ready. Press Play to simulate. RMB+WASD fly | W/E/R gizmo | F focus | Del delete");
}

void EditorLayer::onDetach() {
    m_scene.clear();
    m_scene.stopSimulation();
    m_view.reset();
}

void EditorLayer::play() {
    if (m_state == PlayState::Pause) {
        m_state = PlayState::Play;
        return;
    }
    m_snapshot.clear();
    for (auto [e, t] : m_scene.registry().view<TransformComponent>().each()) m_snapshot.emplace_back(e, t);
    m_scene.startSimulation();
    m_state = PlayState::Play;
}

void EditorLayer::stop() {
    m_scene.stopSimulation();
    for (auto& [e, t] : m_snapshot)
        if (m_scene.registry().valid(e)) m_scene.registry().replace<TransformComponent>(e, t);
    m_snapshot.clear();
    m_state = PlayState::Edit;
}

Entity EditorLayer::createPrimitive(const char* name, MeshHandle mesh) {
    Entity e = m_scene.createEntity(name);
    e.add<MeshComponent>(mesh);
    return e;
}

void EditorLayer::buildDemoScene() {
    Renderer& r = renderer();

    Entity cam = m_scene.createEntity("Main Camera");
    cam.add<CameraComponent>();
    cam.transform().position = {0.0f, 3.0f, 10.0f};
    cam.transform().lookAt({0.0f, 1.0f, 0.0f});

    Entity sun = m_scene.createEntity("Sun");
    sun.add<DirectionalLightComponent>(glm::vec3(1.0f, 0.95f, 0.85f), 3.5f);
    sun.transform().rotation = {glm::radians(-42.0f), glm::radians(30.0f), 0.0f};

    Entity ground = createPrimitive("Ground", r.plane());
    ground.transform().scale = {60.0f, 1.0f, 60.0f};
    auto& gm = ground.get<MeshComponent>().material;
    gm.baseColor = {0.75f, 0.75f, 0.78f, 1.0f};
    gm.roughness = 0.8f;
    gm.checker = true;
    ground.add<RigidBodyComponent>(physics::BodyType::Static);
    ground.add<BoxColliderComponent>(glm::vec3(0.5f, 0.05f, 0.5f), glm::vec3(0.0f, -0.05f, 0.0f));

    // Physics playground: a stack of boxes and balls falling onto it (press Play).
    for (int y = 0; y < 6; ++y) {
        Entity b = createPrimitive(("Box " + std::to_string(y)).c_str(), r.cube());
        b.transform().position = {7.5f + (y % 2) * 0.15f, 0.4f + y * 0.81f, 1.0f};
        b.transform().scale = glm::vec3(0.8f);
        b.get<MeshComponent>().material.baseColor = {0.55f, 0.38f, 0.2f, 1.0f};
        b.add<RigidBodyComponent>();
        b.add<BoxColliderComponent>();
    }
    for (int i = 0; i < 4; ++i) {
        Entity ball = createPrimitive(("Ball " + std::to_string(i)).c_str(), r.sphere());
        ball.transform().position = {7.3f + 0.2f * i, 7.0f + 1.5f * i, 1.0f + 0.15f * i};
        ball.transform().scale = glm::vec3(0.6f);
        auto& bm = ball.get<MeshComponent>().material;
        bm.baseColor = {0.2f, 0.8f, 0.35f, 1.0f};
        bm.roughness = 0.3f;
        ball.add<RigidBodyComponent>().restitution = 0.6f;
        ball.add<SphereColliderComponent>();
    }

    // Row of spheres sweeping roughness (dielectric) and metallic (gold).
    for (int i = 0; i < 6; ++i) {
        Entity s = createPrimitive(("Sphere " + std::to_string(i)).c_str(), r.sphere());
        s.transform().position = {-5.0f + 2.0f * i, 0.6f, 0.0f};
        s.transform().scale = glm::vec3(1.2f);
        auto& m = s.get<MeshComponent>().material;
        m.baseColor = {0.85f, 0.15f, 0.12f, 1.0f};
        m.roughness = 0.05f + 0.18f * i;
        s.add<RigidBodyComponent>(physics::BodyType::Static);
        s.add<SphereColliderComponent>();

        Entity g = createPrimitive(("Gold " + std::to_string(i)).c_str(), r.sphere());
        g.transform().position = {-5.0f + 2.0f * i, 0.6f, 2.5f};
        g.transform().scale = glm::vec3(1.2f);
        auto& gmat = g.get<MeshComponent>().material;
        gmat.baseColor = {1.0f, 0.78f, 0.34f, 1.0f};
        gmat.metallic = 1.0f;
        gmat.roughness = 0.05f + 0.18f * i;
    }

    // Pillars that cast long shadows.
    for (int i = 0; i < 4; ++i) {
        Entity p = createPrimitive(("Pillar " + std::to_string(i)).c_str(), r.cube());
        p.transform().position = {-6.0f + 4.0f * i, 2.0f, -4.0f};
        p.transform().scale = {0.6f, 4.0f, 0.6f};
        p.get<MeshComponent>().material.baseColor = {0.6f, 0.62f, 0.66f, 1.0f};
        p.add<RigidBodyComponent>(physics::BodyType::Static);
        p.add<BoxColliderComponent>();
    }

    Entity crate = createPrimitive("Spinning Crate", r.cube());
    crate.transform().position = {0.0f, 2.2f, -1.5f};
    crate.transform().scale = glm::vec3(1.2f);
    crate.get<MeshComponent>().material.baseColor = {0.2f, 0.45f, 0.9f, 1.0f};
    crate.add<RotatorComponent>(glm::vec3(0.4f, 1.0f, 0.2f), 0.7f);
    crate.add<RigidBodyComponent>(physics::BodyType::Kinematic);

    Entity lamp = createPrimitive("Lamp", r.sphere());
    lamp.transform().position = {4.0f, 1.5f, -2.0f};
    lamp.transform().scale = glm::vec3(0.25f);
    auto& lm = lamp.get<MeshComponent>().material;
    lm.emissive = {1.0f, 0.55f, 0.2f};
    lm.emissiveStrength = 12.0f;
    lamp.get<MeshComponent>().castShadows = false;
    lamp.add<PointLightComponent>(glm::vec3(1.0f, 0.55f, 0.2f), 25.0f, 9.0f);

    Entity lamp2 = createPrimitive("Lamp 2", r.sphere());
    lamp2.transform().position = {-4.0f, 1.2f, -2.0f};
    lamp2.transform().scale = glm::vec3(0.25f);
    auto& lm2 = lamp2.get<MeshComponent>().material;
    lm2.emissive = {0.3f, 0.6f, 1.0f};
    lm2.emissiveStrength = 12.0f;
    lamp2.get<MeshComponent>().castShadows = false;
    lamp2.add<PointLightComponent>(glm::vec3(0.3f, 0.6f, 1.0f), 25.0f, 9.0f);

    m_selected = crate;
}

// ----------------------------------------------------------------- update --
void EditorLayer::onUpdate(float dt) {
    // Resize the render targets to last frame's panel size (avoids recreating mid-frame).
    Extent2D wanted{static_cast<u32>(std::max(1.0f, m_viewportSize.x)), static_cast<u32>(std::max(1.0f, m_viewportSize.y))};
    m_view->resize(wanted);

    bool wasActive = m_cameraActive;
    m_cameraActive = m_camera.update(dt, m_viewportHovered);
    if (m_cameraActive != wasActive) app().window().setCursorCaptured(m_cameraActive);

    if (!m_cameraActive && !ImGui::GetIO().WantTextInput) {
        if (Input::isKeyPressed(Key::W)) m_gizmoOperation = 0;
        if (Input::isKeyPressed(Key::E)) m_gizmoOperation = 1;
        if (Input::isKeyPressed(Key::R)) m_gizmoOperation = 2;
        if (Input::isKeyPressed(Key::F) && m_selected.valid()) m_camera.focus(m_selected.transform().position);
        if (Input::isKeyPressed(Key::Delete) && m_selected.valid() && m_viewportHovered) {
            m_scene.destroyEntity(m_selected);
            m_selected = {};
        }
    }

    if (m_state == PlayState::Play) m_scene.update(dt);
    if (m_selected && !m_selected.valid()) m_selected = {};
}

void EditorLayer::onRender(rhi::CommandList& cmd) {
    Extent2D size = m_view->size();
    float aspect = static_cast<float>(size.width) / static_cast<float>(size.height);
    CameraData editorCam = m_camera.data(aspect);
    bool ok = m_scene.buildRenderScene(m_renderScene, aspect, m_useSceneCamera ? nullptr : &editorCam);
    if (!ok) m_scene.buildRenderScene(m_renderScene, aspect, &editorCam);
    renderer().render(cmd, m_renderScene, *m_view);
}

// --------------------------------------------------------------------- UI --
void EditorLayer::setupDockLayout(unsigned int dockId) {
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockId;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.24f, nullptr, &center);
    ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Renderer", rightBottom);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderFinish(dockId);
}

void EditorLayer::onImGui() {
    ImGuizmo::BeginFrame();
    drawMenuBar();

    ImGuiID dockId = ImGui::GetID("NovaDockSpace");
    if (!ImGui::DockBuilderGetNode(dockId)) setupDockLayout(dockId);
    ImGui::DockSpaceOverViewport(dockId, ImGui::GetMainViewport());

    drawViewport();
    drawHierarchy();
    drawInspector();
    drawRendererPanel();
    drawConsole();
    drawImportPopup();
}

void EditorLayer::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Import glTF...")) m_openImport = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) app().quit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Entity")) {
        if (ImGui::MenuItem("Empty")) m_selected = m_scene.createEntity("Entity");
        if (ImGui::MenuItem("Cube")) m_selected = createPrimitive("Cube", renderer().cube());
        if (ImGui::MenuItem("Sphere")) m_selected = createPrimitive("Sphere", renderer().sphere());
        if (ImGui::MenuItem("Plane")) m_selected = createPrimitive("Plane", renderer().plane());
        if (ImGui::MenuItem("Point Light")) {
            m_selected = m_scene.createEntity("Point Light");
            m_selected.add<PointLightComponent>();
            m_selected.transform().position = {0, 2, 0};
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (m_state != PlayState::Play) {
        if (ImGui::MenuItem(m_state == PlayState::Pause ? "Resume" : "Play")) play();
    } else if (ImGui::MenuItem("Pause")) {
        m_state = PlayState::Pause;
    }
    ImGui::BeginDisabled(m_state == PlayState::Edit);
    if (ImGui::MenuItem("Stop")) stop();
    ImGui::EndDisabled();
    ImGui::TextColored(m_state == PlayState::Edit ? ImVec4(0.6f, 0.6f, 0.65f, 1) : ImVec4(0.4f, 0.9f, 0.5f, 1), "%s",
                       m_state == PlayState::Edit ? "EDIT" : m_state == PlayState::Play ? "PLAYING" : "PAUSED");
    ImGui::Separator();
    ImGui::TextDisabled("%.0f FPS  %.2f ms", app().fps(), app().deltaTime() * 1000.0f);
    ImGui::EndMainMenuBar();
}

void EditorLayer::drawViewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::Begin("Viewport");
    ImVec2 avail = ImGui::GetContentRegionAvail();
    m_viewportSize = {avail.x, avail.y};
    m_viewportHovered = ImGui::IsWindowHovered();

    ImVec2 imagePos = ImGui::GetCursorScreenPos();
    if (app().imgui()) ImGui::Image(static_cast<ImTextureID>(app().imgui()->textureId(m_view->output())), avail);

    // Toolbar overlay
    ImGui::SetCursorScreenPos({imagePos.x + 8, imagePos.y + 8});
    const char* ops[] = {"Move (W)", "Rotate (E)", "Scale (R)"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        bool active = m_gizmoOperation == i;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(ops[i])) m_gizmoOperation = i;
        if (active) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Local", &m_gizmoLocal);
    ImGui::SameLine();
    ImGui::Checkbox("Scene camera", &m_useSceneCamera);

    // Gizmo
    if (m_selected.valid() && !m_useSceneCamera) {
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(imagePos.x, imagePos.y, avail.x, avail.y);
        float aspect = avail.y > 0 ? avail.x / avail.y : 1.0f;
        CameraData cam = m_camera.data(aspect);
        glm::mat4 proj = cam.projection;
        proj[1][1] *= -1.0f; // ImGuizmo expects GL-style Y-up clip space
        auto& t = m_selected.transform();
        glm::mat4 model = t.matrix();
        const ImGuizmo::OPERATION op = m_gizmoOperation == 0   ? ImGuizmo::TRANSLATE
                                       : m_gizmoOperation == 1 ? ImGuizmo::ROTATE
                                                               : ImGuizmo::SCALE;
        bool snap = Input::isKeyDown(Key::LeftControl);
        float snapValue = m_gizmoOperation == 1 ? 15.0f : 0.5f;
        float snaps[3] = {snapValue, snapValue, snapValue};
        if (ImGuizmo::Manipulate(glm::value_ptr(cam.view), glm::value_ptr(proj), op,
                                 m_gizmoLocal ? ImGuizmo::LOCAL : ImGuizmo::WORLD, glm::value_ptr(model), nullptr,
                                 snap ? snaps : nullptr)) {
            glm::vec3 scale, translation, skew;
            glm::vec4 persp;
            glm::quat rot;
            if (glm::decompose(model, scale, rot, translation, skew, persp)) {
                t.position = translation;
                t.rotation = glm::eulerAngles(rot);
                t.scale = scale;
                if (m_scene.isSimulating()) m_scene.syncTransformToPhysics(m_selected); // drag bodies while playing
            }
        }
    }

    // Click-free hint
    if (!m_cameraActive) {
        ImGui::SetCursorScreenPos({imagePos.x + 8, imagePos.y + avail.y - 24});
        ImGui::TextDisabled("RMB + WASD/QE: fly   Wheel: speed   F: focus   Ctrl: snap");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void EditorLayer::drawHierarchy() {
    ImGui::Begin("Hierarchy");
    ImGui::TextDisabled("%s  (%zu entities)", m_scene.name().c_str(),
                        static_cast<size_t>(m_scene.registry().view<NameComponent>().size()));
    ImGui::Separator();

    Entity toDelete;
    for (auto [handle, name] : m_scene.registry().view<NameComponent>().each()) {
        Entity e{handle, &m_scene};
        ImGui::PushID(static_cast<int>(entt::to_integral(handle)));
        const char* icon = e.has<CameraComponent>()             ? "[C] "
                           : e.has<DirectionalLightComponent>() ? "[S] "
                           : e.has<PointLightComponent>()       ? "[L] "
                           : e.has<MeshComponent>()             ? "[M] "
                                                                : "[ ] ";
        std::string label = std::string(icon) + name.name;
        if (ImGui::Selectable(label.c_str(), m_selected == e)) m_selected = e;
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Focus")) m_camera.focus(e.transform().position);
            if (ImGui::MenuItem("Delete")) toDelete = e;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (toDelete.valid()) {
        if (m_selected == toDelete) m_selected = {};
        m_scene.destroyEntity(toDelete);
    }

    if (ImGui::BeginPopupContextWindow("hierarchy_ctx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("Create Empty")) m_selected = m_scene.createEntity("Entity");
        if (ImGui::MenuItem("Create Cube")) m_selected = createPrimitive("Cube", renderer().cube());
        if (ImGui::MenuItem("Create Sphere")) m_selected = createPrimitive("Sphere", renderer().sphere());
        ImGui::EndPopup();
    }
    ImGui::End();
}

void EditorLayer::drawInspector() {
    ImGui::Begin("Inspector");
    if (!m_selected.valid()) {
        ImGui::TextDisabled("Select an entity.");
        ImGui::End();
        return;
    }
    Entity e = m_selected;

    auto& name = e.get<NameComponent>().name;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", name.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##name", buf, sizeof(buf))) name = buf;

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& t = e.transform();
        drawVec3("Position", t.position);
        glm::vec3 deg = glm::degrees(t.rotation);
        if (drawVec3("Rotation", deg, 0.5f)) t.rotation = glm::radians(deg);
        drawVec3("Scale", t.scale, 0.02f, 1.0f);
    }

    componentSection<MeshComponent>(e, "Mesh", [&](MeshComponent& m) {
        if (ImGui::BeginCombo("Mesh", renderer().meshName(m.mesh).c_str())) {
            for (MeshHandle h : renderer().meshes())
                if (ImGui::Selectable(renderer().meshName(h).c_str(), h == m.mesh)) m.mesh = h;
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Cast shadows", &m.castShadows);
        materialEditor(m.material);
    });
    componentSection<CameraComponent>(e, "Camera", [](CameraComponent& c) {
        float fov = glm::degrees(c.fovY);
        if (ImGui::SliderFloat("FOV", &fov, 10.0f, 120.0f)) c.fovY = glm::radians(fov);
        ImGui::DragFloat("Near", &c.nearClip, 0.01f, 0.001f, 10.0f);
        ImGui::DragFloat("Far", &c.farClip, 1.0f, 1.0f, 10000.0f);
        ImGui::Checkbox("Primary", &c.primary);
    });
    componentSection<DirectionalLightComponent>(e, "Directional Light", [](DirectionalLightComponent& l) {
        ImGui::ColorEdit3("Color", glm::value_ptr(l.color));
        ImGui::DragFloat("Intensity", &l.intensity, 0.05f, 0.0f, 100.0f);
    });
    componentSection<PointLightComponent>(e, "Point Light", [](PointLightComponent& l) {
        ImGui::ColorEdit3("Color", glm::value_ptr(l.color));
        ImGui::DragFloat("Intensity", &l.intensity, 0.1f, 0.0f, 1000.0f);
        ImGui::DragFloat("Radius", &l.radius, 0.05f, 0.1f, 100.0f);
    });
    componentSection<RotatorComponent>(e, "Rotator", [](RotatorComponent& r) {
        ImGui::DragFloat3("Axis", glm::value_ptr(r.axis), 0.01f);
        ImGui::DragFloat("Speed", &r.speed, 0.01f);
    });
    componentSection<RigidBodyComponent>(e, "Rigid Body", [&](RigidBodyComponent& rb) {
        const char* types[] = {"Static", "Kinematic", "Dynamic"};
        int type = static_cast<int>(rb.type);
        ImGui::BeginDisabled(m_scene.isSimulating());
        if (ImGui::Combo("Type", &type, types, 3)) rb.type = static_cast<physics::BodyType>(type);
        ImGui::EndDisabled();
        ImGui::DragFloat("Mass", &rb.mass, 0.05f, 0.001f, 10000.0f);
        ImGui::SliderFloat("Friction", &rb.friction, 0.0f, 1.0f);
        ImGui::SliderFloat("Restitution", &rb.restitution, 0.0f, 1.0f);
        ImGui::SliderFloat("Linear damping", &rb.linearDamping, 0.0f, 1.0f);
        ImGui::SliderFloat("Angular damping", &rb.angularDamping, 0.0f, 1.0f);
        ImGui::DragFloat("Gravity factor", &rb.gravityFactor, 0.01f, -10.0f, 10.0f);
        ImGui::Checkbox("Sensor", &rb.isSensor);
        if (m_scene.isSimulating()) {
            ImGui::TextDisabled("Body properties apply on next Play.");
            glm::vec3 v = m_scene.linearVelocity(e);
            ImGui::Text("Velocity %.2f %.2f %.2f", v.x, v.y, v.z);
        }
    });
    componentSection<BoxColliderComponent>(e, "Box Collider", [](BoxColliderComponent& c) {
        ImGui::DragFloat3("Half extents", glm::value_ptr(c.halfExtents), 0.01f, 0.001f, 1000.0f);
        ImGui::DragFloat3("Offset", glm::value_ptr(c.offset), 0.01f);
    });
    componentSection<SphereColliderComponent>(e, "Sphere Collider", [](SphereColliderComponent& c) {
        ImGui::DragFloat("Radius", &c.radius, 0.01f, 0.001f, 1000.0f);
        ImGui::DragFloat3("Offset", glm::value_ptr(c.offset), 0.01f);
    });
    componentSection<CapsuleColliderComponent>(e, "Capsule Collider", [](CapsuleColliderComponent& c) {
        ImGui::DragFloat("Radius", &c.radius, 0.01f, 0.001f, 1000.0f);
        ImGui::DragFloat("Half height", &c.halfHeight, 0.01f, 0.001f, 1000.0f);
        ImGui::DragFloat3("Offset", glm::value_ptr(c.offset), 0.01f);
    });
    if (e.has<ScriptComponent>())
        ImGui::TextDisabled("%zu native script(s)", e.get<ScriptComponent>().scripts.size());

    ImGui::Separator();
    if (ImGui::Button("Add Component", {-1, 0})) ImGui::OpenPopup("add_component");
    if (ImGui::BeginPopup("add_component")) {
        if (!e.has<MeshComponent>() && ImGui::MenuItem("Mesh")) e.add<MeshComponent>(renderer().cube());
        if (!e.has<CameraComponent>() && ImGui::MenuItem("Camera")) e.add<CameraComponent>().primary = false;
        if (!e.has<DirectionalLightComponent>() && ImGui::MenuItem("Directional Light"))
            e.add<DirectionalLightComponent>();
        if (!e.has<PointLightComponent>() && ImGui::MenuItem("Point Light")) e.add<PointLightComponent>();
        if (!e.has<RotatorComponent>() && ImGui::MenuItem("Rotator")) e.add<RotatorComponent>();
        ImGui::Separator();
        if (!e.has<RigidBodyComponent>() && ImGui::MenuItem("Rigid Body")) e.add<RigidBodyComponent>();
        const bool hasCollider =
            e.has<BoxColliderComponent>() || e.has<SphereColliderComponent>() || e.has<CapsuleColliderComponent>();
        ImGui::BeginDisabled(hasCollider);
        if (ImGui::MenuItem("Box Collider")) e.add<BoxColliderComponent>();
        if (ImGui::MenuItem("Sphere Collider")) e.add<SphereColliderComponent>();
        if (ImGui::MenuItem("Capsule Collider")) e.add<CapsuleColliderComponent>();
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::End();
}

void EditorLayer::drawRendererPanel() {
    ImGui::Begin("Renderer");
    auto& dev = app().device();
    const auto& caps = dev.caps();
    RenderSettings& s = renderer().settings();
    const RenderStats& st = renderer().stats();

    ImGui::Text("%s", caps.deviceName.c_str());
    ImGui::TextDisabled("%s | RT: %s%s", caps.apiName.c_str(), caps.rayQuery ? "ray query" : "unsupported",
                        caps.rayTracingPipeline ? " + pipeline" : "");
    ImGui::Text("Draws %u | Tris %llu | TLAS inst %u", st.drawCalls, static_cast<unsigned long long>(st.triangles),
                st.tlasInstances);
    ImGui::Text("Physics: %s (%u bodies)", m_scene.isSimulating() ? "Jolt, running" : "stopped",
                m_scene.physics() ? m_scene.physics()->bodyCount() : 0u);
    ImGui::Separator();

    ImGui::BeginDisabled(!caps.rayQuery);
    ImGui::Checkbox("Ray-traced shadows", &s.rayTracedShadows);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!caps.wireframe);
    ImGui::Checkbox("Wireframe", &s.wireframe);
    ImGui::EndDisabled();
    bool vsync = dev.vsync();
    if (ImGui::Checkbox("VSync", &vsync)) dev.setVSync(vsync);

    const char* views[] = {"Lit", "Normals", "Sun shadows", "Albedo"};
    int dv = static_cast<int>(s.debugView);
    if (ImGui::Combo("Debug view", &dv, views, 4)) s.debugView = static_cast<DebugView>(dv);
    const char* tms[] = {"ACES", "Reinhard", "None"};
    int tm = static_cast<int>(s.tonemapper);
    if (ImGui::Combo("Tonemapper", &tm, tms, 3)) s.tonemapper = static_cast<Tonemapper>(tm);
    ImGui::SliderFloat("Exposure", &s.exposure, 0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Shadow bias", &s.shadowBias, 0.0f, 0.2f, "%.3f");
    if (ImGui::TreeNode("Sky & ambient")) {
        ImGui::ColorEdit3("Zenith", glm::value_ptr(s.skyZenith));
        ImGui::ColorEdit3("Horizon", glm::value_ptr(s.skyHorizon));
        ImGui::ColorEdit3("Ground", glm::value_ptr(s.groundColor));
        ImGui::SliderFloat("Ambient", &s.ambientIntensity, 0.0f, 3.0f);
        ImGui::TreePop();
    }
    ImGui::End();
}

void EditorLayer::drawConsole() {
    ImGui::Begin("Console");
    ImGui::Checkbox("Auto-scroll", &m_consoleAutoScroll);
    ImGui::Separator();
    ImGui::BeginChild("log", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    for (const LogEntry& entry : Log::recent(400)) {
        ImVec4 color{0.8f, 0.8f, 0.82f, 1.0f};
        if (entry.level >= spdlog::level::err) color = {1.0f, 0.4f, 0.4f, 1.0f};
        else if (entry.level == spdlog::level::warn) color = {1.0f, 0.8f, 0.35f, 1.0f};
        else if (entry.level <= spdlog::level::debug) color = {0.55f, 0.55f, 0.6f, 1.0f};
        ImGui::TextColored(color, "%s", entry.text.c_str());
    }
    if (m_consoleAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

void EditorLayer::drawImportPopup() {
    if (m_openImport) {
        ImGui::OpenPopup("Import glTF");
        m_openImport = false;
    }
    if (ImGui::BeginPopupModal("Import glTF", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Path to a .gltf or .glb file:");
        ImGui::SetNextItemWidth(500.0f);
        ImGui::InputText("##path", m_importPath, sizeof(m_importPath));
        if (ImGui::Button("Import", {120, 0})) {
            if (auto root = loadGltf(m_scene, renderer(), m_importPath)) m_selected = *root;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120, 0})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
