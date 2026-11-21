# Espaço do mundo: posições globais em inteiros

Objetivo: mundos grandes sem perda de precisão, com escala ajustável e bordas que dão a volta ou
são barreiras.

## Modelo

- **`GlobalPosition`**: um ponto no mundo. Três `int64` em ponto fixo; a unidade (metros por passo
  inteiro) é do mundo, padrão `2^-32 m` (32.32: ~0,2 nm de resolução).
- **`GlobalOffset`**: deslocamento entre pontos, também `int64` na mesma unidade. Toda relação entre
  pontos é inteira e exata: `b - a`, `a + d`, `d1 + d2`, `d * n`, comparação de distâncias
  (quadrados em 128 bits). `a + (b - a) == b` sempre, bit a bit.
- **`WorldSpace`**: configuração do mundo — unidade e, por eixo, tamanho e borda:
  - **Barrier**: posições saturam nos limites (até 2^63 unidades por eixo, para toda diferença
    caber num `int64`);
  - **Wrap**: o eixo dá a volta (até 2^64 unidades; com o tamanho máximo a volta é o próprio overflow
    do inteiro). A diferença entre dois pontos é sempre o caminho mais curto, atravessando a emenda.
- **Metros só nas bordas**: `fromMeters(double)` / `toMeters() → double` para inicializar e exibir;
  deslocamentos convertem de/para `vec3` (`float`) para geometria local, física e GPU. Operações não
  exatas por natureza (`length`, `lerp`, rotação, escala) são feitas em `double` e voltam arredondadas
  para a grade, sem acumular erro.
- **Unidades métricas** (`nova/math/Units.h`): `LengthUnit` de nanômetro a megâmetro, `convertLength`,
  `readableUnit` (escolhe a unidade para exibir). A escala do mundo e o tamanho dos eixos podem ser
  dados em qualquer unidade (`setUnit(1, LengthUnit::Millimeter)`,
  `setAxis(0, 50, LengthUnit::Kilometer, WorldBoundary::Wrap)`), e posições/deslocamentos convertem
  direto (`fromUnits`, `toUnits`, `length(unit)`, `distance(a, b, unit)`), com uma única
  multiplicação a partir dos passos inteiros.
- Vértices, offsets de colliders, rotação, escala e velocidades continuam `float`: são locais e pequenos.

```cpp
nova::WorldSpace::setActive(nova::WorldSpace(nova::WorldSpaceDesc{}
    .setUnit(1, nova::LengthUnit::Millimeter)                                   // passo de 1 mm
    .setAxis(0, 50, nova::LengthUnit::Kilometer, nova::WorldBoundary::Wrap)     // X dá a volta em 50 km
    .setAxis(1, 4, nova::LengthUnit::Kilometer, nova::WorldBoundary::Barrier)   // Y: barreira de 4 km
    .setAxis(2, 50, nova::LengthUnit::Kilometer, nova::WorldBoundary::Wrap)));

auto base = nova::GlobalPosition::fromUnits({12.5, 0, -3}, nova::LengthUnit::Kilometer);
base += velocity * dt;                                       // float em metros → passo inteiro exato
nova::GlobalOffset d = enemy - base;                         // exato, pelo caminho mais curto
double km = d.length(nova::LengthUnit::Kilometer);
```

**Tamanho máximo por dimensão.** Para dizer só "quero no máximo 10 km por dimensão" e deixar a
engine escolher a escala, use `WorldSpaceDesc::withMaxSize(10, LengthUnit::Kilometer)`: todos os
eixos ficam com 10 km e a unidade vira a mais fina que cabe (10 km / 2^63 ≈ 1,1 fm com barreira;
com `WorldBoundary::Wrap`, 10 km / 2^64). Com tamanhos diferentes por eixo, configure-os com
`setAxis` e chame `fitUnit()`: a unidade passa a caber no maior eixo.

```cpp
scene.setWorldSpace(nova::WorldSpaceDesc::withMaxSize(10, nova::LengthUnit::Kilometer));
```

## Na cena (fase 2)

- `TransformComponent::position` é um `GlobalPosition`. Atribuir metros continua natural
  (`t.position = {0, 3, 10}`), mover usa deslocamentos (`t.position += velocity * dt`), e
  `t.position - other.position` é um `GlobalOffset` exato. Para mexer num eixo só, guarde uma base e
  some um deslocamento (`position = base + glm::vec3(0, y, 0)`).
- `Scene::setWorldSpace(desc)` define o mundo da cena (as posições existentes são convertidas em
  metros); `Scene::update` reativa o mundo da cena a cada frame.
- Arquivos de cena gravam `"world": {"unit", "axes": [{"size", "boundary"}]}`. Posições são gravadas
  como `[x, y, z]` em metros quando a conversão é exata (o caso comum, e o formato antigo — cenas
  antigas carregam sem mudança); senão como `{"raw": [inteiros], "meters": [...]}`, exato.
- Prefabs guardam deslocamentos exatos a partir da primeira entidade e são instanciados em qualquer
  `GlobalPosition`.
- `TransformComponent::matrixRelativeTo(origem)` dá a matriz com a translação medida a partir de um
  ponto próximo (a câmera, na fase 3). `matrix()` ainda é absoluta em `float`.
- Inspetor: posição em metros com `double` (o tooltip mostra a unidade legível e os passos inteiros);
  editar um eixo move só ele, os outros continuam bit a bit.
- A física usa uma origem flutuante (fase 4).

## Renderização relativa à câmera (fase 3)

- `CameraData` tem uma `origin` (`GlobalPosition`); `CameraData::relative(eye, orientação, ...)` e
  `relativeLookAt(eye, direção, ...)` fazem do olho a origem. Tudo na `RenderScene` (transforms dos
  draws, luzes, TLAS) é medido a partir dela, então a GPU só vê `float` pequenos — testado com a
  cena inteira a 10.000 km da origem, onde o `float` absoluto tem precisão de 1 m.
- `Scene::buildRenderScene` monta tudo relativo à câmera (`TransformComponent::matrixRelativeTo`).
- Shaders: posições são relativas à câmera (`s.relativePos`, exato, é o que a iluminação usa);
  `s.worldPos` é a posição de mundo **módulo 4096 m** (`NOVA_WORLD_TILE`), contínua para padrões
  cujo período divide 4096 m (o checker, ruído em potências de 2) e precisa em qualquer lugar.
  `FrameData::originTiled` leva a origem já reduzida (em inteiro, quando a unidade permite).
- `DebugDraw` recebe `GlobalPosition` (metros em `vec3` convertem implicitamente) e guarda as linhas
  relativas a uma origem própria (a câmera do frame anterior); o renderer corrige a diferença para a
  câmera atual com um único deslocamento por render.
- Editor: a câmera é uma `GlobalPosition`; o raio do mouse (`ViewportState::rayOrigin`), o picking
  (`PickHit::point`), o ponto de spawn e o soltar de assets são exatos; o gizmo trabalha em espaço
  da câmera e só regrava a posição quando ela de fato mudou.

## Física com origem flutuante (fase 4)

- O Jolt continua em `float`, mas em torno de `Scene::physicsOrigin()`, uma `GlobalPosition`:
  corpos entram como `posição − origem` e voltam como `origem + p`.
- A origem segue um foco: `Scene::setPhysicsFocus(p)` (ex.: o jogador) ou, sem foco, a câmera
  principal. Quando o foco passa de `physicsRebaseDistance` (1024 m) da origem, todos os corpos são
  deslocados de uma vez (`PhysicsWorld::shiftOrigin`), mantendo velocidades e sono. O deslocamento é
  um valor que o `float` representa exatamente, então origem inteira e Jolt nunca divergem.
- A precisão é a do `float` em torno do foco: ~0,06 mm a 1 km, ~0,5 mm a 5 km. Corpos longe do foco
  (ex.: outro jogador a dezenas de km) ficam menos precisos — várias regiões de física são a fase 6.
- Consultas (`raycast`, `sweep*`) recebem `GlobalPosition` (metros convertem implicitamente) e
  `SceneHit::point` / `sweptPoint` e `Collision::point` / `otherPoint` são `GlobalPosition`.
- **Barreiras:** bordas de eixos `Barrier` a menos de `barrierWallRange` (8 km) da origem viram
  paredes estáticas no Jolt (sem restituição própria), recriadas a cada rebase. Encostar nelas chama
  `Script::onWorldBoundary(WorldBoundaryHit{axis, side, point, normal})`; raycasts acertam a parede
  com `entity` inválida. Movimentos sem física (scripts, kinematic) saturam na borda pela própria
  aritmética de `GlobalPosition`.
- **Volta (Wrap):** como as posições relativas usam o caminho mais curto, corpos dos dois lados da
  emenda são vizinhos para o Jolt quando o foco está perto dela, e um corpo que atravessa a emenda
  reaparece do outro lado.
- Testado com a cena física inteira a 10.000 km da origem (pilha de caixas estável, consultas
  corretas), onde o Jolt em coordenadas absolutas teria precisão de 1 m.

## Editor (fase 5)

- Painel **World**: passo (unidade) e, por eixo, tamanho e borda, cada valor na unidade métrica que
  for mais cômoda; "Finest step for these sizes" (`fitUnit`) e "Every axis at most this"
  (o `withMaxSize`); prévia do resultado já com os limites aplicados; Apply / Revert. Mostra também a
  posição da câmera e, durante o Play, a origem da física. Fica bloqueado durante o Play.
- `EditorContext::setWorldSpace(desc)` aplica como **um passo de undo**: entidades e a câmera do editor
  mantêm o lugar em metros; o undo volta as configurações e cada posição bit a bit.
- *View/Show World Bounds* (ligado por padrão): bordas `Barrier` em vermelho e emendas `Wrap` em
  ciano, como uma grade ancorada no mundo, quando estão a menos de 300 m da câmera.
- A câmera do editor atravessa a emenda sem pulo, e renderização, picking e gizmo veem o mundo
  contínuo através dela (coberto pelos testes do editor).
- A posição da câmera aparece no canto da viewport, na unidade mais legível.

## Tempo exato

O tempo segue o mesmo modelo das posições (`nova/math/Time.h`):

- **Tick = flick** (1/705.600.000 s ≈ 1,42 ns): divide exatamente todas as taxas de quadro (24–240 Hz)
  e de áudio (8–192 kHz). Um `int64` de flicks cobre ±414 anos.
- **`TimePoint`** (instante) e **`Duration`** (intervalo): `b - a` é uma `Duration` exata,
  `a + d` um instante; construtores exatos `milliseconds/seconds/minutes/hours/days(n)` e
  `perSecond(hz)` (`perSecond(60)` = 11.760.000 flicks); `fromSeconds`/`seconds()` em `double`,
  `secondsF()` em `float`; interop com `std::chrono` (`fromChrono`, `toChrono`, tipo `Flicks`).
- **Unidades de tempo** em `Units.h`: `TimeUnit` (ns até dia), `convertTime`, `toSeconds`,
  `fromSeconds`, `readableTimeUnit`, `unitSymbol`.
- **Na engine:** o relógio da cena (`Scene::time()`) é um `TimePoint`; o passo da física
  (`Scene::fixedTimeStep`) é uma `Duration` e o acumulador soma flicks inteiros (60 passos = 1 s
  exato, sem deriva); `Application::frameDuration()` mede o frame direto do relógio do sistema; o
  `dt` dos scripts continua `float` em segundos. Shaders recebem `nova_time()` dentro de um ciclo de
  1 hora (`Scene::kShaderTimeCycle`, módulo inteiro), então o `float` nunca perde precisão.

Vetores e quaternions continuam sendo os do glm (genérico por tipo em tempo de compilação:
`glm::vec3`, `glm::dvec3`, `glm::i64vec3`...). `GlobalPosition::raw()` e `GlobalOffset::raw()`
devolvem os passos como `glm::i64vec3`.

Status: **Fases 1 a 5 concluídas** (a 6 — várias regiões de física e determinismo — fica para depois) (`engine/src/nova/math/`, testes em `tests/WorldSpaceTests.cpp`).

## Fases

1. **Tipos e aritmética** — `WorldSpace`, `GlobalPosition`, `GlobalOffset`, `UInt128`, conversões,
   helpers (`lerp`, `moveTowards`, `rotateAround`), testes headless. Nada do resto da engine muda.
2. **Cena** — `TransformComponent::position` vira `GlobalPosition`; a cena guarda o `WorldSpace`
   (serializado); reflexão/serialização do novo tipo (JSON exato em inteiros + metros legíveis);
   inspetor e gizmo em metros; migração de cenas antigas (`vec3`) e de todo o código existente.
3. **Renderização relativa à câmera** — `buildRenderScene` converte cada transform para
   `objeto − câmera` em `float`; a câmera fica na origem para a GPU; TLAS idem; DebugDraw e picking
   recebem posições globais.
4. **Física com origem flutuante** — o Jolt continua em `float`, numa origem local inteira que segue um
   foco (câmera/jogador) e é rebaseada ao se afastar; consultas e eventos de contato usam
   `GlobalPosition`; planos estáticos de colisão nas barreiras próximas; evento
   `Script::onWorldBoundary`.
5. **Editor** — painel de configurações do mundo, desenho das barreiras e emendas, câmera do editor
   atravessando a emenda sem pulo.
6. **Depois** — várias regiões de física (jogadores distantes em multiplayer), determinismo.
