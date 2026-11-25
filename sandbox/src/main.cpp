// Nova Sandbox: a physics test bench built only on the public API (nova/Nova.h).
//
// Pick a scenario in the panel (or with the number keys), then pause, single-step, reset, shoot
// into it and watch colliders and contacts. Each scenario prints its own measurements, and
// "Check determinism" runs the scenario twice in hidden scenes and compares the state hashes.
#include <nova/Nova.h>

#include "Scenarios.h"
#include "Shapes.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace {

// Input is declared once as named actions/axes. Player 0 = keyboard + mouse, players 1.. = gamepads.
void bindControls() {
    using nova::Input;
    Input::bindAxis("Move", {.up = nova::Key::W, .down = nova::Key::S, .left = nova::Key::A, .right = nova::Key::D,
                             .stick = nova::GamepadStick::Left});
    Input::bindAxis("Look", {.mouseDelta = true, .mouseScale = 0.003f});
    Input::bindAction("Fast", {.keys = {nova::Key::LeftShift}});
    Input::bindAction("Shoot", {.keys = {nova::Key::F}, .mouseButtons = {nova::MouseButton::Left}});
    Input::bindAction("Jump", {.keys = {nova::Key::Space}, .gamepadButtons = {nova::GamepadButton::A}});
}

// A ball driven by one gamepad player: left stick rolls it (relative to the camera), A jumps.
struct PlayerBall : nova::Script {
    nova::u32 player;
    nova::Entity camera;
    PlayerBall(nova::u32 p, nova::Entity cam) : player(p), camera(cam) {}

    void onUpdate(float) override {
        glm::vec2 move = nova::Input::axis("Move", player);
        glm::vec3 fwd = camera.transform().forward();
        fwd = glm::normalize(glm::vec3(fwd.x, 0.0f, fwd.z));
        glm::vec3 right = glm::cross(fwd, glm::vec3(0, 1, 0));
        scene().addForce(entity(), (fwd * move.y + right * move.x) * 40.0f);
        // Grounded = something right below us (sweep our own collider down a little).
        bool grounded = scene().sweepEntity(entity(), {0, -1, 0}, 0.1f).has_value();
        if (grounded && nova::Input::isActionPressed("Jump", player)) scene().addImpulse(entity(), {0.0f, 12.0f, 0.0f});
    }
};

constexpr glm::vec4 kStatic{0.6f, 0.6f, 0.65f, 1.0f}, kKinematic{1.0f, 0.8f, 0.2f, 1.0f},
    kDynamic{0.3f, 1.0f, 0.4f, 1.0f}, kSensor{0.2f, 0.85f, 1.0f, 1.0f}, kCharacter{0.4f, 0.6f, 1.0f, 1.0f};

void drawCylinder(nova::DebugDraw& dd, const nova::GlobalPosition& center, const glm::quat& rotation, float radius,
                  float halfHeight, const glm::vec4& color) {
    const glm::vec3 axis = rotation * glm::vec3(0, 1, 0), a = rotation * glm::vec3(radius, 0, 0),
                    b = rotation * glm::vec3(0, 0, radius);
    dd.circle(center + axis * halfHeight, axis, radius, color);
    dd.circle(center - axis * halfHeight, axis, radius, color);
    for (const glm::vec3& side : {a, -a, b, -b}) dd.line(center + side - axis * halfHeight, center + side + axis * halfHeight, color);
}

} // namespace

class PhysicsBench : public nova::GameLayer {
public:
    void onStart() override {
        bindControls();
        auto& r = renderer();
        m_meshes = {r.cube(), r.sphere(), r.plane(),
                    r.createMesh(shapes::capsule(), "Capsule"), r.createMesh(shapes::cylinder(), "Cylinder"),
                    r.createMesh(shapes::gem(), "Gem"), r.createMesh(shapes::terrain(), "Terrain"),
                    r.createMesh(shapes::capsule(0.3f, 0.6f, 0.9f), "Player capsule"),
                    r.createMesh(nova::MeshData::plane(1.0f, 32), "Grid"), r.createMesh(shapes::bendableColumn(), "Bendable column")};
        int first = 0; // `--scenario <name>`: start there
        for (int i = 0; i < static_cast<int>(scenarios().size()); ++i)
            if (s_startScenario == scenarios()[i].name) first = i;
        load(first, false); // GameLayer starts the simulation after onStart()
        if (s_noRt) r.settings().rayTracedShadows = false;
        if (s_noCull) r.settings().gpuCulling = false;
        if (s_noOcclusion) r.settings().occlusionCulling = false;
        if (s_noBloom) r.settings().bloom = false;
        if (s_autoExposure) r.settings().autoExposure = true;
        if (s_noSsao) r.settings().ssao = false;
        if (s_debugView >= 0) r.settings().debugView = static_cast<nova::DebugView>(s_debugView);
#if NOVA_WRAP_RENDER_MODES
        if (s_wrapShift >= 0) r.settings().wrapShift = static_cast<nova::WrapShift>(s_wrapShift);
        if (s_wrapShift == 3) r.settings().wrapCrop = s_cropCamera ? nova::WrapCrop::Camera : nova::WrapCrop::World;
        if (s_wrapRepeat >= 0) r.settings().wrapRepeat = static_cast<nova::u32>(s_wrapRepeat);
#endif
        if (s_selfCheck) {
            // `Sandbox --check`: every scenario, two hidden runs each; then quit (exit code 1 on divergence).
            for (const Scenario& sc : scenarios()) {
                const std::string result = checkDeterminism(sc);
                NOVA_INFO("[check] {}: {}", sc.name, result);
                if (result.rfind("DIVERGED", 0) == 0) s_checkFailed = true;
            }
            app().quit();
        }
    }

    static inline bool s_selfCheck = false, s_checkFailed = false;
    static inline bool s_noRt = false;
    static inline bool s_noCull = false;
    static inline bool s_noOcclusion = false;
    static inline bool s_noBloom = false;
    static inline bool s_autoExposure = false;
    static inline bool s_noSsao = false;
    static inline int s_debugView = -1; // --debug-view N (nova::DebugView)
    static inline int s_quitAfterFrames = -1; // --frames N: quit after N frames (smoke tests)
    static inline std::string s_screenshot;   // --screenshot <file.png>: the last frame before quitting
    static inline std::string s_startScenario;              // --scenario "<name>"
    static inline int s_wrapShift = -1, s_wrapRepeat = -1; // --wrap triangle|object|split|crop|crop-camera, --laps N
    static inline bool s_cropCamera = false;

    // Like GameLayer::onUpdate, plus pause, single step and time scale.
    void onUpdate(float dt) override {
        if (s_quitAfterFrames == 1 && !s_screenshot.empty()) renderer().captureNextFrame(s_screenshot);
        if (s_quitAfterFrames >= 0 && s_quitAfterFrames-- == 0) {
            const nova::RenderStats& st = renderer().stats();
            NOVA_INFO("[frames] draws {} | graph passes {} ({} dropped) | auto exposure {:.3f}", st.drawCalls, st.graphPasses,
                      st.graphCulled, st.autoExposure);
            NOVA_INFO("[frames] graph:\n{}", renderer().lastGraph());
            app().quit();
        }
        onTick(dt);
        if (!m_paused) scene().update(app().frameDuration().scaled(m_timeScale));
        else if (m_stepOnce) scene().update(scene().fixedTimeStep); // exactly one physics step
        m_stepOnce = false;
        if (scenarios()[m_current].character) followCharacter(); // after the update: where it is now
    }

    void onTick(float dt) override {
        using nova::Input, nova::Key;
        const Scenario& sc = scenarios()[m_current];
        const bool keyboard = !ImGui::GetIO().WantCaptureKeyboard, mouse = !ImGui::GetIO().WantCaptureMouse;
        if (keyboard) {
            if (Input::isKeyPressed(Key::Escape)) app().quit();
            if (Input::isKeyPressed(Key::P)) m_paused = !m_paused;
            if (Input::isKeyPressed(Key::N)) m_paused = m_stepOnce = true;
            if (Input::isKeyPressed(Key::R)) load(m_current, true);
            if (Input::isKeyPressed(Key::C)) m_showColliders = !m_showColliders;
            for (int i = 0; i < std::min(10, static_cast<int>(scenarios().size())); ++i) { // keys 1..9, then 0
                const Key key = i == 9 ? Key::D0 : static_cast<Key>(static_cast<int>(Key::D1) + i);
                if (Input::isKeyPressed(key)) load(i, false);
            }
        }
        if (sc.character) driveCharacter();
        else flyCamera(dt);
        if (mouse && Input::isActionPressed("Shoot")) shoot();

        if (sc.fire && sc.autoFire > 0.0f && !m_paused && (m_fireTimer += dt * m_timeScale) >= sc.autoFire) {
            m_fireTimer = 0.0f;
            sc.fire(scene(), m_meshes, m_spawned);
        }
        if (sc.gamepads) syncPlayerBalls();
        trim(m_spawned, 60);
        trim(m_projectiles, 50);
        drawDebug(m_paused ? 0.0f : dt);
    }

    void onImGui() override {
        const Scenario& sc = scenarios()[m_current];
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({360, 0}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Physics bench");
        if (ImGui::BeginCombo("Scenario", sc.name)) {
            for (int i = 0; i < static_cast<int>(scenarios().size()); ++i)
                if (ImGui::Selectable(scenarios()[i].name, i == m_current)) load(i, false);
            ImGui::EndCombo();
        }
        ImGui::TextWrapped("%s", sc.hint);
        if (ImGui::Button("Reset (R)")) load(m_current, true);
        ImGui::SameLine();
        if (ImGui::Button(m_paused ? "Resume (P)" : "Pause (P)")) m_paused = !m_paused;
        ImGui::SameLine();
        if (ImGui::Button("Step (N)")) m_paused = m_stepOnce = true;
        ImGui::SameLine();
        if (ImGui::Button("Camera")) load(m_current, false);
        if (sc.fire && ImGui::Button(sc.fireLabel)) sc.fire(scene(), m_meshes, m_spawned);
        ImGui::SliderFloat("Time scale", &m_timeScale, 0.05f, 2.0f, "%.2fx");

        nova::PhysicsSettings settings = scene().physicsSettings();
        float gravity = nova::toFloat(settings.gravity.y);
        if (ImGui::SliderFloat("Gravity Y", &gravity, -30.0f, 5.0f, "%.2f m/s2")) {
            settings.gravity.y = gravity;
            scene().setPhysicsSettings(settings);
        }
        ImGui::SliderFloat("Shot speed", &m_shotSpeed, 5.0f, 80.0f, "%.0f m/s");
        ImGui::SliderFloat("Shot mass", &m_shotMass, 0.5f, 50.0f, "%.1f kg");
        ImGui::Checkbox("Colliders (C)", &m_showColliders);
        ImGui::SameLine();
        ImGui::Checkbox("Contacts", &m_showContacts);
        if (renderer().rayTracingAvailable()) {
            ImGui::SameLine();
            ImGui::Checkbox("RT shadows", &renderer().settings().rayTracedShadows);
        }
        ImGui::SameLine();
        ImGui::Checkbox("GPU culling", &renderer().settings().gpuCulling);
        ImGui::SameLine();
        ImGui::Checkbox("Occlusion", &renderer().settings().occlusionCulling);

        ImGui::SeparatorText("Measurements");
        if (sc.status) sc.status(scene());
        ImGui::Text("%.0f FPS | %u bodies | step %.1f ms | %s", app().fps(), scene().physicsBodyCount(),
                    scene().fixedTimeStep.seconds() * 1000.0, m_paused ? "PAUSED" : "running");
        ImGui::Text("State hash: %016llx", static_cast<unsigned long long>(scene().stateHash()));
        {
            const nova::RenderStats& st = renderer().stats();
            ImGui::Text("Draws %u (%u GPU-culled groups) | GPU culling: %u of %u visible, %u hidden", st.drawCalls,
                        st.cullGroups, st.gpuVisible, st.gpuTested, st.gpuOccluded);
            if (ImGui::TreeNode("graph", "Render graph: %u passes (%u dropped), %u transients in %u textures", st.graphPasses,
                                st.graphCulled, st.transientTextures, st.pooledTextures)) {
                ImGui::TextUnformatted(renderer().lastGraph().c_str());
                ImGui::TreePop();
            }
        }
        if (ImGui::Button("Check determinism")) m_determinism = checkDeterminism(sc);
        if (!m_determinism.empty()) ImGui::TextWrapped("%s", m_determinism.c_str());
        if (!sc.character) {
            auto& cam = m_camera.transform();
            if (auto hit = scene().sweepSphere(cam.position + cam.forward(), 0.2f, cam.forward(), 200.0f))
                ImGui::Text("Aim: %s (%.1f m)", hit->entity.name().c_str(), hit->distance);
            else
                ImGui::TextDisabled("Aim: nothing");
        }
        for (nova::u32 p = 1; p < nova::Input::playerCount(); ++p)
            ImGui::Text("Player %u: %s", p, nova::Input::playerDeviceName(p).c_str());
        ImGui::TextDisabled(sc.character ? "F / LMB shoot, Esc quit" : "RMB + WASD fly, Shift fast, F / LMB shoot, Esc quit");
        ImGui::End();
    }

private:
    // Rebuilds scenario `index` from scratch; `keepCamera` keeps the current view (a reset).
    void load(int index, bool keepCamera) {
        nova::GlobalPosition eye;
        glm::quat look{1, 0, 0, 0};
        const bool keep = keepCamera && index == m_current && m_camera.valid();
        if (keep) eye = m_camera.transform().position, look = m_camera.transform().orientation();
        const bool simulating = scene().isSimulating();

        scene().stopSimulation();
        scene().clear();
        m_spawned.clear();
        m_projectiles.clear();
        m_playerBalls.clear();
        contactMarks().clear();
        m_fireTimer = 0.0f;
        m_determinism.clear();
        scene().setWorldSize({});
        scene().setPhysicsSettings({});

        m_current = index;
        const Scenario& sc = scenarios()[index];
        const View view = sc.build(scene(), m_meshes);
        if (sc.enter && !keep) sc.enter(); // a reset keeps what was chosen in its panel
        m_camera = scene().createEntity("Camera");
        m_camera.add<nova::CameraComponent>().farClip = 500.0f;
        m_camera.transform().position = keep ? eye : view.eye;
        if (keep) m_camera.transform().setRotation(look);
        else m_camera.transform().lookAt(view.target);
        const glm::vec3 e = m_camera.transform().euler(nova::EulerOrder::YXZ);
        m_pitch = e.x;
        m_yaw = e.y;
        if (!keep) m_orbitYaw = 0.0f, m_orbitPitch = -0.35f;
        if (simulating) scene().startSimulation();
    }

    // Player 0 flies the camera: WASD to move, hold RMB to look (yaw/pitch rebuilt in YXZ order).
    void flyCamera(float dt) {
        auto& t = m_camera.transform();
        if (nova::Input::isMouseButtonDown(nova::MouseButton::Right)) {
            const glm::vec2 look = nova::Input::axis("Look"); // +Y = up
            m_yaw -= look.x;
            m_pitch = glm::clamp(m_pitch + look.y, -1.5f, 1.5f);
            t.setEuler({m_pitch, m_yaw, 0.0f}, nova::EulerOrder::YXZ);
        }
        const glm::vec2 move = nova::Input::axis("Move");
        const float speed = (nova::Input::isActionDown("Fast") ? 14.0f : 5.0f) * dt;
        t.position += (t.forward() * move.y + t.right() * move.x) * speed;
    }

    // The Character scenario: WASD walks the "Player" relative to the camera, which orbits it.
    void driveCharacter() {
        nova::Entity player = scene().findEntity("Player");
        if (!player) return;
        if (nova::Input::isMouseButtonDown(nova::MouseButton::Right)) {
            const glm::vec2 look = nova::Input::axis("Look");
            m_orbitYaw -= look.x;
            m_orbitPitch = glm::clamp(m_orbitPitch + look.y, -1.3f, 0.4f);
        }
        auto& cc = player.get<nova::CharacterControllerComponent>();
        const glm::vec3 fwd(-std::sin(m_orbitYaw), 0.0f, -std::cos(m_orbitYaw)), right(std::cos(m_orbitYaw), 0.0f, -std::sin(m_orbitYaw));
        const glm::vec2 move = nova::Input::axis("Move");
        const glm::vec3 v = (fwd * move.y + right * move.x) * (nova::Input::isActionDown("Fast") ? 8.0f : 4.0f);
        cc.velocity.x = v.x; // the vertical part is the scene's (gravity, landing)
        cc.velocity.z = v.z;
        if (cc.grounded && nova::Input::isActionPressed("Jump")) cc.velocity.y = 6.0f;
    }

    // The orbit camera, placed after the scene update so it sees the character's new position
    // (placed before it, the camera would trail by 0, 1 or 2 physics steps: visible jitter).
    void followCharacter() {
        nova::Entity player = scene().findEntity("Player");
        if (!player) return;
        auto& cam = m_camera.transform();
        cam.setEuler({m_orbitPitch, m_orbitYaw, 0.0f}, nova::EulerOrder::YXZ);
        // Where the player is drawn (between physics steps), or the camera and the player shake.
    cam.position = scene().renderTransform(player).position + glm::vec3(0.0f, 1.5f, 0.0f) - cam.forward() * 7.0f;
    }

    void shoot() {
        auto& cam = m_camera.transform();
        auto ball = scene().createEntity("Projectile");
        ball.transform().position = cam.position + cam.forward() * 1.0f;
        ball.transform().scale = glm::vec3(0.4f);
        nova::Material m;
        m.baseColor = {0.2f, 0.8f, 0.35f, 1.0f};
        m.roughness = 0.3f;
        ball.add<nova::MeshComponent>(m_meshes.sphere, m);
        auto& rb = ball.add<nova::RigidBodyComponent>();
        rb.mass = m_shotMass;
        rb.continuousCollision = true; // fast: never through thin walls
        ball.add<nova::SphereColliderComponent>(); // radius 0.5 * scale 0.4
        ball.addScript<FlashOnHit>();
        scene().setLinearVelocity(ball, cam.forward() * m_shotSpeed);
        m_projectiles.push_back(ball);
    }

    void trim(std::vector<nova::Entity>& list, size_t max) {
        while (list.size() > max) {
            scene().destroyEntity(list.front());
            list.erase(list.begin());
        }
    }

    // One ball per connected gamepad player; spawned on connect, removed on disconnect.
    void syncPlayerBalls() {
        const nova::u32 players = nova::Input::playerCount();
        while (m_playerBalls.size() + 1 < players) {
            const nova::u32 player = static_cast<nova::u32>(m_playerBalls.size()) + 1;
            auto ball = scene().createEntity("Player " + std::to_string(player));
            ball.transform().position = {-2.0f + 1.5f * player, 2.0f, 3.0f};
            ball.transform().scale = glm::vec3(0.8f);
            nova::Material m;
            const glm::vec3 colors[] = {{0.2f, 0.5f, 1.0f}, {1.0f, 0.3f, 0.6f}, {1.0f, 0.85f, 0.2f}, {0.6f, 0.3f, 1.0f}};
            m.baseColor = glm::vec4(colors[(player - 1) % 4], 1.0f);
            m.roughness = 0.25f;
            ball.add<nova::MeshComponent>(m_meshes.sphere, m);
            ball.add<nova::RigidBodyComponent>().mass = 2.0f;
            ball.add<nova::SphereColliderComponent>();
            ball.addScript<PlayerBall>(player, m_camera);
            m_playerBalls.push_back(ball);
            NOVA_INFO("Player {} joined with {}", player, nova::Input::playerDeviceName(player));
        }
        while (m_playerBalls.size() + 1 > players) {
            NOVA_INFO("Player {} left", m_playerBalls.size());
            scene().destroyEntity(m_playerBalls.back());
            m_playerBalls.pop_back();
        }
    }

    void drawDebug(float dt) {
        auto& dd = renderer().debugDraw();
        auto& marks = contactMarks();
        for (auto& c : marks) c.age += dt;
        std::erase_if(marks, [](const ContactMark& c) { return c.age > 1.5f; });
        if (m_showContacts)
            for (const auto& c : marks) {
                const float fade = 1.0f - c.age / 1.5f;
                dd.sphere(c.point, 0.05f, {1.0f, 0.3f, 0.2f, fade}, true);
                dd.arrow(c.point, c.point + c.normal * 0.6f, {1.0f, 0.9f, 0.2f, fade}, true);
            }
        if (!m_showColliders) return;

        auto& reg = scene().registry();
        for (auto [e, rb, t] : reg.view<nova::RigidBodyComponent, nova::TransformComponent>().each()) {
            const glm::vec4 color = rb.isSensor ? kSensor
                                    : rb.type == nova::physics::BodyType::Static    ? kStatic
                                    : rb.type == nova::physics::BodyType::Kinematic ? kKinematic
                                                                                    : kDynamic;
            const glm::quat rot = t.orientation();
            const glm::vec3 s = t.scaleF();
            const float maxScale = std::max({s.x, s.y, s.z}), radial = std::max(s.x, s.z);
            bool shaped = false;
            if (const auto* c = reg.try_get<nova::BoxColliderComponent>(e)) {
                dd.box(t.position + rot * (c->offset.toGlm() * s), c->halfExtents.toGlm() * s, rot * c->rotation.toGlm(), color);
                shaped = true;
            }
            if (const auto* c = reg.try_get<nova::SphereColliderComponent>(e)) {
                dd.sphere(t.position + rot * (c->offset.toGlm() * s), nova::toFloat(c->radius) * maxScale, color);
                shaped = true;
            }
            if (const auto* c = reg.try_get<nova::CapsuleColliderComponent>(e)) {
                dd.capsule(t.position + rot * (c->offset.toGlm() * s), rot * c->rotation.toGlm(),
                           nova::toFloat(c->radius) * radial, nova::toFloat(c->halfHeight) * s.y, color);
                shaped = true;
            }
            if (const auto* c = reg.try_get<nova::CylinderColliderComponent>(e)) {
                drawCylinder(dd, t.position + rot * (c->offset.toGlm() * s), rot * c->rotation.toGlm(),
                             nova::toFloat(c->radius) * radial, nova::toFloat(c->halfHeight) * s.y, color);
                shaped = true;
            }
            if (reg.any_of<nova::ConvexHullColliderComponent, nova::MeshColliderComponent>(e)) {
                dd.axes(t.position, t.matrixRelativeTo(t.position), 0.6f); // mesh shapes: just the frame
                shaped = true;
            }
            if (!shaped) dd.box(t.position, s * 0.5f, rot, color); // the default: a box matching the scale
        }
        for (auto [e, cc, t] : reg.view<nova::CharacterControllerComponent, nova::TransformComponent>().each()) {
            const float r = nova::toFloat(cc.radius), half = nova::toFloat(cc.height) * 0.5f - r;
            const nova::GlobalPosition center = t.position + cc.offset.toGlm();
            dd.capsule(center, glm::quat(1, 0, 0, 0), r, half, kCharacter);
            if (cc.grounded) dd.arrow(t.position, t.position + cc.groundNormal.toGlm(), kCharacter, true);
        }
    }

    // Builds the scenario twice in hidden scenes, runs both 300 physics steps and compares the
    // state hash after every step. UUIDs are random per entity, so both get the same numbering.
    std::string checkDeterminism(const Scenario& sc) {
        constexpr int kSteps = 300;
        auto run = [&](std::vector<nova::u64>& hashes) {
            nova::Scene s("Determinism check");
            sc.build(s, m_meshes);
            // Random UUIDs renumbered in the order the entities were made (their entt ids, the same
            // in both runs; the registry's iteration order is not guaranteed to be).
            std::unordered_map<nova::u64, nova::u64> renumber;
            std::vector<entt::entity> made;
            for (auto [e, id] : s.registry().view<nova::IdComponent>().each()) made.push_back(e);
            std::sort(made.begin(), made.end(), [](entt::entity a, entt::entity b) { return entt::to_integral(a) < entt::to_integral(b); });
            nova::u64 next = 1;
            for (entt::entity e : made) {
                auto& id = s.registry().get<nova::IdComponent>(e);
                renumber[id.uuid] = next;
                id.uuid = next++;
            }
            for (auto [e, j] : s.registry().view<nova::JointComponent>().each())
                if (j.connected) j.connected.uuid = renumber[j.connected.uuid];
            s.startSimulation();
            for (int i = 0; i < kSteps; ++i) {
                s.update(s.fixedTimeStep);
                hashes.push_back(s.stateHash());
            }
        };
        gRecordContacts = false;
        std::vector<nova::u64> a, b;
        run(a);
        run(b);
        gRecordContacts = true;
        for (int i = 0; i < kSteps; ++i)
            if (a[i] != b[i]) return "DIVERGED at step " + std::to_string(i + 1) + " of " + std::to_string(kSteps);
        char text[96];
        std::snprintf(text, sizeof text, "Deterministic: %d steps, same hash every step (%016llx)", kSteps,
                      static_cast<unsigned long long>(a.back()));
        return text;
    }

    BenchMeshes m_meshes;
    int m_current = 0;
    bool m_paused = false, m_stepOnce = false;
    float m_timeScale = 1.0f, m_fireTimer = 0.0f;
    float m_shotSpeed = 25.0f, m_shotMass = 3.0f;
    bool m_showColliders = false, m_showContacts = true;
    nova::Entity m_camera;
    float m_yaw = 0.0f, m_pitch = 0.0f;           // fly camera
    float m_orbitYaw = 0.0f, m_orbitPitch = -0.35f; // character camera
    std::vector<nova::Entity> m_spawned;      // scenario actions (bullets, dropped boxes)
    std::vector<nova::Entity> m_projectiles;  // shots
    std::vector<nova::Entity> m_playerBalls;  // index i = player i + 1
    std::string m_determinism;
};

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--check") PhysicsBench::s_selfCheck = true;
        else if (arg == "--scenario" && hasValue) PhysicsBench::s_startScenario = argv[++i];
        else if (arg == "--no-rt") PhysicsBench::s_noRt = true;
        else if (arg == "--no-cull") PhysicsBench::s_noCull = true;
        else if (arg == "--no-occlusion") PhysicsBench::s_noOcclusion = true;
        else if (arg == "--no-bloom") PhysicsBench::s_noBloom = true;
        else if (arg == "--auto-exposure") PhysicsBench::s_autoExposure = true;
        else if (arg == "--no-ssao") PhysicsBench::s_noSsao = true;
        else if (arg == "--debug-view" && hasValue) PhysicsBench::s_debugView = std::atoi(argv[++i]);
        else if (arg == "--frames" && hasValue) PhysicsBench::s_quitAfterFrames = std::atoi(argv[++i]);
        else if (arg == "--screenshot" && hasValue) PhysicsBench::s_screenshot = argv[++i];
        else if (arg == "--laps" && hasValue) PhysicsBench::s_wrapRepeat = std::atoi(argv[++i]);
        else if (arg == "--wrap" && hasValue) {
            const std::string_view v = argv[++i];
            PhysicsBench::s_wrapShift = v == "triangle" ? 0 : v == "object" ? 1 : v == "split" ? 2 : v.starts_with("crop") ? 3 : -1;
            PhysicsBench::s_cropCamera = v == "crop-camera";
        }
    }
    const int code = nova::run<PhysicsBench>({.name = "Nova Physics Bench", .width = 1280, .height = 720});
    return code != 0 ? code : PhysicsBench::s_checkFailed ? 1 : 0;
}
