// Minimal game built on Nova: shows the whole public API surface a game needs.
#include <nova/Nova.h>

// Gameplay logic lives in native scripts attached to entities.
struct Bobbing : nova::Script {
    float phase = 0.0f;
    nova::GlobalPosition base;
    explicit Bobbing(float p) : phase(p) {}
    void onCreate() override { base = transform().position; }
    void onUpdate(float dt) override {
        phase += dt;
        transform().position = base + glm::vec3(0.0f, 0.35f * std::sin(phase * 2.0f), 0.0f);
    }
};

// Input is declared once as named actions/axes. Player 0 = keyboard + mouse, players 1.. = gamepads.
void bindControls() {
    using nova::Input;
    Input::bindAxis("Move", {.up = nova::Key::W, .down = nova::Key::S, .left = nova::Key::A, .right = nova::Key::D,
                             .stick = nova::GamepadStick::Left});
    Input::bindAxis("Look", {.mouseDelta = true, .mouseScale = 0.003f});
    Input::bindAction("Fast", {.keys = {nova::Key::LeftShift}});
    Input::bindAction("Shoot", {.keys = {nova::Key::Space}});
    Input::bindAction("Jump", {.gamepadButtons = {nova::GamepadButton::A}});
}

// Player 0 flies the camera: WASD to move, hold RMB to look.
struct FlyCamera : nova::Script {
    void onUpdate(float dt) override {
        auto& t = transform();
        if (nova::Input::isMouseButtonDown(nova::MouseButton::Right)) {
            glm::vec2 look = nova::Input::axis("Look"); // +Y = up
            t.rotation.y -= look.x;
            t.rotation.x = glm::clamp(t.rotation.x + look.y, -1.5f, 1.5f);
        }
        glm::vec2 move = nova::Input::axis("Move");
        float speed = (nova::Input::isActionDown("Fast") ? 12.0f : 4.0f) * dt;
        t.position += (t.forward() * move.y + t.right() * move.x) * speed;
    }
};

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
        if (grounded && nova::Input::isActionPressed("Jump", player))
            scene().addImpulse(entity(), {0.0f, 12.0f, 0.0f});
    }
};

// Glows briefly whenever its rigid body hits something.
struct FlashOnHit : nova::Script {
    float flash = 0.0f;
    void onCollision(const nova::Collision&) override { flash = 1.0f; }
    void onUpdate(float dt) override {
        flash = std::max(0.0f, flash - dt * 3.0f);
        auto& m = get<nova::MeshComponent>().material;
        m.emissive = {1.0f, 0.6f, 0.2f};
        m.emissiveStrength = flash * 4.0f;
    }
};

class SandboxGame : public nova::GameLayer {
public:
    void onStart() override {
        bindControls();

        auto camera = m_camera = scene().createEntity("Camera");
        camera.add<nova::CameraComponent>();
        camera.transform().position = {0.0f, 3.0f, 9.0f};
        camera.transform().lookAt({0.0f, 0.5f, 0.0f});
        camera.addScript<FlyCamera>();

        auto sun = scene().createEntity("Sun");
        sun.add<nova::DirectionalLightComponent>();
        sun.transform().rotation = {glm::radians(-50.0f), glm::radians(35.0f), 0.0f};

        auto ground = scene().createEntity("Ground");
        ground.transform().scale = {40.0f, 1.0f, 40.0f};
        nova::Material floor;
        floor.baseColor = {0.8f, 0.8f, 0.8f, 1.0f};
        floor.checker = true;
        ground.add<nova::MeshComponent>(renderer().plane(), floor);
        // Physics: static body; a thin box whose top face sits at y = 0.
        ground.add<nova::RigidBodyComponent>(nova::physics::BodyType::Static);
        ground.add<nova::BoxColliderComponent>(glm::vec3(0.5f, 0.05f, 0.5f), glm::vec3(0.0f, -0.05f, 0.0f));

        // A stack of dynamic crates.
        for (int y = 0; y < 5; ++y) {
            for (int x = 0; x < 3; ++x) {
                auto box = scene().createEntity("Crate");
                box.transform().position = {3.0f + x * 1.02f, 0.5f + y * 1.01f, -4.0f};
                nova::Material m;
                m.baseColor = {0.55f, 0.38f, 0.2f, 1.0f};
                m.roughness = 0.7f;
                box.add<nova::MeshComponent>(renderer().cube(), m);
                box.add<nova::RigidBodyComponent>(); // dynamic, box collider from scale
                box.addScript<FlashOnHit>();
            }
        }

        for (int i = 0; i < 5; ++i) {
            auto ball = scene().createEntity("Ball " + std::to_string(i));
            ball.transform().position = {-4.0f + 2.0f * i, 0.75f, 0.0f};
            nova::Material m;
            m.baseColor = {0.9f, 0.25f + 0.15f * i, 0.2f, 1.0f};
            m.roughness = 0.15f + 0.2f * i;
            ball.add<nova::MeshComponent>(renderer().sphere(), m);
            ball.addScript<Bobbing>(static_cast<float>(i));
        }

        auto crate = scene().createEntity("Spinning Crate");
        crate.transform().position = {0.0f, 1.0f, -3.0f};
        crate.transform().scale = glm::vec3(1.5f);
        crate.add<nova::MeshComponent>(renderer().cube());
        crate.add<nova::RotatorComponent>(glm::vec3(0.3f, 1.0f, 0.0f), 0.8f);
        // Kinematic: driven by the Rotator, but pushes dynamic bodies around.
        crate.add<nova::RigidBodyComponent>(nova::physics::BodyType::Kinematic);

        // Game shaders, precompiled at build time (nova_add_game_shaders in CMakeLists.txt).
        // ShaderSource::Auto would compile assets/shaders/*.frag at runtime when the source is there.
        auto& shaders = renderer().shaders();
        nova::Material holo;
        holo.shader = shaders.load("hologram.frag", {.source = nova::ShaderSource::Precompiled});
        holo.baseColor = {0.1f, 0.12f, 0.15f, 1.0f};
        holo.customParams = {1.0f, 0.4f, 0.9f, 14.0f}; // rgb = color, a = scanline density
        auto hologram = scene().createEntity("Hologram");
        hologram.transform().position = {-3.0f, 1.2f, -3.0f};
        hologram.transform().scale = glm::vec3(1.6f);
        hologram.add<nova::MeshComponent>(renderer().sphere(), holo);

        if (auto vignette = shaders.load("vignette.frag", {.source = nova::ShaderSource::Precompiled}))
            renderer().settings().postProcess.push_back({.name = "Vignette", .shader = vignette, .params = {0.5f, 1.5f, 0, 0}});
    }

    void onTick(float) override {
        if (nova::Input::isKeyPressed(nova::Key::Escape)) app().quit();
        if (nova::Input::isActionPressed("Shoot")) shoot();
        syncPlayerBalls();
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
            ball.add<nova::MeshComponent>(renderer().sphere(), m);
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

    void shoot() {
        auto& cam = m_camera.transform();
        auto ball = scene().createEntity("Projectile");
        ball.transform().position = cam.position + cam.forward() * 1.0f;
        ball.transform().scale = glm::vec3(0.4f);
        nova::Material m;
        m.baseColor = {0.2f, 0.8f, 0.35f, 1.0f};
        m.roughness = 0.3f;
        ball.add<nova::MeshComponent>(renderer().sphere(), m);
        ball.add<nova::RigidBodyComponent>().mass = 3.0f;
        ball.add<nova::SphereColliderComponent>(); // radius 0.5 * scale 0.4
        scene().setLinearVelocity(ball, cam.forward() * 25.0f);

        m_projectiles.push_back(ball);
        if (m_projectiles.size() > 50) { // keep the world small
            scene().destroyEntity(m_projectiles.front());
            m_projectiles.erase(m_projectiles.begin());
        }
    }

    // Optional debug UI (Dear ImGui is available to every layer).
    void onImGui() override {
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##stats", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("%.0f FPS | %u draws | %u post passes", app().fps(), renderer().stats().drawCalls,
                    renderer().stats().postProcessPasses);
        if (renderer().rayTracingAvailable())
            ImGui::Checkbox("Ray-traced shadows", &renderer().settings().rayTracedShadows);
        ImGui::Text("Physics bodies: %u", scene().physics() ? scene().physics()->bodyCount() : 0u);
        // Sweep a projectile-sized sphere from the camera: shows what a shot would hit.
        auto& cam = m_camera.transform();
        if (auto hit = scene().sweepSphere(cam.position + cam.forward(), 0.2f, cam.forward(), 200.0f))
            ImGui::Text("Aim: %s (%.1f m)", hit->entity.name().c_str(), hit->distance);
        else
            ImGui::TextDisabled("Aim: nothing");
        for (nova::u32 p = 0; p < nova::Input::playerCount(); ++p)
            ImGui::Text("Player %u: %s", p, nova::Input::playerDeviceName(p).c_str());
        ImGui::TextDisabled("P0: RMB + WASD to fly, Space to shoot, Esc to quit");
        ImGui::TextDisabled("Gamepads: left stick to roll, A to jump");
        ImGui::End();
    }

private:
    nova::Entity m_camera;
    std::vector<nova::Entity> m_projectiles;
    std::vector<nova::Entity> m_playerBalls; // index i = player i + 1
};

int main() {
    return nova::run<SandboxGame>({.name = "Nova Sandbox", .width = 1280, .height = 720});
}
