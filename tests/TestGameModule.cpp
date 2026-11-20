// A small game module used by EditorTests to exercise hot reload. Built twice: version 1 and
// version 2 (TEST_MODULE_VERSION) differ like two builds of a game would.
#include <nova/editor/Editor.h>

#ifndef TEST_MODULE_VERSION
    #define TEST_MODULE_VERSION 1
#endif

namespace {

struct Spin {
    float speed = 1.0f;
    int turns = 0;
#if TEST_MODULE_VERSION >= 2
    float bonus = 3.0f; // a field added in the new build
#endif
};

struct Waver : nova::Script {
    float height = 0.5f;
    void onUpdate(float) override { transform().position.y = height * TEST_MODULE_VERSION; }
};

class TestPanel final : public nova::editor::Panel {
public:
    explicit TestPanel(int* attached) : Panel("Test Panel"), m_attached(attached) {}
    void onAttach(nova::editor::EditorContext&) override { ++*m_attached; }
    void onDetach(nova::editor::EditorContext&) override { --*m_attached; }
    void onImGui(nova::editor::EditorContext&) override {}

private:
    int* m_attached;
};

int g_panelsAttached = 0;

struct TestPlugin final : nova::editor::Plugin {
    void onRegister(nova::editor::Registry& r) override {
        r.panel<TestPanel>(&g_panelsAttached);
        r.command("Test/Version", [](nova::editor::EditorContext&) {}, "Ctrl+H").order = TEST_MODULE_VERSION;
        r.assetAction("TestData", "Inspect", [](nova::editor::EditorContext&, const nova::AssetRecord&) {});
        r.fieldDrawer("Spin", "turns", [](void*, const nova::reflect::FieldInfo&, nova::editor::EditorContext&) { return false; });
        r.uiHook([](nova::editor::EditorContext&) {});
    }
};

struct TestModule final : nova::editor::GameModule {
    void onLoad(nova::editor::Registry& r) override {
        auto spin = nova::reflect::type<Spin>("Spin");
        spin.field("speed", &Spin::speed).field("turns", &Spin::turns);
#if TEST_MODULE_VERSION >= 2
        spin.field("bonus", &Spin::bonus);
#endif
        spin.component();
        nova::reflect::type<Waver>("Waver").field("height", &Waver::height).script();
        r.plugin<TestPlugin>();
        r.context().assets().addImporter({
            .type = "TestData",
            .extensions = {".testdata"},
            .load = [](nova::AssetDatabase&, const std::filesystem::path&, nova::AssetData& out) {
                out.payload = std::make_shared<std::string>("v" + std::to_string(TEST_MODULE_VERSION));
                return true;
            },
        });
    }
};

} // namespace

NOVA_GAME_MODULE(TestModule)

/// Lets the test check which build is loaded and whether panels are attached.
extern "C" NOVA_MODULE_EXPORT int testModuleVersion() { return TEST_MODULE_VERSION; }
extern "C" NOVA_MODULE_EXPORT int testModulePanelsAttached() { return g_panelsAttached; }
