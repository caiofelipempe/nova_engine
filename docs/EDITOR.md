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

### Ferramenta da viewport (gizmo, pincel, posicionamento)

```cpp
class PaintTool : public nova::editor::Tool {
public:
    PaintTool() : Tool("Paint Targets", "B") {}                 // botão na toolbar + View/Tools + atalho
    bool clickSelects() const override { return false; }        // o botão esquerdo pinta (não seleciona)
    bool isInteracting() const override { return m_stroke.active(); }

    void onViewport(EditorContext& ctx, const ViewportState& vp) override {
        auto hit = nova::editor::pick(ctx.scene(), [&](auto m) { return ctx.renderer().meshData(m); },
                                      vp.rayOrigin, vp.rayDirection);
        if (vp.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_stroke = ctx.beginCreation("Paint");
        if (m_stroke.active() && hit) { /* criar entidades em hit->point, alinhadas a hit->normal */ }
        if (m_stroke.active() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_stroke.commit(); // 1 passo de undo
    }
    void onDebugDraw(EditorContext&, nova::DebugDraw& dd) override { /* prévia do pincel */ }

private:
    nova::editor::CreationTransaction m_stroke;
};
// registro: r.tool<PaintTool>();
```

- `ViewportState`: retângulo da imagem, mouse, câmera e **raio do mouse no mundo** (`rayOrigin`, `rayDirection`).
- `nova::editor::pick(...)`: entidade sob um raio (triângulos exatos das malhas; esfera pequena para luzes/câmeras), com `PickOptions::ignore`.
- Ferramentas embutidas: **Move (W)**, **Rotate (E)**, **Scale (R)** — gizmo que move toda a seleção. Exemplo completo de pincel: `PaintTargetsTool` em [editor/app/ExamplePlugin.cpp](../editor/app/ExamplePlugin.cpp).

### Undo / redo

Quase sempre é **automático**: qualquer mudança nas entidades **selecionadas** (inspetor, gizmo, painel do jogo, comando) vira um passo quando o usuário solta o widget/gizmo. Para o resto, use transações:

```cpp
ctx.edit("Align to ground", entities, [&] { /* modificar */ });         // modificação
ctx.recordCreation("Spawn wave", [&] { /* criar entidades */ });        // criação (undo apaga)
ctx.destroyEntities("Delete enemies", entities);                         // remoção (undo restaura tudo)
auto tx = ctx.beginEdit("Drag", entities); /* vários frames */ tx.commit(); // interações longas
```

Os passos guardam snapshots JSON por **UUID**, então continuam válidos mesmo quando entidades são apagadas e recriadas. Durante o Play nada é gravado (o Stop já restaura a cena). Estado fora da cena: `ctx.undoStack().push({nome, undo, redo})`.

### Linhas de debug

```cpp
auto& dd = ctx.renderer().debugDraw();   // também disponível no jogo: app().renderer().debugDraw()
dd.arrow(from, to, {1, 1, 0, 1});
dd.box(transform, center, halfExtents, {0, 1, 0, 1});
dd.sphere(p, r, color, /*onTop*/ true);   // também circle, capsule, grid, axes, line
```

Desenhadas em todo frame em que forem adicionadas (com teste de profundidade, ou por cima com `onTop`).

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

### Assets: importador e ação do jogo

A pasta de assets (`EditorDesc::assetsDir`, padrão `assets/`) aparece no painel **Assets**. Cada arquivo reconhecido por um importador vira um asset com GUID estável num arquivo `<arquivo>.meta` (versione os `.meta` junto). Cenas referenciam malhas e texturas por `"asset:<guid>#<índice>"`, então mover/renomear arquivos (com o `.meta`) não quebra nada.

```cpp
// Um tipo de arquivo do jogo (no onSetup, ou em qualquer lugar com acesso ao AssetDatabase):
ctx.assets().addImporter({
    .type = "Dialogue",
    .extensions = {".dlg.json"},
    .load = [](nova::AssetDatabase&, const std::filesystem::path& file, nova::AssetData& out) {
        out.payload = std::make_shared<Dialogue>(loadDialogue(file));
        return true;
    },
    // opcional: .instantiate = [...] cria entidades (arrastar para a viewport, "Add to Scene")
});
// O que fazer com ele no painel (a primeira ação roda no duplo clique):
r.assetAction("Dialogue", "Preview", [](EditorContext& ctx, const nova::AssetRecord& a) {
    openDialoguePreview(ctx.assets().load(a.guid)->as<Dialogue>());
});
```

- Embutidos: **Texture** (png/jpg/tga/bmp), **Model** (gltf/glb, com textura base color), **Prefab** (`.prefab.json`), **Scene** (`.scene.json`).
- Arrastar para a viewport: modelos/prefabs são colocados onde o raio do mouse acerta (apoiados na superfície); texturas vão para a malha sob o mouse. Arrastar uma textura para um campo de textura do Inspector também funciona.
- Prefabs: selecione entidades → *Assets/Create Prefab from Selection*. Por código: `ctx.assets().createPrefab(entities, "prefabs/torre")` e `ctx.instantiateAsset(guid, posição)` (um passo de undo, seleciona o resultado).
- *File/Import Asset...* (Ctrl+I) copia um arquivo externo (e os `.bin`/imagens de um glTF) para a pasta de assets.

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
| Seleção (múltipla) | `ctx.selection()`, `ctx.selected()` (primária), `ctx.select(e)`, `addToSelection`, `toggleSelection`, `setSelection`, `isSelected`, `clearSelection()` |
| Undo / redo | `ctx.undo()`, `ctx.redo()`, `ctx.edit(...)`, `ctx.recordCreation(...)`, `ctx.destroyEntities(...)`, `ctx.beginEdit(...)`, `ctx.beginCreation(...)`, `ctx.undoStack()` |
| Ferramentas | `ctx.activeTool()`, `ctx.setActiveTool(tool)` |
| Modo de jogo | `ctx.play()`, `ctx.pause()`, `ctx.stop()`, `ctx.playState()` — o Stop restaura a cena inteira (entidades criadas somem, destruídas voltam); handles mudam, **UUIDs** (`entity.uuid()`, `scene.findEntityByUuid`) não |
| Arquivos de cena | `ctx.newScene()`, `ctx.openScene(path)`, `ctx.saveScene(path)`, `ctx.scenePath()` |
| Assets | `ctx.assets()` (`AssetDatabase`: `find`, `findByPath`, `load`, `instantiate`, `import`, `createPrefab`, `addImporter`), `ctx.instantiateAsset(guid, pos)`, `ctx.spawnPoint()` |
| Câmera do editor | `ctx.camera()` (`focus(pos)`, `position`, `yaw`, `pitch`) |
| Engine | `ctx.app()`, `ctx.renderer()` |
| Extensões | `ctx.registry()` (`execute("id")`, `findPanel<T>()`...) |

Widgets prontos com o visual do editor: `nova::editor::widgets::vec3`, `widgets::material`, `widgets::componentHeader`.

## 4. Comandos e atalhos embutidos

| Menu | Atalho |
|---|---|
| File/New Scene · Open Scene... · Save Scene · Save Scene As... | Ctrl+N · Ctrl+O · Ctrl+S · Ctrl+Shift+S |
| File/Import Asset... | Ctrl+I |
| Assets/Create Prefab from Selection · Refresh | — |
| Edit/Undo · Redo | Ctrl+Z · Ctrl+Y (ou Ctrl+Shift+Z) |
| Edit/Select All · Duplicate · Delete · Focus Selection · Deselect | Ctrl+A · Ctrl+D · Delete · F · Escape |
| Entity/Create Empty, Cube, Sphere, Plane, Point Light | — |
| Play/Play · Pause · Stop | F5 · F6 · Shift+F5 |
| View/Tools/Move · Rotate · Scale · Local Space | W · E · R |
| View/Show Selection Bounds · All Colliders · Light and Camera Gizmos · Grid | — |
| Viewport: clique seleciona (Ctrl alterna, Shift adiciona); Hierarchy: Ctrl/Shift (intervalo) | — |
| Window/Reset Layout · Command Palette... | — · Ctrl+Shift+P |

## 5. Convenções (para pessoas e IA)

- Uma extensão por arquivo; registro em uma linha no plugin do jogo.
- Não altere `nova/editor/*` para adicionar ferramentas de jogo — use o `Registry`.
- Lógica testável fora da UI: coloque regras em funções que recebem `Scene&`/`EditorContext&` e teste sem janela (veja `tests/EditorTests.cpp`).
- Exemplo completo: [editor/app/ExamplePlugin.cpp](../editor/app/ExamplePlugin.cpp) (componente, painel, comando com atalho, sinal).
