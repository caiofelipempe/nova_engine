# Nova Engine

Engine 3D em C++20, distribuída como **biblioteca** (`Nova::Engine`). Usa Vulkan 1.3 e GLFW, ambos atrás de interfaces abstratas (backends), ECS com EnTT, Dear ImGui (docking), física Jolt determinística e um renderer híbrido. Com **ray tracing**, as sombras, os reflexos e o AO saem por ray query, e há um pipeline de RT com path tracer de referência. Sem RT, as sombras vêm de **shadow maps** (em cascata para o sol, de cubo para as point lights).

## Usando a engine no seu `main`

```cpp
#include <nova/Nova.h>

struct Spin : nova::Script {                       // lógica de gameplay por entidade
    void onUpdate(float dt) override { transform().rotateLocal(dt, {0, 1, 0}); }
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

O exemplo completo está em [sandbox/src/main.cpp](sandbox/src/main.cpp): uma bancada de testes de física (pilhas, materiais, formas, juntas, CCD, personagem, sensores e layers, bordas do mundo) com pausa, passo a passo, colliders e contatos visíveis e verificação de determinismo.

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
build\bin\Debug\Sandbox.exe       # bancada de física (--check: determinismo de todos os cenários; --frames N: sai após N frames e grava no nova.log as estatísticas de render e os tempos de frame (média, mínimo, mediana, p99, máximo, frames irregulares); --paused: começa com a simulação pausada; --no-vsync: frames sem esperar o monitor, para medir custo)
ctest --preset debug              # testes headless (física, cenas, assets, shaders, espaço do mundo, tempo, cenários, input, editor, hot reload)
doxygen docs/Doxyfile             # referência da API em build/docs/html (requer Doxygen)
```

Shaders do jogo podem ser compilados em runtime (`NOVA_SHADER_COMPILER=ON`, padrão: usa o `shaderc_combined` do Vulkan SDK). Com `-DNOVA_SHADER_COMPILER=OFF` o compilador fica fora da engine e o jogo usa só SPIR-V pré-compilado (veja [Shaders do jogo](#shaders-do-jogo)).

O mundo é circular. Com `NOVA_WRAP_RENDER_MODES` (ligada por padrão), `RenderSettings::wrapShift` escolhe como desenhar o que cruza a emenda: por triângulo (o padrão), por objeto, recortado na emenda, ou cortado numa volta só (`Crop`, a maquete). `wrapRepeat` repete o mundo até o horizonte. Com a opção desligada (`-DNOVA_WRAP_RENDER_MODES=OFF`, preset `windows-vs2022-nowrap`, em `build-nowrap/`), cada objeto aparece na cópia mais próxima da câmera.

Engine e editor são compilados como DLLs (`NOVA_SHARED=ON`, padrão) para que o código do jogo possa ser uma DLL recarregada a quente; com `-DNOVA_SHARED=OFF` viram bibliotecas estáticas (sem hot reload).

A API pública é documentada no estilo Doxygen nos headers de `engine/src/nova/`. Com o Doxygen instalado, o CMake também cria o target `NovaDocs`.

As dependências (manifest [vcpkg.json](vcpkg.json)) são instaladas automaticamente no configure: GLFW, Vulkan Memory Allocator, glm, EnTT, spdlog, Dear ImGui (docking), ImGuizmo, cgltf, stb (imagens: leitura e PNG das capturas), Jolt Physics, nlohmann-json, miniaudio. O Jolt vem de um *overlay port* ([cmake/vcpkg-ports/joltphysics](cmake/vcpkg-ports/joltphysics), ligado em [vcpkg-configuration.json](vcpkg-configuration.json)): a mesma versão, compilada com `CROSS_PLATFORM_DETERMINISTIC=ON`; o configure recusa um Jolt sem isso.
Em Debug, as validation layers do Vulkan ficam ligadas e as mensagens vão para o console e para `nova.log`.

## Arquitetura

```
engine/src/nova/
  core/       Application (loop), Layer, Log (spdlog)
  math/       WorldSpace, GlobalPosition / GlobalOffset (posições inteiras exatas), TimePoint / Duration (tempo exato em flicks), rotações (quaternion + Euler em 6 ordens), unidades
  platform/   Window (interface) + Input        → glfw/  (backend GLFW)
  audio/      Audio (miniaudio), clipes, fontes e ouvinte
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
sandbox/         bancada de testes de física usando só a API pública
```

**Backends.** `platform::Window` e `rhi::Device` são interfaces criadas por fábricas (`Window::create(WindowBackend::Glfw)`, `Device::create(Backend::Vulkan)`). Cada backend vive na sua pasta e é ligado por opções do CMake (`NOVA_RHI_VULKAN`, `NOVA_PLATFORM_GLFW`). A janela não depende de nenhuma API gráfica (a ponte WSI do Vulkan é type-erased), então dá para adicionar SDL3 sem mexer nas camadas de cima. Um backend D3D12 exige mais (veja o modelo de binding abaixo).

**Modelo de binding (bindless).** Todos os backends seguem o mesmo contrato:
- buffers são acessados no shader por *device address* (64 bits, via push constants) — vértices são lidos por "vertex pulling";
- texturas ficam num array global de descritores (`Device::bindlessIndex`), e storage images em outro (`Device::storageIndex`);
- acceleration structures também são referenciadas por endereço.

Isso mapeia direto para Vulkan (BDA + descriptor indexing) e é o modelo de que o ray tracing precisa (o hit shader tem de enxergar a geometria de qualquer objeto). As texturas mapeariam para o D3D12 (SM 6.6 `ResourceDescriptorHeap`), mas os **buffers por endereço não**: o HLSL não tem ponteiros em shader. Um backend D3D12 exige antes trocar, nos shaders (`GL_EXT_buffer_reference`) e no RHI, os endereços por buffers indexados num heap de descritores.

**Recursos.** São handles geracionais (`BufferHandle`, `TextureHandle`, …): handles obsoletos são detectados em vez de apontarem para outro recurso. A destruição é adiada até a GPU terminar os frames em voo. O estado das texturas é rastreado e as barreiras (sync2) são emitidas automaticamente.

**Frame.** Relativo à câmera (a GPU só vê posições medidas a partir dela; veja [docs/WORLD_SPACE.md](docs/WORLD_SPACE.md)). Cada frame é um render graph (a sequência completa está em [Render graph](#render-graph)): céu e forward PBR em HDR (RGBA16F), refinamentos (SSAO, TAA), pós-processamento, bloom, tonemap (ACES), anti-aliasing e UI. Os dados por frame vêm de um alocador linear num buffer de upload por frame em voo.

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
- Conexões e desconexões aparecem no log; no cenário Gamepads do Sandbox, cada gamepad conectado ganha uma bola controlável.
- **Olhar com o mouse:** `Input::setCursorCaptured(true)` esconde e trava o cursor. O movimento fica sem limite de tela e vem em contagens brutas do mouse, sem a aceleração do ponteiro do sistema; o salto que a plataforma faz ao reposicionar o cursor não conta como movimento. O editor e o Sandbox capturam enquanto o botão direito está pressionado e giram 0,0008 rad por contagem, perto do padrão dos jogos. Valores bem maiores fazem cada contagem andar vários pixels na tela, e a rotação fica espaçada em movimentos lentos. No editor, a opção é `EditorCamera::lookSensitivity`, também no painel Renderer.

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

**Hierarquia.** Uma entidade pode ser filha de outra e segue o pai, mantendo o deslocamento dela (posição, rotação e escala no referencial do pai):

```cpp
wheel.setParent(car);                       // mantém a pose no mundo; recusa laços
car.transform().position = {20, 0, 0};      // a roda vai junto (no próximo update, ou scene.updateTransforms())
scene.setLocalTransform(wheel, {{2, 0, 1}, glm::dquat(1, 0, 0, 0), glm::dvec3(1)}); // deslocamento do pai
for (nova::Entity c : car.children()) { /* ... */ }  // e parent(), scene.descendantsOf(car)
```

- O `TransformComponent` continua sendo a pose **no mundo** (posição `int64`): renderização, física e ferramentas não mudam. O `ParentComponent` guarda só a referência ao pai (por UUID, salva e remapeada em prefabs); o deslocamento sai das poses salvas.
- A cena põe os filhos no lugar antes dos scripts, depois deles e depois da física (`Scene::update`), e antes de desenhar. Um filho movido por outra coisa (script, gizmo, o próprio corpo dinâmico) fica onde foi posto e passa a ter o deslocamento dali. Corpos estáticos e cinemáticos filhos seguem o pai; um dinâmico se move sozinho.
- Apagar um pai apaga os descendentes; duplicar copia a subárvore (os filhos copiados seguem a cópia); um prefab leva os descendentes, e um pai que fica de fora é esquecido. Os filhos de um corpo interpolado são desenhados com ele.
- Modelos glTF chegam com a árvore dos nós: uma raiz com o nome do arquivo e cada nó sob o seu pai (nós sem malha viram transforms vazios; primitivas extras de uma malha, filhas do nó). Mover a raiz move o modelo inteiro.
- **Animação dos nós:** as animações do glTF (posição, rotação e escala por nó; passo, linear ou spline cúbica) viram `AnimationClip`s. Um modelo animado ganha um `AnimatorComponent` na raiz, que toca o primeiro clipe e escreve a pose local dos nós (`clip`, `speed`, `loop`, `playing`). Cada canal acha o nó pelo caminho de nomes a partir da raiz ("Porta/Dobradiça"), então sobrevive a salvar, duplicar e prefabs. O clipe é salvo por referência (`"asset:<guid>#<índice>"`) e resolvido pela fonte de animações da cena (`scene.setAnimationSource(assets.animationSource())`, que o editor já faz). O tempo não é salvo: uma cena aberta toca do início. Também dá para tocar um clipe próprio: `animator.data = std::make_shared<AnimationClip>(...)`. **Troca e mistura:** `scene.playAnimation(e, clipe, 0.3f)` troca de clipe com um cross-fade (o anterior continua tocando enquanto some); `scene.blendAnimation(e, clipe, peso, sync)` mistura outro clipe com um peso relativo ao principal (peso 0 remove; com `sync`, segue a fase do principal, como andar e correr com os passos juntos). Por nó e propriedade, os valores são a média pelos pesos (rotações como quatérnios); o que nenhum clipe anima fica como está. **Máscaras por osso:** `scene.blendAnimation(e, aceno, mix)` com um `AnimationMix` (`weight`, `sync`, `mode`, `mask`). A máscara lista caminhos de nós ("Quadril/Tronco"), cada um valendo para o nó e tudo abaixo dele. No modo `Override`, a camada substitui o resultado das outras pelo seu peso (0 a 1), em ordem: com peso 1 e máscara no tronco, a parte de cima acena enquanto o clipe principal anda com o resto. No modo `Average` (o padrão), a camada entra na média só nos nós da máscara. As camadas não são salvas. **Eventos:** `AnimationClip::events` marca momentos do clipe (`{0.4f, "passo", 1.0f}`: tempo, nome e um valor). Quando o tempo do clipe passa por um evento (para a frente ou para trás, em cada volta de um clipe em laço, uma vez num clipe sem laço), os scripts da entidade recebem `onAnimationEvent`, e `scene.animationEvents()` lista os do último update. Um evento em 0 dispara quando o clipe começa e a cada volta. Dispara o clipe principal e as camadas misturadas com `AnimationMix::events`; um clipe sumindo num fade não dispara. O glTF não tem eventos: acrescente numa cópia do clipe. No editor, o inspetor do Animator escolhe o clipe pelo nome: primeiro os do próprio modelo da entidade, depois os de outros modelos do projeto (`AssetDatabase::animationNames`); a referência aparece na dica do campo. O exemplo `assets/models/windmill/windmill.gltf` (na cena de demonstração) tem dois clipes, "spin" e "wobble".
- **Skinning:** malhas com esqueleto (`MeshData::skin`: até 4 juntas e pesos por vértice, num buffer à parte; malhas rígidas não pagam nada). O glTF traz os esqueletos (juntas pelo caminho de nomes, matrizes de ligação inversas) e os pesos; a malha ganha um `SkinComponent` (`root`, a raiz do modelo, e `skin`, a referência ao esqueleto, resolvida pela fonte de esqueletos da cena: `scene.setSkinSource(assets.skinSource())`). As juntas são as entidades dos nós: a animação, scripts ou o gizmo as movem, e o vertex shader deforma a malha com as matrizes delas, calculadas a cada quadro em dupla precisão relativas à malha (exatas longe da origem). Sombras por ray tracing seguem a deformação: a cada quadro, um compute shader (`skin.comp`) deforma os vértices de cada malha com esqueleto num buffer próprio e o BLAS dela é reajustado (refit, `CommandList::updateBLAS`, BLAS criado com `allowUpdate`); objetos que somem por 2 s têm esses recursos liberados. O picking do editor também acerta a pose atual (`Scene::skinPose` dá as matrizes das juntas e `MeshData::posed` a malha deformada, com a mesma conta dos shaders). O sandbox tem o cenário **Skinning** (colunas que dobram).
- No editor, a *Hierarchy* é uma árvore: arraste uma entidade para outra para torná-la filha, ou para o espaço vazio abaixo para virar raiz (com desfazer). Duplicar e apagar agem nas raízes da seleção.

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
- **Estado em ponto fixo** (`math/Fixed.h`, lib `fpm`): rotação (Q1.30) e escala do transform, todos os campos dos componentes de física, juntas e personagem, e a gravidade, cada um com o ponto escolhido para o menor movimento que o Jolt resolve. Números entram direto (`rb.mass = 2.5f`, `box.halfExtents = {1, 0.5, 1}`, `t.rotation = glm::angleAxis(...)`) e são arredondados ali; contas e comparações com números também funcionam (`rb.friction > 0.5f`), e formatos diferentes não se misturam sem querer. `float` só sai explicitamente (`toFloat(v)`, `v.toGlm()`, `t.orientation()`): no Jolt, no renderer e no ImGui. Os arquivos guardam o decimal mais curto que volta aos mesmos bits; scripts recebem o tempo exato em `onUpdate(Duration)` (o `onUpdate(float)` continua como atalho).
- **Determinismo entre plataformas:** com o estado em ponto fixo e o Jolt compilado com `CROSS_PLATFORM_DETERMINISTIC`, a mesma cena dá os mesmos bits em qualquer build e plataforma (lockstep, replays). `scene().stateHash()` resume o estado; o teste `physics` confere um hash de referência gravado, igual em Debug e Release.
- **Contrato global:** a engine fala com a física só em `GlobalPosition` (`physics::PhysicsWorld`); o Jolt implementa um contrato `float` local e é envolvido pelo `TorusWorld`, o único lugar onde posições viram `float`. Um motor que trabalhe em inteiros implementa `PhysicsWorld` direto: `scene().startSimulation(makeScope<MeuMundo>())`.
- **Jolt direto, em relação a um `WorldOrigin`:** corpos, juntas e personagens vão para um único mundo Jolt, com os ids dele, em `float` medido a partir do `WorldOrigin` (`scene().worldOrigin()`). O `WorldOrigin` é uma posição `int64` numa grade de células de até 256 m. Ele segue a câmera (ou `scene().setPhysicsFocus(p)`) com histerese, ou fica fixo com `scene().setWorldOrigin(p)`. Cada corpo vai para a cópia mais próxima da origem. Em mundos pequenos, os estáticos levam na própria forma cópias a uma volta, para existirem dos dois lados da emenda da janela sem quinas entre as cópias. Sem regiões nem dormência; consultas e colisões usam `GlobalPosition`. Veja [docs/WORLD_SPACE.md](docs/WORLD_SPACE.md).
- **Mundo sempre circular** (`scene().setWorldSize(WorldSize::meters(60, 0, 60))`, salvo na cena): cada eixo tem um tamanho (padrão 16 384 m; 0 = o padrão) e dá a volta. As posições `int64` Q45.18 dão a volta sozinhas, `b - a` é sempre o caminho mais curto, e o Jolt colide através da emenda. `Script::onWorldWrap` avisa quando a física leva um corpo de um lado para o outro.
- `GameLayer` liga a simulação sozinho. No editor: **Play / Pause / Stop** (Stop restaura os transforms), gizmo arrasta corpos durante o Play.
- **Contatos e filtros:** `Script::onCollision` / `onCollisionEnd` (um início e um fim por par de corpos, também ao destruir ou adormecer) e `onTriggerEnter` / `onTriggerExit` para sensores; 32 camadas de colisão com matriz e nomes em `Scene::physicsSettings()` (salvos na cena, junto com a gravidade); máscara de camadas em todas as consultas e `overlap` / `overlapSphere` / `overlapBox`.
- **Corpos:** edições durante o Play valem na hora (propriedades no lugar; tipo, massa, colliders e escala reconstroem o corpo mantendo a velocidade); `continuousCollision` para corpos rápidos; travas de eixo (`locks`); vários colliders numa entidade (com `rotation`) formam uma forma composta.
- **Joints** (`JointComponent`: fixed, point, hinge, slider, distance) com limites, motor, mola e força de quebra (`Script::onJointBreak`); `EntityRef` liga à outra entidade e é remapeado em prefabs.
- **Character controller** (`CharacterControllerComponent`, o `CharacterVirtual` do Jolt): scripts definem `velocity`; desliza em paredes, sobe degraus e rampas, gruda no chão, empurra corpos e é visto por consultas e sensores.
- Estrutura: frontend (`physics/PhysicsWorld.h`, `scene/PhysicsComponents.h`, `scene/ScenePhysics.cpp`) separado do backend (`physics/backend/`: contrato `float` local, origem flutuante e Jolt). Testes em `tests/PhysicsFeatureTests.cpp`.
- **Movimento suave:** um corpo cinemático movido num quadro com vários passos anda o mesmo tanto em cada passo. `Scene::interpolatePhysics` (ligado por padrão) desenha corpos dinâmicos, cinemáticos e personagens entre as poses dos dois últimos passos, pelo tempo que sobrou, no máximo um passo atrás. Os transforms guardam a pose simulada. Câmeras que seguem um corpo usam `scene().renderTransform(e)`.

## Materiais

O `Material` segue o modelo PBR do glTF:
- cor base (`baseColor` × `baseColorTexture`, sRGB), metálico e rugosidade (× `metallicRoughnessTexture`: G rugosidade, B metálico);
- `normalTexture` (mapa de normal em espaço tangente, convenção glTF; `normalScale`), aplicado sem tangentes guardadas, pelas derivadas de posição e UV na tela;
- `occlusionTexture` (R; `occlusionStrength`), que escurece a luz ambiente; `emissiveTexture` (sRGB) × `emissive` × `emissiveStrength`;
- `alphaMode`: `Opaque`, `Mask` (recorte abaixo de `alphaCutoff`: folhagem, cercas) ou `Blend` (transparente: desenhado depois dos opacos, de trás para frente, sem escrever profundidade e sem projetar sombra por ray tracing);
- `doubleSided`: as duas faces (sem descarte de faces de trás).
- `uvScale`, `uvRotation`, `uvOffset`: transformação de UV de todas as texturas (como a `KHR_texture_transform` do glTF, lida da textura de cor); escala 4 repete a textura 4 vezes.

O glTF traz tudo isso, criando cada textura no espaço de cor certo (cor e emissiva em sRGB, os mapas de dados lineares; `Renderer::createTexture2D(..., srgb)`). Toda textura ganha a **cadeia completa de mipmaps**, gerada no upload (`Device::uploadTexture`, por blits lineares), e é amostrada com filtragem trilinear e anisotrópica (16×): de longe ou de lado, ela fica filtrada em vez de cintilar. Formatos que não aceitam blit linear ficam com um nível. Num recorte (`Mask`), a média do alfa nos níveis menores pode afinar o recorte à distância. Materiais de jogo (`nova/material.glsl`) recebem `Surface::alpha` e `Surface::occlusion` também. O cenário **Surfaces** do sandbox mostra cada recurso. As sombras por ray tracing respeitam o recorte: os raios testam o alfa da textura no ponto atingido (instâncias recortadas são não opacas no TLAS), então uma grade projeta sombra com os furos.

## Áudio

Som com a [miniaudio](https://miniaud.io) (vcpkg), atrás de uma classe `Audio` que não expõe a biblioteca:
- `AudioClip`: o som já decodificado (WAV, MP3 e FLAC: `decodeAudioFile`). Arquivos de som em `assets/` são assets ("Sound"): arrastados para a cena viram uma entidade tocando em laço.
- `AudioSourceComponent` toca numa entidade (`clip` por referência, volume, pitch, laço, "tocar ao começar", espacial com distâncias mínima e máxima); `AudioListenerComponent` marca o ouvinte (sem ele, a câmera principal). Scripts: `scene.playSource(e)`, `stopSource`, `playSound(clip, posição)` para um som avulso num ponto do mundo e `playSound(clip)` para música e interface.
- Cada som é posicionado relativo ao ouvinte pela diferença das posições `int64` (exato longe da origem e através das emendas do mundo circular), e só então vira `float` para a saída.
- Sem dispositivo de áudio (ou com `Audio(true)`), a saída é silenciosa mas as vozes andam no tempo e terminam: testes e servidores funcionam igual. `Application::audio()` é a saída padrão de toda cena.
- No editor, o inspetor escolhe o som pelo nome. Exemplos: `assets/sounds/hum.wav` (o moinho da cena de demonstração zumbe no Play) e `chime.wav`; no sandbox, as batidas dos objetos (`FlashOnHit`) fazem um estalo no ponto do contato.

## Shaders do jogo

O jogo traz os próprios shaders — **materiais**, **pós-processamento**, **compute** e **ray tracing** (veja [Ray tracing](#ray-tracing)) — e escolhe, por shader, de onde vem o SPIR-V:

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

Um efeito de pós (`#include "nova/postprocess.glsl"`) implementa `vec3 postProcess(vec2 uv)` usando `nova_scene(uv)`, `nova_params()`, `nova_texelSize()`, `nova_time()` e a **profundidade**: `nova_depth(uv)` (0..1, 1 = céu), `nova_linearDepth(uv)` (distância ao plano da câmera, em metros) e `nova_viewPosition(uv)`. Exemplos: [assets/shaders/hologram.frag](assets/shaders/hologram.frag), [assets/shaders/vignette.frag](assets/shaders/vignette.frag) e [assets/shaders/fog.frag](assets/shaders/fog.frag) (neblina por distância; `Sandbox --fog`).

**Pré-compilando no CMake:**

```cmake
nova_add_game_shaders(MyGame
    MATERIALS assets/shaders/water.frag       # + variante ray query (water.frag.rq.spv)
    SHADERS   assets/shaders/vignette.frag assets/shaders/blur.comp
    DEPENDS   assets/shaders/noise.glsl)      # includes do jogo que devem disparar rebuild
```

Detalhes:
- Os headers `nova/*.glsl` vão para `<exe>/shaders/include`; `#include "x.glsl"` também resolve ao lado do arquivo que inclui.
- O renderer compila sozinho a **variante ray query** de cada material (`NOVA_RAY_QUERY`) na primeira vez que ela é usada; ela traz as sombras, os reflexos e o AO traçados. Sem ela (ou sem RT na GPU), o material é desenhado com shadow maps e a aproximação especular da luz ambiente.
- Um erro de compilação mantém o SPIR-V anterior e registra o erro (`ShaderLibrary::error`); se o *primeiro* compile falhar, o objeto usa o material padrão até o arquivo ser corrigido. Pipelines são recriados quando `version()` muda.
- Cache em `<exe>/shader_cache`, validado por hash do conteúdo da fonte e de cada include.
- Shaders são assets (`.vert`, `.frag`, `.comp`, `.rgen`, `.rmiss`, `.rchit`, `.rahit` e as formas `.glsl`/`.spv`; só fragment shaders entram como material): cenas guardam `Material::shader` por GUID. No editor: campo **Shader** no inspetor (combo ou arrastar do painel Assets), soltar o shader sobre um objeto no viewport, ações *Apply to Selection* / *Reload*, e o painel **Renderer** lista os shaders (origem, versão, erros) e os passes de pós.

## Render graph

Cada frame é um `RenderGraph` (`nova/renderer/RenderGraph.h`). Os passes declaram as texturas que leem e escrevem, e são executados na ordem em que foram adicionados:

`[skinned shapes] → [TLAS build] → [shadow maps] → Forward (céu, opacos, transparentes; HDR + parte ambiente + movimento) → [SSAO] → [visualização traçada ou path tracer] → passes BeforeTaa → [TAA] → passes AfterOpaque → pós-processos (HDR) → passes BeforeTonemap → [bloom] → [exposição automática] → Tonemap → [FXAA] (saída) → passes AfterTonemap → [debug lines]`

Com base no que cada passe declara, o graph:
- **descarta os passes de que nada precisa:** fica só o que alimenta uma saída (uma textura importada com estado final, ou marcada com `markOutput`) ou tem efeito colateral (`sideEffect()`: buffers, estruturas de aceleração);
- **faz as transições:** attachments, `sample` (ShaderRead), `storage` (UnorderedAccess) e o estado final das importadas;
- **cria as texturas transitórias** (`create`) a partir de um pool mantido entre frames. Transitórias do mesmo tamanho e formato cujos tempos de vida não se sobrepõem dividem a mesma textura. Por isso uma cadeia de pós-processos usa só duas, e o atlas de sombras não existe quando não é usado.

O jogo adiciona passes em quatro pontos: `RenderStage::BeforeTaa` (depois do forward, do SSAO e das visualizações traçadas, antes do TAA), `AfterOpaque`, `BeforeTonemap` e `AfterTonemap`. O TAA média o que é desenhado em `BeforeTaa`, então ali um passe com ruído pode girar o padrão a cada frame; nos outros três estágios, use um padrão fixo.

```cpp
#include <nova/renderer/GpuTypes.h> // gpu::PostProcessPush

auto blur = renderer().shaders().load("assets/shaders/blur.frag"); // nova/postprocess.glsl
renderer().addRenderPasses(RenderStage::BeforeTonemap, [&, blur](RenderGraphFrame& f) {
    rhi::PipelineHandle p = renderer().postProcessPipeline(blur);
    if (!p) return;
    const RGTexture src = f.color, depth = f.depth;
    const RGTexture out = f.graph.create({f.extent.width, f.extent.height, Renderer::kHdrFormat, "blur"});
    const u64 frame = f.frameData;
    f.graph.addPass("Blur").sample(src).sample(depth).color(out, rhi::LoadOp::DontCare).execute([=](RenderPassContext& ctx) {
        ctx.cmd.bindPipeline(p);
        gpu::PostProcessPush push{frame, ctx.bindlessIndex(src)}; // o push de nova/postprocess.glsl
        push.depthTexture = ctx.bindlessIndex(depth);             // nova_depth (sampled: declared above)
        ctx.cmd.pushConstants(push);
        ctx.cmd.draw(3);
    });
    f.color = out; // os passes seguintes e o tonemap leem o resultado
});
```

**Culling na GPU** (`RenderSettings::gpuCulling`, ligado por padrão). Os draws opacos comuns (sem esqueleto e sem corte por triângulo na emenda) não são mais desenhados um a um:
- um compute shader (`cull.comp`, o passe "GPU culling") testa a esfera envolvente de cada instância contra o frustum;
- as visíveis são agrupadas por pipeline e malha, e cada grupo vira um `drawIndexedIndirect` só, com uma lista das instâncias visíveis lida pelo vertex shader (`gl_InstanceIndex`). Cem caixas iguais são um draw;
- os transparentes (ordenados por distância), as malhas com esqueleto e as cópias cortadas por triângulo continuam no caminho de um draw por instância;
- materiais de jogo não mudam: `nova_instance()` lê a instância que o vertex shader passa (`vInstance`);
- **occlusion culling** (`RenderSettings::occlusionCulling`, ligado por padrão) em duas fases, para que nada apareça atrasado:
  1. as esferas são testadas contra a pirâmide de profundidade (Hi-Z) do frame anterior de cada view, deslocadas pelo quanto a origem da câmera andou, e as que passam são desenhadas;
  2. a pirâmide é refeita com a profundidade desse primeiro desenho (`hiz.comp`: o mais distante de cada bloco, nível por nível, num buffer);
  3. só as rejeitadas na fase 1 são testadas de novo, e as visíveis são desenhadas num passe "Forward (late)", junto com os transparentes;
- o que a GPU escreve (comandos, listas de visíveis, estados, contadores) fica num buffer na memória dela, e os comandos iniciais são copiados do buffer de upload;
- as sombras não mudam: a TLAS e os shadow maps têm todos os objetos;
- precisa de `DeviceCaps::drawIndirectFirstInstance`; sem isso, tudo é desenhado sem culling;
- `RenderStats` traz os grupos e quantas instâncias a GPU testou e achou visíveis (lido de volta alguns frames depois, sem esperar);
- **depth prepass** (`RenderSettings::depthPrepass`, desligado por padrão): os grupos culled na GPU, exceto os recortados, desenham antes só a profundidade, com os mesmos comandos indiretos. O forward a carrega e testa `LessEqual` (`invariant gl_Position` garante a mesma profundidade), então os pixels escondidos são rejeitados antes de sombrear. A imagem é idêntica. Nas cenas do Sandbox, com pouca sobreposição, o ganho fica entre 0 e 7% a 1080p; compensa com sombreamento pesado e muita sobreposição. Um material de jogo opaco que use `discard` por conta própria não deve ligar isso. `Sandbox --prepass` liga a opção;
- `Sandbox --no-cull` desliga todo o culling e `--no-occlusion` só o de oclusão, para comparar; o cenário **Occlusion** põe um muro na frente de uma cidade de 900 blocos (das 1802 instâncias, 82 são desenhadas).

**Anti-aliasing** (`RenderSettings::antiAliasing`, FXAA por padrão): o tonemap escreve numa textura RGBA8 transitória e o FXAA (como o FXAA 3.11 de Lottes, qualidade de PC) suaviza as bordas ao escrever a saída. Ele acha as bordas pelo contraste da luma, procura as duas pontas de cada uma e mistura através dela, com um ajuste para detalhes de um pixel (`fxaaSubpixel`). As linhas de debug e os passes `AfterTonemap` vêm depois, nítidos. `AntiAliasing::None` desliga, e `Sandbox --no-aa` também.

**TAA** (`AntiAliasing::TAA`; o padrão continua FXAA): anti-aliasing temporal.
- A cada frame, a projeção se desloca uma fração de pixel (sequência Halton 2, 3 com 8 posições, `FrameData::jitter`). As linhas de debug descontam o deslocamento e não tremem.
- O passe forward ganha uma terceira saída, os **vetores de movimento** (`outMotion` em `nova/material.glsl`). Cada instância leva o transform do frame anterior (`InstanceData::prevModel`), que a view guarda por `DrawItem::objectKey` (a Scene usa o UUID da entidade), e a câmera leva a view-projection anterior sem jitter, já vista da origem atual. O céu se move só com a rotação da câmera. Nas malhas com esqueleto, os ossos também contam: a view guarda as matrizes deles por objeto, e o vertex shader refaz a pose do frame anterior (`DrawPush::prevJoints`).
- O resolve (`taa.frag`, logo depois do SSAO, em HDR) lê o histórico onde o pixel estava no frame anterior, com filtro Catmull-Rom. O movimento vem da superfície mais próxima na vizinhança 3 × 3, para que as bordas de objetos em movimento o acompanhem. O histórico é recortado pela faixa de cores da vizinhança atual (média ± desvio padrão em YCoCg), o que elimina rastros e o que reaparece atrás de objetos, e é misturado após um tonemap reversível. `taaFeedback` (0,1) é o peso de cada frame novo e cresce com a velocidade.
- O histórico fica em duas texturas por view, usadas em alternância, e recomeça quando a view muda de tamanho ou não é desenhada num frame.
- `DebugView::Motion` mostra os vetores de movimento. `Sandbox --taa` liga, e `--debug-view 5` mostra o movimento.
- Com TAA, o padrão de amostras do SSAO gira a cada frame, e o acúmulo dilui o ruído.

**Bloom** (`RenderSettings::bloom`, ligado por padrão), como no Call of Duty: Advanced Warfare (Jimenez):
- uma cadeia de imagens de metade do tamanho (`bloomLevels`, 6), cada uma com um filtro de 13 amostras da anterior; a primeira usa a média de Karis, para que pixels isolados muito brilhantes não pisquem;
- a volta para cima soma, em cada nível, um filtro tenda 3×3 do nível menor (`bloomRadius`);
- o resultado é misturado ao HDR no tonemap (`bloomStrength`, padrão 0,04). Sem limiar, tudo brilha um pouco, como numa lente real; `bloomThreshold` > 0 limita o efeito ao que passa desse brilho, com transição suave;
- todas as imagens são transitórias do render graph, e o painel Renderer do editor tem os controles. `Sandbox --no-bloom` desliga para comparar.

**SSAO**, a oclusão ambiente em espaço de tela (`RenderSettings::ssao`, ligada por padrão):
- o passe forward tem uma segunda saída, a parte da luz ambiente em cada pixel (`outAmbient` em `nova/material.glsl`; materiais do jogo a recebem sem mudar nada);
- `ssao.frag` calcula a oclusão só a partir da profundidade, porque o renderer forward não guarda normais: posições e normais são reconstruídas dela. São 16 amostras no hemisfério da normal, até `ssaoRadius` (0,5 m), giradas por pixel, em **meia resolução** (um quarto dos pixels: a 1080p, o SSAO custa cerca de 0,3 ms);
- outro passe desfoca a oclusão em 3 × 3 texels de meia resolução de profundidade parecida e escurece só a parte ambiente da imagem (`ssaoIntensity`). Sol e point lights continuam com as próprias sombras;
- `DebugView::AmbientOcclusion` mostra a oclusão. `Sandbox --no-ssao` desliga a opção, e `--debug-view 4` mostra a oclusão;
- sem SSAO, a segunda saída fica sem textura (`DeviceCaps::unusedAttachments`, `VK_EXT_dynamic_rendering_unused_attachments`). Nas GPUs sem essa extensão, ela recebe uma textura transitória que ninguém lê.

**Captura:** `Renderer::captureNextFrame("shot.png")` grava a saída da próxima renderização, depois do tonemap e com as linhas de debug, num PNG, assim que a GPU termina. `Sandbox --frames 120 --screenshot shot.png` usa isso.

**Exposição automática** (`RenderSettings::autoExposure`, desligada por padrão), a adaptação do olho:
- cada view mede a própria imagem HDR antes do tonemap (`exposure.comp`): um histograma de 256 faixas do log da luminância, de 2⁻¹² a 2¹², montado em memória compartilhada;
- a média sai dos pixels entre 40% e 95% do histograma, então nem as sombras nem alguns pontos muito brilhantes decidem sozinhos;
- a exposição vai aos poucos (`autoExposureSpeed`, 1,5/s) até a que leva essa média a `autoExposureKey` (0,18, cinza médio), dentro de `autoExposureMin`..`autoExposureMax`;
- o estado fica num buffer da view, e o tonemap o lê direto na GPU, sem esperar a CPU. `exposure` continua valendo, como compensação por cima;
- `RenderStats::autoExposure` traz o valor escolhido (lido de volta alguns frames depois), e o painel Renderer mostra os controles. `Sandbox --auto-exposure` liga a opção.

**Tempo de GPU por passe:** `CommandList::beginGpuTimer(nome)` / `endGpuTimer()` medem trechos de comandos com timestamps (aninháveis; `DeviceCaps::gpuTimers`), e `Device::gpuTimings()` devolve os do último frame concluído, lidos sem esperar a GPU. O render graph mede cada passe dentro de um `Render` por view, e a `Application` mede a interface (`UI`). O painel Renderer mostra a árvore (`GPU time`), e `Sandbox --frames` registra a média por frame de cada passe.

`RenderStats` traz os passes executados e descartados, as texturas transitórias e o tempo de CPU gasto montando e gravando o frame (`cpuMs`), e `Renderer::lastGraph()` descreve o último frame, um passe por linha. O painel **Renderer** do editor e o sandbox mostram as duas coisas. A compilação do graph não precisa de GPU e tem testes próprios (`tests/RenderGraphTests.cpp`).

## Ray tracing

Já implementado:
- detecção das capacidades (`DeviceCaps::rayQuery`, `rayTracingPipeline`) e ativação automática das extensões;
- uma **BLAS** por mesh, criada junto com a mesh e **compactada** (construída com `ALLOW_COMPACTION`, copiada para uma estrutura do tamanho compactado; nas malhas do Sandbox, 2,0 MB viram 0,7 MB). As BLAS das malhas com esqueleto continuam atualizáveis (refit);
- uma **TLAS** por frame em voo, refeita a cada frame a partir das instâncias da cena: com o mesmo número de instâncias do último build, ela é **atualizada** (refit, bem mais barato), com um build completo a cada 16 frames para a árvore não degradar com o movimento. Na cena Occlusion (1802 instâncias), o frame a 1080p caiu de 1,22 para 0,97 ms;
- **sombras por ray query** no fragment shader (sol e point lights), num pipeline-variante (`forward_rq.frag`, e a variante `rq` de cada material do jogo). Em GPUs sem RT, o pipeline raster normal é usado sem nenhuma mudança no código do jogo.
- **shadow maps em cascata** do sol quando não há ray tracing (ou com `RenderSettings::rayTracedShadows` desligado): 4 cascatas num atlas de profundidade (`shadowMapSize`, padrão 2048 por cascata) até `shadowDistance` (padrão 120 m), divisões práticas entre log e linear, cada cascata numa esfera estável (sem tremer ao girar a câmera) com o centro preso aos texels em coordenadas de mundo, PCF 3×3 e deslocamento pela normal contra acne. Cada cascata desenha só os objetos cuja esfera envolvente alcança o volume dela. Projetam sombra as malhas com esqueleto (mesmo vertex shader) e as recortadas respeitam o alfa (`shadow.frag`); as transparentes (`Blend`) não projetam. No fim de cada cascata (`shadowCascadeBlend`, padrão 15% do comprimento dela) a sombra se mistura com a seguinte, e a última some aos poucos até `shadowDistance`: não aparece a borda onde uma cascata termina. Desligue com `RenderSettings::shadowMaps = false`. `Sandbox --no-rt` força esse caminho.
- **sombras das point lights sem ray tracing:** shadow maps de cubo para as luzes mais próximas da câmera (`maxPointShadows`, padrão 4; `pointShadowMapSize`, padrão 512 por face). Cada luz ocupa uma linha de um atlas transitório, com 6 faces de 90°, e desenha só os objetos dentro do seu raio. O shader escolhe a face e a profundidade analiticamente, com PCF 3×3 e deslocamento pela normal. `PointLightComponent::castShadows` desliga a sombra de uma luz (também no ray tracing). O sandbox tem o cenário **Point lights** (três luzes coloridas girando entre pilares).

- **pipeline de ray tracing** (`DeviceCaps::rayTracingPipeline`):
  - `rhi::RayTracingPipelineDesc` tem um raygen, os misses (por índice) e os hit groups (`HitGroupDesc`: closest hit e/ou any hit). `Device::createRayTracingPipeline` monta junto a shader binding table, com cada região no alinhamento de base, e `CommandList::traceRays(w, h)` dispara o pipeline ligado;
  - `Device::storageIndex` dá o índice dos storage images bindless (set 0, binding 1);
  - as barreiras de estruturas de aceleração e de imagens incluem os shaders de RT;
  - `nova/raytracing.glsl` traz o push (`gpu::RtPush`), os payloads, os dois tipos de raio (primário e sombra: `sbtRecordOffset` = tipo, stride 2), a superfície atingida (`nova_hitSurface`) e o teste de alfa dos recortados;
  - a BRDF e o céu ficaram em `nova/brdf.glsl` e `nova/sky.glsl`, usados pela rasterização e pelos raios.
- **visualização traçada** (`DebugView::RayTraced`, ou "Ray traced (reference)" no painel Renderer; `Sandbox --debug-view 6`): a imagem inteira sai do pipeline de RT, como referência para a rasterizada.
  - Cada pixel dispara um raio primário. O closest hit lê o triângulo e o material (cor base e textura, xadrez, metálico/rugosidade, emissivo) e lança raios de sombra para o sol e para cada point light. O any hit deixa passar o que é transparente nos materiais recortados, e o miss mostra o céu.
  - A TLAS é construída também sem as sombras por ray query. As malhas com esqueleto aparecem na pose, porque a instância aponta para os vértices deformados.

- **path tracer de referência** (`DebugView::PathTraced`, ou "Path traced (reference)" no painel Renderer; `Sandbox --debug-view 7 --paused`): iluminação global sobre o mesmo pipeline de RT (`pt_reference.rgen`).
  - A cada frame, cada pixel ganha um caminho, que começa num ponto aleatório do pixel e quica até `pathTracerBounces` vezes (padrão 4).
  - Em cada ponto atingido, o sol e as point lights são amostrados diretamente com raios de sombra. A próxima direção sai da BRDF: lóbulo especular GGX ou difuso por cosseno, escolhidos pela refletância de Fresnel. A partir do 3º quique, a roleta russa corta caminhos fracos sem viés.
  - Quem escapa vê o céu. O disco do sol só conta no raio da câmera, porque nos quiques o sol já entrou pela amostragem direta. Como luz, o céu é escalado por `ambientIntensity`, como a luz ambiente da rasterização, para que as duas imagens sejam comparáveis. Superfícies emissivas iluminam o que o caminho encontra.
  - A média fica num RGBA32F por view e recomeça quando muda algo que ela vê: câmera, objetos, materiais, ossos, luzes, céu ou opções (uma chave FNV de tudo isso). Ela para em `pathTracerMaxSamples` (4096), e `RenderStats::pathSamples` traz a contagem.
  - Converge quando a cena está parada; com física rodando, recomeça a cada frame. O TAA não se aplica a essa visualização.

- **shaders de RT do jogo:** `.rgen`, `.rmiss`, `.rchit` e `.rahit` vêm da `ShaderLibrary`, compilados em runtime com recarga a quente, ou pré-compilados com `nova_add_game_shaders`. `Renderer::rayTracingPipeline(RayTracingShaders)` monta o pipeline e a shader binding table, guarda em cache e reconstrói quando algum shader recarrega. Os passes do jogo (`addRenderPasses`) recebem `RenderGraphFrame::tlas` e `instances`, e o push padrão (`gpu::RtPush`, em `nova/raytracing.glsl`) tem `inputs[4]` (índices bindless) e `params` livres para o jogo. `RenderSettings::rayTracingScene` constrói a TLAS mesmo quando a engine não traça nada. O `rt_alpha.rahit` e o `rt_shadow.rmiss` da engine podem ser reaproveitados como shaders pré-compilados.

```cpp
nova::RayTracingShaders ao;
ao.rayGen = renderer().shaders().load("rtao.rgen");
ao.miss = {renderer().shaders().load("rtao.rmiss")};
ao.hitGroups = {{{}, renderer().shaders().load("rt_alpha.rahit", {.source = nova::ShaderSource::Precompiled})}};
renderer().settings().rayTracingScene = true;
renderer().addRenderPasses(nova::RenderStage::BeforeTaa, [&](nova::RenderGraphFrame& f) {
    auto pipeline = renderer().rayTracingPipeline(ao);
    if (!pipeline || !f.tlas) return;
    // f.graph.addPass(...).sample(f.color).storage(out).execute(... ctx.cmd.traceRays(w, h) ...)
});
```

  O Sandbox tem um exemplo completo: AO traçado por raios com shaders próprios (`sandbox/shaders/rtao.*`), em `BeforeTaa` (com TAA, o ruído dele some), ligado com a tecla **O**, o checkbox "RT AO" ou `--rtao`.

- **reflexos e AO por ray query no passe forward** (na variante `rq` dos materiais, com as sombras traçadas ligadas):
  - `RenderSettings::rayTracedReflections` (ligado por padrão) lança um raio por pixel em torno da direção do espelho, espalhado pela rugosidade (um meio-vetor GGX; o TAA média o ruído).
  - No ponto atingido, a superfície é iluminada de forma simples: cor base e textura, xadrez, sol com raio de sombra, ambiente e emissivo, como difusa. Sem acerto, vem o céu. A luz volta pesada pelo Fresnel e entra no lugar da aproximação especular da luz ambiente, sumindo aos poucos até `reflectionMaxRoughness` (0,5).
  - `RenderSettings::rayTracedAo` (desligado por padrão) troca o SSAO por `rayTracedAoRays` raios curtos (2, até `rayTracedAoRadius`) no hemisfério. Com TAA, o padrão gira a cada frame.
  - Ficam no painel Renderer; no Sandbox, `--no-reflections` e `--rq-ao`. O cenário **Surfaces** tem a bola de metal com faixas de rugosidade, do espelho ao áspero.


## Roadmap geral

- **Mundo em ponto fixo, circular, com `WorldOrigin`** (feito): etapas 1–6 (ponto fixo `fpm`, sem `Integer.h`, Jolt direto e determinístico entre plataformas) e depois:
  - posição `int64` Q45.18 (mesma fração de antes, arquivos compatíveis);
  - mundo sempre circular, com um tamanho por eixo de 1 m a 2⁴⁰ m;
  - física em relação a um `WorldOrigin` em grade;
  - renderização na emenda escolhível com `NOVA_WRAP_RENDER_MODES` (por triângulo, por objeto ou recortada, e voltas repetidas).

  Detalhes em [docs/WORLD_SPACE.md](docs/WORLD_SPACE.md#posição-em-64-bits-mundo-sempre-circular-e-worldorigin-feito).
- **Mundo grande contínuo por blocos** (adiado: com posições `int64`, mundo circular e `WorldOrigin`, não foi preciso): blocos de 4096 m, dormência e carregamento com histerese (carrega no 5×5, descarrega fora do 7×7) e eventos de bloco para o jogo.
- Modelo anterior (substituído): **Mundos grandes** com posições globais inteiras (ponto fixo em `int64`, escala ajustável, bordas com volta ou barreira). O histórico está em [docs/WORLD_SPACE.md](docs/WORLD_SPACE.md).
- **Renderização circular**: feita como opção de compilação (`NOVA_WRAP_RENDER_MODES`, `RenderSettings::wrapShift` / `wrapRepeat`), conferida em GPU. Falta o picking ciente das cópias.
- Shaders do jogo, próximos passos: vertex shaders de material (deslocamento de vértices), materiais transparentes/blend, profundidade no pós-processamento, passes compute dentro do frame do renderer. Os shaders de RT do jogo já existem (`Renderer::rayTracingPipeline`).
- Editor: receitas para IA / servidor MCP (fase 5).
- Renderer: materiais completos (feitos: mapas de normal, metálico/rugosidade, oclusão e emissivo, recorte e transparência por alfa, dois lados). Shadow maps em cascata como fallback sem RT, render graph, culling na GPU por frustum e oclusão (Hi-Z em duas fases), sombras de point lights, bloom, mistura entre cascatas, exposição automática, SSAO, FXAA e TAA (feitos). Ray tracing: sombras, reflexos e AO por ray query, pipeline de RT, visualização traçada, path tracer e shaders de RT do jogo (feitos). Backend D3D12: depende de trocar os endereços de GPU nos shaders (veja o modelo de binding).
- Cena: hierarquia de transforms (feita: `ParentComponent`, `Entity::setParent`, árvore na *Hierarchy*; modelos glTF instanciados com a árvore dos nós, sob uma raiz com o nome do arquivo).
- Animação: dos nós, skinning (com sombras e picking na pose atual), troca e mistura de clipes, máscaras por osso, eventos e escolha do clipe pelo nome no inspetor (feitos).
