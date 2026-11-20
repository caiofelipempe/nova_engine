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

class SandboxGame : public nova::GameLayer {
public:
    void onStart() override {
        auto camera = scene().createEntity("Camera");
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
    }

    void onTick(float) override {
        if (nova::Input::isKeyPressed(nova::Key::Escape)) app().quit();
    }

    // Optional debug UI (Dear ImGui is available to every layer).
    void onImGui() override {
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##stats", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("%.0f FPS | %u draws", app().fps(), renderer().stats().drawCalls);
        if (renderer().rayTracingAvailable())
            ImGui::Checkbox("Ray-traced shadows", &renderer().settings().rayTracedShadows);
        ImGui::TextDisabled("RMB + WASD to fly, Esc to quit");
        ImGui::End();
    }
};

int main() {
    return nova::run<SandboxGame>({.name = "Nova Sandbox", .width = 1280, .height = 720});
}
