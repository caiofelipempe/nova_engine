// Minimal game built on Nova: shows the whole public API surface a game needs.
#include <nova/Nova.h>

// Gameplay logic lives in native scripts attached to entities.
struct Bobbing : nova::Script {
    float phase = 0.0f;
    explicit Bobbing(float p) : phase(p) {}
    void onUpdate(float dt) override {
        phase += dt;
        transform().position.y = 0.75f + 0.35f * std::sin(phase * 2.0f);
    }
};

// Fly the camera with WASD + right mouse button.
struct FlyCamera : nova::Script {
    void onUpdate(float dt) override {
        auto& t = transform();
        if (nova::Input::isMouseButtonDown(nova::MouseButton::Right)) {
            glm::vec2 d = nova::Input::mouseDelta() * 0.003f;
            t.rotation.y -= d.x;
            t.rotation.x = glm::clamp(t.rotation.x - d.y, -1.5f, 1.5f);
        }
        float speed = (nova::Input::isKeyDown(nova::Key::LeftShift) ? 12.0f : 4.0f) * dt;
        if (nova::Input::isKeyDown(nova::Key::W)) t.position += t.forward() * speed;
        if (nova::Input::isKeyDown(nova::Key::S)) t.position -= t.forward() * speed;
        if (nova::Input::isKeyDown(nova::Key::D)) t.position += t.right() * speed;
        if (nova::Input::isKeyDown(nova::Key::A)) t.position -= t.right() * speed;
    }
};

// Glows briefly whenever its rigid body hits something.
struct FlashOnHit : nova::Script {
    float flash = 0.0f;
    void onCollision(nova::Entity, const glm::vec3&) override { flash = 1.0f; }
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
    }

    void onTick(float) override {
        if (nova::Input::isKeyPressed(nova::Key::Escape)) app().quit();
        if (nova::Input::isKeyPressed(nova::Key::Space)) shoot();
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
        ImGui::Text("%.0f FPS | %u draws", app().fps(), renderer().stats().drawCalls);
        if (renderer().rayTracingAvailable())
            ImGui::Checkbox("Ray-traced shadows", &renderer().settings().rayTracedShadows);
        ImGui::Text("Physics bodies: %u", scene().physics() ? scene().physics()->bodyCount() : 0u);
        // Sweep a projectile-sized sphere from the camera: shows what a shot would hit.
        auto& cam = m_camera.transform();
        if (auto hit = scene().sweepSphere(cam.position + cam.forward(), 0.2f, cam.forward(), 200.0f))
            ImGui::Text("Aim: %s (%.1f m)", hit->entity.name().c_str(), hit->distance);
        else
            ImGui::TextDisabled("Aim: nothing");
        ImGui::TextDisabled("RMB + WASD to fly, Space to shoot, Esc to quit");
        ImGui::End();
    }

private:
    nova::Entity m_camera;
    std::vector<nova::Entity> m_projectiles;
};

int main() {
    return nova::run<SandboxGame>({.name = "Nova Sandbox", .width = 1280, .height = 720});
}
