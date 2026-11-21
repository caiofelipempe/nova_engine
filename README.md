# Nova Engine

Engine 3D em C++20, distribuída como **biblioteca** (`Nova::Engine`). Usa Vulkan 1.3 e GLFW, ambos atrás de interfaces abstratas (backends), ECS com EnTT, Dear ImGui (docking) e um renderer híbrido com sombras por **ray tracing** (ray query).

## Usando a engine no seu `main`

```cpp
#include <nova/Nova.h>

struct Spin : nova::Script {                       // lógica de gameplay por entidade
    void onUpdate(float dt) override { transform().rotation.y += dt; }
};

struct MyGame : nova::GameLayer {                  // já atualiza e renderiza a Scene
    void onStart() override {
        auto cam = scene().createEntity("Camera");
        cam.add<nova::CameraComponent>();
        cam.transform().position = {0, 2, 6};

        auto sun = scene().createEntity("Sun");
        sun.add<nova::DirectionalLightComponent>();
        sun.transform().rotation = {glm::radians(-45.0f), 0, 0};

        auto box = scene().createEntity("Box");
        box.add<nova::MeshComponent>(renderer().cube());
        box.addScript<Spin>();
    }
};

int main() { return nova::run<MyGame>({.name = "My Game", .width = 1280, .height = 720}); }
```

E no CMake do jogo:

```cmake
add_subdirectory(path/to/nova)          # ou a pasta deste repositório
nova_add_app(MyGame src/main.cpp)       # linka Nova::Engine e copia os shaders
```

O exemplo completo está em [sandbox/src/main.cpp](sandbox/src/main.cpp).

**Níveis de API** — use só o que precisar:

| Nível | O que você escreve | Para quê |
|---|---|---|
| `nova::run<GameLayer>()` | só `onStart` / `onTick` | jogos simples |
| `Application` + `Layer` | vários layers, `onUpdate`/`onImGui`/`onRender` | ferramentas, editor |
| `Renderer` + `RenderScene` | listas de desenho manuais, sem ECS | integrações próprias |
| `rhi::Device` / `CommandList` | passes e pipelines próprios | features novas de rendering |

## Build

Requisitos: Visual Studio 2022, CMake ≥ 3.25, Vulkan SDK (glslc), vcpkg com `VCPKG_ROOT` definido.

```powershell
cmake --preset windows-vs2022
cmake --build --preset debug      # ou release
build\bin\Debug\NovaEditor.exe
build\bin\Debug\Sandbox.exe
ctest --preset debug              # testes headless (física, cenas, assets, shaders, espaço do mundo, tempo, cenários, input, editor, hot reload)
doxygen docs/Doxyfile             # referência da API em build/docs/html (requer Doxygen)
```

Shaders do jogo podem ser compilados em runtime (`NOVA_SHADER_COMPILER=ON`, padrão: usa o `shaderc_combined` do Vulkan SDK). Com `-DNOVA_SHADER_COMPILER=OFF` o compilador fica fora da engine e o jogo usa só SPIR-V pré-compilado (veja [Shaders do jogo](#shaders-do-jogo)).

Engine e editor são compilados como DLLs (`NOVA_SHARED=ON`, padrão) para que o código do jogo possa ser uma DLL recarregada a quente; com `-DNOVA_SHARED=OFF` viram bibliotecas estáticas (sem hot reload).

A API pública é documentada no estilo Doxygen nos headers de `engine/src/nova/`. Com o Doxygen instalado, o CMake também cria o target `NovaDocs`.

As dependências (manifest [vcpkg.json](vcpkg.json)) são instaladas automaticamente no configure: GLFW, Vulkan Memory Allocator, glm, EnTT, spdlog, Dear ImGui (docking), ImGuizmo, cgltf, stb_image, Jolt Physics, nlohmann-json.
Em Debug, as validation layers do Vulkan ficam ligadas e as mensagens vão para o console e para `nova.log`.

## Arquitetura

```
engine/src/nova/
  core/       Application (loop), Layer, Log (spdlog)
  math/       WorldSpace, GlobalPosition / GlobalOffset (posições inteiras exatas), TimePoint / Duration (tempo exato em flicks), rotações (quaternion + Euler em 6 ordens), unidades
  platform/   Window (interface) + Input        → glfw/  (backend GLFW)
  rhi/        Device, CommandList (interfaces)  → vulkan/ (backend Vulkan)
  renderer/   Renderer, RenderView, Mesh (+ raycast CPU), DebugDraw, ShaderLibrary — só fala com o RHI
  scene/      Scene (EnTT), Entity, Components, Script, GameLayer, SceneSerializer (JSON)
  physics/    PhysicsWorld (interface)          → jolt/ (backend Jolt)
  reflect/    reflexão de tipos (componentes, scripts, structs, enums)
  assets/     AssetDatabase (GUIDs em .meta, importadores), glTF (cgltf), imagens (stb_image)
  ui/         tema/fontes do ImGui
engine/shaders/  GLSL → SPIR-V em build-time (glslc); nova/*.glsl são headers públicos para shaders do jogo
editor/src/      Nova::Editor — framework de editor extensível (painéis, comandos, inspetores, plugins)
editor/app/      NovaEditor — editor da Nova montado sobre o framework (exemplo de editor de jogo)
editor/game/     NovaEditorGame — código do jogo de exemplo numa DLL recarregada a quente pelo editor
sandbox/         jogo mínimo usando só a API pública
```

**Backends.** `platform::Window` e `rhi::Device` são interfaces criadas por fábricas (`Window::create(WindowBackend::Glfw)`, `Device::create(Backend::Vulkan)`). Cada backend vive na sua pasta e é ligado por opções do CMake (`NOVA_RHI_VULKAN`, `NOVA_PLATFORM_GLFW`). A janela não depende de nenhuma API gráfica (a ponte WSI do Vulkan é type-erased), então dá para adicionar SDL3 ou D3D12 sem mexer nas camadas de cima.

**Modelo de binding (bindless).** Todos os backends seguem o mesmo contrato:
- buffers são acessados no shader por *device address* (64 bits, via push constants) — vértices são lidos por "vertex pulling";
- texturas ficam num array global de descritores (`Device::bindlessIndex`);
- acceleration structures também são referenciadas por endereço.

Isso mapeia direto para Vulkan (BDA + descriptor indexing) e para D3D12 (SM 6.6 `ResourceDescriptorHeap`), e é o mesmo modelo de que o ray tracing precisa (o hit shader tem de enxergar a geometria de qualquer objeto).

**Recursos.** São handles geracionais (`BufferHandle`, `TextureHandle`, …): handles obsoletos são detectados em vez de apontarem para outro recurso. A destruição é adiada até a GPU terminar os frames em voo. O estado das texturas é rastreado e as barreiras (sync2) são emitidas automaticamente.

**Frame.** Relativo à câmera (a GPU só vê posições medidas a partir dela; veja [docs/WORLD_SPACE.md](docs/WORLD_SPACE.md)). `[TLAS build] → sky → forward PBR (HDR, RGBA16F) → pós-processamento do jogo (HDR) → tonemap (ACES) → UI`. Os dados por frame vêm de um alocador linear num buffer de upload por frame em voo.

## Input e players

**Player 0 é teclado + mouse; players 1, 2, … são os gamepads conectados** (Xbox, PlayStation, Switch e outros, via o banco de mapeamentos SDL embutido no GLFW). Declare ações e eixos uma vez e consulte por player — o mesmo código serve para todos:

```cpp
nova::Input::bindAction("Jump", {.keys = {nova::Key::Space}, .gamepadButtons = {nova::GamepadButton::A}});
nova::Input::bindAction("Fire", {.mouseButtons = {nova::MouseButton::Left}, .gamepadTriggers = {nova::GamepadAxis::RightTrigger}});
nova::Input::bindAxis("Move", {.up = nova::Key::W, .down = nova::Key::S, .left = nova::Key::A,
                               .right = nova::Key::D, .stick = nova::GamepadStick::Left});

for (nova::u32 p = 0; p < nova::Input::playerCount(); ++p) {
    if (nova::Input::isActionPressed("Jump", p)) { /* ... */ }
    glm::vec2 move = nova::Input::axis("Move", p);     // +Y = frente/cima
}
```

- Ações: `isActionDown / Pressed / Released`; eixos 2D combinam 4 teclas (diagonais normalizadas) e opcionalmente o mouse para o player 0, e um analógico ou o d-pad para gamepads.
- Analógicos com **zona morta radial** reescalada (padrão 0.15) e +Y = cima; gatilhos de 0 a 1 com zona morta própria.
- Consultas diretas por dispositivo continuam disponíveis (`isKeyPressed`, `isMouseButtonPressed`, `isGamepadButtonPressed`, `gamepadLeftStick`, `gamepadDpad`…); controles sem mapeamento (volantes, manches) via `Input::joystickState` (eixos, botões e hats crus); mapeamentos extras com `Input::addGamepadMappings`.
- Conexões e desconexões aparecem no log; o Sandbox cria uma bola controlável para cada gamepad conectado.

## Editor extensível

O editor é uma biblioteca (`Nova::Editor`): cada jogo monta o **seu** editor registrando painéis, comandos (menu + atalho + paleta `Ctrl+Shift+P`) e inspetores de componentes — os painéis embutidos usam a mesma API e podem ser substituídos ou removidos.

```cpp
struct MyGameEditor : nova::editor::EditorApp {
    void onSetup(nova::editor::Registry& r) override {
        r.panel<WavesPanel>();
        r.component<Health>("Health", [](Health& h) { ImGui::DragFloat("HP", &h.current); });
        r.command("Tools/Spawn Wave", [](auto& ctx) { spawnWave(ctx.scene()); }, "Ctrl+W");
    }
};
int main() { return nova::editor::run<MyGameEditor>(); }
```

Código do jogo com **hot reload**: com `nova_add_game_module(MyGame ...)` e `NOVA_GAME_MODULE(MyGame)` o jogo vira uma DLL que o editor recompila (`Ctrl+B`) e troca sem fechar, mantendo a cena (veja o guia).

No CMake: `nova_add_editor(MyGameEditor editor/main.cpp ...)`. Guia com receitas: [docs/EDITOR.md](docs/EDITOR.md).

Recursos do editor: undo/redo automático para qualquer edição (incluindo ferramentas do jogo) com painel *History*; seleção múltipla (clique na viewport, Ctrl/Shift, hierarquia); ferramentas de viewport registráveis (gizmos Move/Rotate/Scale, exemplo de pincel); visualização de seleção, colliders, luzes e câmeras via `DebugDraw`; cenas em JSON com Play/Stop restaurando tudo; paleta de comandos; painel **Assets** (pasta `assets/`) com arrastar-e-soltar de modelos, prefabs e texturas para a viewport e importadores/ações registráveis pelo jogo.

## Reflexão e cenas

Componentes e scripts são descritos **uma vez** com reflexão (API fluente, sem macros); a partir disso a engine e o editor derivam o inspetor, o *Add Component*, a serialização de cenas, o Play/Stop e o *Duplicate*:

```cpp
nova::reflect::type<Health>("Health")
    .field("current", &Health::current, {.min = 0, .max = 1000})
    .field("invulnerable", &Health::invulnerable, {.tooltip = "Ignores all damage"})
    .component();

nova::AssetDatabase assets(&renderer);
assets.setRoot("assets");                                    // GUIDs em <arquivo>.meta
assets.instantiate(assets.findByPath("models/crate/crate.gltf")->guid, scene, {0, 0, 0});
nova::saveScene(scene, "assets/levels/forest.scene.json", assets.serializeOptions());
nova::loadScene(scene, "assets/levels/forest.scene.json", assets.serializeOptions());
```

- Cenas em JSON legível e estável para diff (`"format": "nova.scene"`): enums por nome, malhas/texturas por referência de asset (`"asset:<guid>#0"`) ou nome, floats curtos e exatos, ordem de campos preservada.
- Prefabs (`.prefab.json`): `serializePrefab`/`instantiatePrefab` ou `AssetDatabase::createPrefab`/`instantiate`; posições relativas à primeira entidade, UUIDs novos a cada instância.
- Toda entidade tem um UUID (`IdComponent`) que sobrevive a salvar/abrir e ao Play/Stop.
- Arquivos antigos continuam abrindo: campos novos usam o padrão, componentes/scripts desconhecidos são ignorados com aviso.
- Os componentes da engine são refletidos em [engine/src/nova/reflect/EngineTypes.cpp](engine/src/nova/reflect/EngineTypes.cpp).

## Física (Jolt Physics)

A física 3D usa o [Jolt Physics](https://github.com/jrouwe/JoltPhysics) (multi-thread, usado em Horizon Forbidden West), atrás da interface `physics::PhysicsWorld` — mesmo esquema de backends do RHI (`NOVA_PHYSICS_JOLT`). Nenhum header do Jolt vaza para a API pública.

```cpp
auto box = scene().createEntity("Crate");
box.add<nova::MeshComponent>(renderer().cube());
box.add<nova::RigidBodyComponent>();                    // dinâmico, 1 kg
box.add<nova::BoxColliderComponent>();                  // opcional: sem collider usa uma caixa do tamanho do scale

ground.add<nova::RigidBodyComponent>(nova::physics::BodyType::Static);

scene().setLinearVelocity(ball, dir * 25.0f);           // também addForce / addImpulse
if (auto hit = scene().raycast(origin, dir)) hit->entity.name();

struct Bomb : nova::Script {                            // eventos de colisão nos scripts
    void onCollision(const nova::Collision& c) override {
        // c.other, c.point / c.otherPoint, c.normal (empurra este corpo para fora),
        // c.surfaceNormal (minha face que tocou) e c.otherSurfaceNormal (a face do outro)
    }
};
```

**Consultas (queries):**

```cpp
scene().raycast(origin, dir, maxDist, ignoreEntity);
// Sweep tests (shape casts): movem uma forma ao longo de uma direção e reportam o primeiro contato
scene().sweepSphere(origin, radius, dir);
scene().sweepBox(center, halfExtents, rotation, dir);
scene().sweepCapsule(center, radius, halfHeight, rotation, dir);
scene().sweepAll(shapeDesc, pos, rot, dir);                    // todos ao longo do caminho, mais próximo primeiro
scene().sweepEntity(player, velocity, dist);                   // varre o collider da própria entidade, ignorando-a
```

Cada hit traz `entity`, `point`, `distance` (0 = já encostado no início) e três normais:

| Campo | Significado | Uso típico |
|---|---|---|
| `normal` | normal de **contato** (direção de separação), aponta para quem consultou | deslizar, empurrar para fora |
| `hitSurfaceNormal` | face do corpo **atingido** voltada para o contato | "é chão andável?", decalques |
| `sweptSurfaceNormal` / `sweptPoint` | face/ponto da forma **varrida** no instante do impacto (zero em raycasts) | "qual face minha bateu?", alinhar ao pousar |

Em superfícies lisas (esfera numa face) as três coincidem; em arestas e vértices divergem. As normais de superfície usam a *supporting face* voltada para o outro corpo (não o ponto), então continuam corretas quando o contato cai exatamente numa quina; em formas curvas (esfera, cápsula) usa-se a normal no ponto. Os eventos de contato (`physics::ContactEvent`, `Script::onCollision`) trazem o mesmo par de normais e de pontos. No backend, `PhysicsWorld::raycast/sweep/sweepAll/sweepBody` aceitam um `QueryFilter` (ignorar um corpo, incluir sensores).

- Corpos **Static**, **Kinematic** (seguem o transform do ECS e empurram os dinâmicos) e **Dynamic**; colliders Box, Sphere, Capsule e Cylinder com offset, **ConvexHull** (poliedro: envoltória convexa dos vértices de uma malha, qualquer tipo de corpo) e **MeshCollider** (malha de triângulos exata, para cenário estático/kinematic — num corpo dinâmico vira a envoltória); tamanhos multiplicados pelo scale. Malhas vazias usam a `MeshComponent` da entidade.
- Passo fixo exato (`Duration`, 60 Hz) com acumulador; transforms dos dinâmicos são escritos de volta no ECS.
- **Contrato global:** a engine fala com a física só em `GlobalPosition` (`physics::PhysicsWorld`); o Jolt implementa um contrato `float` local e é envolvido pelo `FloatingOriginWorld`, o único lugar onde posições viram `float`. Um motor que trabalhe em inteiros implementa `PhysicsWorld` direto: `scene().startSimulation(makeScope<MeuMundo>())`.
- **Origem flutuante:** o Jolt simula em `float` em torno da câmera (ou de `scene().setPhysicsFocus(p)`), com rebase automático; consultas e colisões usam `GlobalPosition`, e bordas `Barrier` do mundo viram paredes (`Script::onWorldBoundary`). Veja [docs/WORLD_SPACE.md](docs/WORLD_SPACE.md).
- `GameLayer` liga a simulação sozinho. No editor: **Play / Pause / Stop** (Stop restaura os transforms), gizmo arrasta corpos durante o Play.
- Próximos passos: character controller, joints, interpolação de transforms na renderização.

## Shaders do jogo

O jogo traz os próprios shaders — **materiais**, **pós-processamento** e **compute** — e escolhe, por shader, de onde vem o SPIR-V:

| Origem | Como | Quando usar |
|---|---|---|
| **Runtime** | GLSL compilado pela engine (shaderc), com cache em disco e **hot reload** quando o arquivo (ou um `#include`) muda | desenvolvimento, shaders gerados/configuráveis, editor |
| **Pré-compilado** | SPIR-V gerado no build por `nova_add_game_shaders` (glslc), copiado para `<exe>/shaders` | jogo final, sem compilador nem custo de compilação |
| **Auto** (padrão) | runtime se a engine tem o compilador e a fonte existe; senão, pré-compilado | o mesmo código serve aos dois casos |

```cpp
auto& shaders = renderer().shaders();                                  // nova::ShaderLibrary
auto water = shaders.load("assets/shaders/water.frag");                 // Auto
auto blur  = shaders.load("blur.comp", {.source = nova::ShaderSource::Precompiled});
auto gen   = shaders.compile("gen.frag", sourceText, nova::ShaderStage::Fragment); // gerado em memória

// Material: um fragment shader que só altera a superfície; luz, sombras RT e debug views são da engine.
nova::Material m;
m.shader = water;
m.customParams = {0.1f, 0.4f, 0.8f, 2.0f};  // Surface::custom no GLSL
m.customTexture = foamTexture;              // nova_customTexture(uv)

// Pós-processamento: passes em HDR, na ordem, antes da exposição/tonemap.
renderer().settings().postProcess.push_back({.name = "Vignette", .shader = vignette, .params = {0.5f, 1.5f, 0, 0}});

// Compute: pipeline em cache, reconstruído quando o shader recarrega (busque o handle a cada dispatch).
cmd.bindPipeline(renderer().computePipeline(blur));
```

Um material (`#include "nova/material.glsl"`) implementa `material(inout Surface s)`; `s` já chega preenchido pelo `Material` (cor × textura, checker, metallic, roughness, emissive). O `forward.frag` da própria engine é exatamente essa interface com um `material()` vazio:

```glsl
#version 460
#include "nova/material.glsl"

void material(inout Surface s) {
    float scan = step(0.5, fract(s.worldPos.y * 8.0 - nova_time())); // worldPos: mundo módulo 4096 m
    s.albedo   = mix(s.albedo, s.custom.rgb, 0.6);
    s.emissive += s.custom.rgb * scan;            // também: s.normal, s.metallic, s.roughness, s.unlit
}
```

Um efeito de pós (`#include "nova/postprocess.glsl"`) implementa `vec3 postProcess(vec2 uv)` usando `nova_scene(uv)`, `nova_params()`, `nova_texelSize()` e `nova_time()`. Exemplos: [assets/shaders/hologram.frag](assets/shaders/hologram.frag) e [assets/shaders/vignette.frag](assets/shaders/vignette.frag).

**Pré-compilando no CMake:**

```cmake
nova_add_game_shaders(MyGame
    MATERIALS assets/shaders/water.frag       # + variante ray query (water.frag.rq.spv)
    SHADERS   assets/shaders/vignette.frag assets/shaders/blur.comp
    DEPENDS   assets/shaders/noise.glsl)      # includes do jogo que devem disparar rebuild
```

Detalhes:
- Os headers `nova/*.glsl` vão para `<exe>/shaders/include`; `#include "x.glsl"` também resolve ao lado do arquivo que inclui.
- O renderer compila sozinho a **variante ray query** de cada material (`NOVA_RAY_QUERY`) na primeira vez que ela é usada; sem ela (ou sem RT na GPU) o material é desenhado sem sombras traçadas.
- Um erro de compilação mantém o SPIR-V anterior e registra o erro (`ShaderLibrary::error`); se o *primeiro* compile falhar, o objeto usa o material padrão até o arquivo ser corrigido. Pipelines são recriados quando `version()` muda.
- Cache em `<exe>/shader_cache`, validado por hash do conteúdo da fonte e de cada include.
- Shaders são assets (`.vert`, `.frag`, `.comp` e as formas `.glsl`/`.spv`): cenas guardam `Material::shader` por GUID. No editor: campo **Shader** no inspetor (combo ou arrastar do painel Assets), soltar o shader sobre um objeto no viewport, ações *Apply to Selection* / *Reload*, e o painel **Renderer** lista os shaders (origem, versão, erros) e os passes de pós.

## Ray tracing

Já implementado:
- detecção das capacidades (`DeviceCaps::rayQuery`, `rayTracingPipeline`) e ativação automática das extensões;
- uma **BLAS** por mesh, criada junto com a mesh;
- uma **TLAS** por frame em voo, reconstruída a cada frame a partir das instâncias da cena;
- **sombras por ray query** no fragment shader (sol e point lights), num pipeline-variante (`forward_rq.frag`, e a variante `rq` de cada material do jogo). Em GPUs sem RT, o pipeline raster normal é usado sem nenhuma mudança no código do jogo.

Próximos passos (a estrutura já está preparada):
1. `rhi::RayTracingPipelineDesc` + `CommandList::traceRays` + shader binding table — os entry points (`vkCreateRayTracingPipelinesKHR`, `vkCmdTraceRaysKHR`) já são carregados quando há suporte.
2. Um path tracer de referência (raygen escrevendo num storage image bindless — o binding 1 já existe).
3. Reflexos e AO por ray query no mesmo pass forward.
4. Compactação de BLAS e refit da TLAS para cenas grandes.

## Roadmap geral

- **Mundos grandes** com posições globais inteiras (ponto fixo em `int64`, escala ajustável, bordas com volta ou barreira): plano e status em [docs/WORLD_SPACE.md](docs/WORLD_SPACE.md); fases 1 (tipos e aritmética), 2 (cena: `TransformComponent::position` é um `GlobalPosition`, mundo salvo na cena) 3 (renderização relativa à câmera) 4 (física com origem flutuante, paredes nas barreiras) 5 (editor: painel World, bordas visíveis, câmera atravessando a emenda) e 6 (regiões de física por foco, corpos dormentes, determinismo com `stateHash`) concluídas.
- **Física com posições inteiras nativas** (para um motor de física próprio): capacidade declarada pelo backend (`caps().globalPositions`); variante inteira do contrato do `PhysicsWorld` (corpos, transforms, consultas e contatos em `GlobalPosition`/`GlobalOffset`, `step(Duration)`); com um backend inteiro a cena usa uma região só, sem origem flutuante nem `float` no caminho — determinismo entre plataformas. O Jolt continua pelo caminho atual. Inclui um backend esqueleto validado pelos testes de interface. Detalhes em [docs/WORLD_SPACE.md](docs/WORLD_SPACE.md).
- **Renderização circular em mundos que dão a volta:** hoje a física é circular mas cada objeto é desenhado uma vez (na cópia mais próxima da câmera); desenhar as cópias por volta dentro do alcance da câmera (instâncias extras com frustum culling por cópia, sem saltos na fronteira), incluindo luzes, TLAS das sombras, DebugDraw e picking. Plano em [docs/WORLD_SPACE.md](docs/WORLD_SPACE.md).
- Shaders do jogo, próximos passos: vertex shaders de material (deslocamento de vértices), materiais transparentes/blend, profundidade no pós-processamento, passes compute dentro do frame do renderer, e shaders de RT do jogo (raygen/miss/hit) quando o pipeline de RT existir.
- Editor: receitas para IA / servidor MCP (fase 5).
- Renderer: materiais com mais texturas (normal, metallic-roughness, emissive), shadow maps como fallback sem RT, render graph, culling em GPU, backend D3D12.
- Cena: hierarquia de transforms; character controller.
