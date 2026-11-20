# Estendendo o editor Nova

O editor é uma **biblioteca** (`Nova::Editor`). Cada jogo cria o seu próprio executável de editor e registra nele as ferramentas de que precisa. Os painéis, menus e inspetores do editor padrão são registrados pela **mesma API**, então qualquer um deles pode ser substituído ou removido.

```
EditorApp::onSetup(Registry&)   ← registre aqui painéis, comandos, inspetores, plugins
EditorApp::onNewScene(ctx)      ← monte/carregue a cena inicial
EditorContext                   ← cena, seleção, Play/Pause/Stop, câmera, sinais
```

## 1. Criar o editor do jogo

```cmake
# CMakeLists.txt do jogo
add_subdirectory(path/to/nova)
nova_add_editor(MyGameEditor editor/main.cpp editor/WavesPanel.cpp)
```

```cpp
// editor/main.cpp
#include <nova/editor/Editor.h>

struct MyGameEditor : nova::editor::EditorApp {
    void onSetup(nova::editor::Registry& r) override {
        r.plugin<MyGamePlugin>();
    }
    void onNewScene(nova::editor::EditorContext& ctx) override { loadLevel(ctx.scene()); }
};

int main() { return nova::editor::run<MyGameEditor>({.layoutFile = "mygame_editor.ini"}); }
```

`EditorDesc` também permite desligar partes embutidas: `.builtinPanels`, `.builtinCommands`, `.builtinComponents`.

## 2. Receitas

Cada receita é **um arquivo** + **uma linha de registro**. Não é preciso alterar a engine.

### Painel

```cpp
class WavesPanel : public nova::editor::Panel {
public:
    WavesPanel() : Panel("Waves", nova::editor::DockSlot::Right) {}
    void onImGui(nova::editor::EditorContext& ctx) override {
        for (auto [e, wave] : ctx.scene().view<Wave>().each())
            ImGui::Text("Wave %d: %d enemies", wave.index, wave.count);
    }
};
// registro: r.panel<WavesPanel>();
```

- `DockSlot`: `Left`, `Right`, `RightBottom`, `Bottom`, `Center`, `Floating` (posição no layout padrão e no *Window → Reset Layout*).
- Opcional: `onAttach/onDetach` (recursos, conectar sinais), `onUpdate(ctx, dt)`, `onRender(ctx, cmd)` (desenhar na GPU), `noPadding`, `windowFlags`.
- O painel aparece automaticamente no menu *Window*.

### Comando (menu + atalho + paleta)

```cpp
r.command("Tools/Spawn Wave", [](nova::editor::EditorContext& ctx) { spawnWave(ctx.scene()); }, "Ctrl+W");
```

Para controle total, use a struct `Command`: `id`, `menuPath`, `shortcut`, `execute`, `isEnabled`, `isChecked` (marca de seleção), `order`, `separatorBefore`, `showInPalette`. Registrar um `id` que já existe **substitui** o comando (inclusive os embutidos).

Atalhos aceitam `Ctrl`, `Shift`, `Alt` + `A`–`Z`, `0`–`9`, `F1`–`F12`, `Space`, `Enter`, `Escape`, `Delete`, `Tab`, setas etc. Eles não disparam enquanto se digita em um campo de texto nem enquanto a câmera está sendo pilotada.

Comandos em `Entity/...` também aparecem no menu de contexto da *Hierarchy*.

### Componente do jogo (reflexão — o caminho recomendado)

Descreva o componente **uma vez**; inspetor, *Add Component*, salvar/abrir cena, Play/Stop e *Duplicate* funcionam sozinhos:

```cpp
struct Health { float current = 100, max = 100; bool invulnerable = false; };

// uma vez, no início do main (antes de carregar cenas)
nova::reflect::type<Health>("Health")
    .field("current", &Health::current, {.min = 0, .max = 1000})
    .field("max", &Health::max, {.min = 1, .max = 1000})
    .field("invulnerable", &Health::invulnerable, {.tooltip = "Ignores all damage"})
    .component();
```

- Tipos de campo: `bool`, `int`, `u32`, `u64`, `float`, `glm::vec2/3/4`, `std::string`, enums (`reflect::enumeration<E>("E").value("A", E::A)...`), `MeshHandle` e structs refletidas (aninhadas).
- `FieldAttrs`: `min`/`max` (slider), `speed`, `degrees` (guarda radianos, edita graus), `color`, `readOnly`, `hidden` (salva, não mostra), `transient` (estado de runtime: nem salva nem mostra), `tooltip`, `label`.
- O **nome do campo** é o que vai no arquivo: renomeá-lo quebra cenas salvas. Campos novos/removidos são tolerados (valores padrão / ignorados).
- Para controlar a ordem no inspetor ou ajustar a entrada gerada: `r.component<Health>().canAdd = ...;`.

### Script do jogo refletido

```cpp
struct Bob : nova::Script {
    float amplitude = 0.3f, speed = 2.0f;
    void onUpdate(float dt) override { /* ... */ }
};
nova::reflect::type<Bob>("Bob").field("amplitude", &Bob::amplitude).field("speed", &Bob::speed).script();
```

Aparece no inspetor como **Script: Bob** (adicionar, remover, editar campos), é salvo na cena e restaurado no Stop. Scripts não refletidos continuam funcionando, mas não são editáveis nem salvos.

### Widget customizado (por tipo ou por campo)

```cpp
r.fieldDrawer<Range>([](Range& v, const nova::reflect::FieldInfo& f, auto&) {   // todo campo do tipo Range
    return ImGui::DragFloatRange2(f.label.c_str(), &v.min, &v.max);
});
r.fieldDrawer("Health", "current", [](void* p, const auto& f, auto&) {          // só Health.current
    return ImGui::SliderFloat(f.label.c_str(), static_cast<float*>(p), 0, 100);
});
```

### Inspetor totalmente manual (quando a UI não é "um widget por campo")

```cpp
r.component<Health>("Health", [](Health& h) {
    ImGui::ProgressBar(h.current / h.max);
    ImGui::DragFloat("Current", &h.current, 1.0f, 0.0f, h.max);
});
```

- A função pode receber `(T&)`, `(T&, Entity)` ou `(T&, Entity, EditorContext&)`; um editor manual com o mesmo nome substitui o gerado.
- Personalize pelo retorno: `.add`, `.remove = nullptr`, `.canAdd`, `.defaultOpen`.

### Plugin (agrupar ferramentas)

```cpp
struct NavmeshPlugin : nova::editor::Plugin {
    void onRegister(nova::editor::Registry& r) override {
        r.panel<NavmeshPanel>();
        r.command("Tools/Bake Navmesh", &bakeNavmesh, "Ctrl+B");
        r.component<NavAgent>("Nav Agent", drawNavAgent);
    }
};
// registro: r.plugin<NavmeshPlugin>();
```

### Reagir a eventos do editor

```cpp
void onAttach(EditorContext& ctx) override {
    m_id = ctx.selectionChanged.connect([](nova::Entity e) { /* ... */ });
}
void onDetach(EditorContext& ctx) override { ctx.selectionChanged.disconnect(m_id); }
```

Sinais disponíveis: `selectionChanged(Entity)`, `playStateChanged(PlayState)`, `sceneReloaded()` (novo, abrir, Stop — refaça caches de entidades aqui).

### Popups, sobreposições e barra de menus

```cpp
r.uiHook([](EditorContext& ctx) { /* janelas modais, overlays: chamado todo frame */ });
r.menuBarWidget([](EditorContext& ctx) { ImGui::TextDisabled("Level: %s", currentLevel()); });
```

### Substituir ou remover algo embutido

```cpp
r.removePanel("Console");
r.panel<MyBetterInspector>();                       // painel com o mesmo título substitui
r.command("edit.delete", ...);                      // comando com o mesmo id substitui
r.removeComponentEditor("Rotator");
if (auto* vp = r.findPanel<nova::editor::builtin::ViewportPanel>()) vp->useSceneCamera = true;
```

## 3. Serviços do `EditorContext`

| Serviço | API |
|---|---|
| Cena | `ctx.scene()` (entidades, componentes, física, consultas) |
| Seleção | `ctx.selected()`, `ctx.select(e)`, `ctx.clearSelection()` |
| Modo de jogo | `ctx.play()`, `ctx.pause()`, `ctx.stop()`, `ctx.playState()` — o Stop restaura a cena inteira (entidades criadas somem, destruídas voltam); handles mudam, **UUIDs** (`entity.uuid()`, `scene.findEntityByUuid`) não |
| Arquivos de cena | `ctx.newScene()`, `ctx.openScene(path)`, `ctx.saveScene(path)`, `ctx.scenePath()` |
| Câmera do editor | `ctx.camera()` (`focus(pos)`, `position`, `yaw`, `pitch`) |
| Engine | `ctx.app()`, `ctx.renderer()` |
| Extensões | `ctx.registry()` (`execute("id")`, `findPanel<T>()`...) |

Widgets prontos com o visual do editor: `nova::editor::widgets::vec3`, `widgets::material`, `widgets::componentHeader`.

## 4. Comandos e atalhos embutidos

| Menu | Atalho |
|---|---|
| File/New Scene · Open Scene... · Save Scene · Save Scene As... | Ctrl+N · Ctrl+O · Ctrl+S · Ctrl+Shift+S |
| File/Import glTF... | Ctrl+I |
| Edit/Duplicate · Delete · Focus Selection · Deselect | Ctrl+D · Delete · F · Escape |
| Entity/Create Empty, Cube, Sphere, Plane, Point Light | — |
| Play/Play · Pause · Stop | F5 · F6 · Shift+F5 |
| View/Gizmo/Translate · Rotate · Scale · Local Space | W · E · R |
| Window/Reset Layout · Command Palette... | — · Ctrl+Shift+P |

## 5. Convenções (para pessoas e IA)

- Uma extensão por arquivo; registro em uma linha no plugin do jogo.
- Não altere `nova/editor/*` para adicionar ferramentas de jogo — use o `Registry`.
- Lógica testável fora da UI: coloque regras em funções que recebem `Scene&`/`EditorContext&` e teste sem janela (veja `tests/EditorTests.cpp`).
- Exemplo completo: [editor/app/ExamplePlugin.cpp](../editor/app/ExamplePlugin.cpp) (componente, painel, comando com atalho, sinal).
