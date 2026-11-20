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
ctest --preset debug              # testes headless (física)
```

As dependências (manifest [vcpkg.json](vcpkg.json)) são instaladas automaticamente no configure: GLFW, Vulkan Memory Allocator, glm, EnTT, spdlog, Dear ImGui (docking), ImGuizmo, cgltf, Jolt Physics.
Em Debug, as validation layers do Vulkan ficam ligadas e as mensagens vão para o console e para `nova.log`.

## Arquitetura

```
engine/src/nova/
  core/       Application (loop), Layer, Log (spdlog)
  platform/   Window (interface) + Input        → glfw/  (backend GLFW)
  rhi/        Device, CommandList (interfaces)  → vulkan/ (backend Vulkan)
  renderer/   Renderer, RenderView, Mesh — só fala com o RHI
  scene/      Scene (EnTT), Entity, Components, Script, GameLayer
  physics/    PhysicsWorld (interface)          → jolt/ (backend Jolt)
  assets/     glTF (cgltf)
  ui/         tema/fontes do ImGui
engine/shaders/  GLSL → SPIR-V em build-time (glslc)
editor/          editor com docking, hierarquia, inspetor, gizmos, console
sandbox/         jogo mínimo usando só a API pública
```

**Backends.** `platform::Window` e `rhi::Device` são interfaces criadas por fábricas (`Window::create(WindowBackend::Glfw)`, `Device::create(Backend::Vulkan)`). Cada backend vive na sua pasta e é ligado por opções do CMake (`NOVA_RHI_VULKAN`, `NOVA_PLATFORM_GLFW`). A janela não depende de nenhuma API gráfica (a ponte WSI do Vulkan é type-erased), então dá para adicionar SDL3 ou D3D12 sem mexer nas camadas de cima.

**Modelo de binding (bindless).** Todos os backends seguem o mesmo contrato:
- buffers são acessados no shader por *device address* (64 bits, via push constants) — vértices são lidos por "vertex pulling";
- texturas ficam num array global de descritores (`Device::bindlessIndex`);
- acceleration structures também são referenciadas por endereço.

Isso mapeia direto para Vulkan (BDA + descriptor indexing) e para D3D12 (SM 6.6 `ResourceDescriptorHeap`), e é o mesmo modelo de que o ray tracing precisa (o hit shader tem de enxergar a geometria de qualquer objeto).

**Recursos.** São handles geracionais (`BufferHandle`, `TextureHandle`, …): handles obsoletos são detectados em vez de apontarem para outro recurso. A destruição é adiada até a GPU terminar os frames em voo. O estado das texturas é rastreado e as barreiras (sync2) são emitidas automaticamente.

**Frame.** `[TLAS build] → sky → forward PBR (HDR, RGBA16F) → tonemap (ACES) → UI`. Os dados por frame vêm de um alocador linear num buffer de upload por frame em voo.

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

- Corpos **Static**, **Kinematic** (seguem o transform do ECS e empurram os dinâmicos) e **Dynamic**; colliders Box, Sphere e Capsule com offset; tamanhos multiplicados pelo scale.
- Passo fixo de 60 Hz com acumulador; transforms dos dinâmicos são escritos de volta no ECS.
- `GameLayer` liga a simulação sozinho. No editor: **Play / Pause / Stop** (Stop restaura os transforms), gizmo arrasta corpos durante o Play.
- Próximos passos: mesh/convex colliders (para cenas glTF), character controller, joints, debug draw dos colliders, interpolação de transforms.

## Ray tracing

Já implementado:
- detecção das capacidades (`DeviceCaps::rayQuery`, `rayTracingPipeline`) e ativação automática das extensões;
- uma **BLAS** por mesh, criada junto com a mesh;
- uma **TLAS** por frame em voo, reconstruída a cada frame a partir das instâncias da cena;
- **sombras por ray query** no fragment shader (sol e point lights), num pipeline-variante (`forward_rq.frag`). Em GPUs sem RT, o pipeline raster normal é usado sem nenhuma mudança no código do jogo.

Próximos passos (a estrutura já está preparada):
1. `rhi::RayTracingPipelineDesc` + `CommandList::traceRays` + shader binding table — os entry points (`vkCreateRayTracingPipelinesKHR`, `vkCmdTraceRaysKHR`) já são carregados quando há suporte.
2. Um path tracer de referência (raygen escrevendo num storage image bindless — o binding 1 já existe).
3. Reflexos e AO por ray query no mesmo pass forward.
4. Compactação de BLAS e refit da TLAS para cenas grandes.

## Roadmap geral

Texturas em materiais (o slot bindless já está no `InstanceData`), hierarquia de transforms, serialização de cena, shadow maps como fallback sem RT, render graph, culling em GPU, e um backend D3D12.
