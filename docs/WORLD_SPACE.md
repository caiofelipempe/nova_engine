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

## Regiões de física e determinismo (fase 6)

- **Focos:** `scene.setPhysicsFoci({p1, p2, ...})` (ex.: um por jogador); sem focos, a câmera principal.
- **Regiões:** focos a menos de `2 × physicsRegionRadius` (padrão 4 km) formam uma região — um mundo
  Jolt próprio com origem flutuante no **centróide** dos seus focos. Focos distantes viram regiões
  separadas, cada uma precisa em volta dos seus jogadores; quando eles se aproximam, as regiões se
  fundem (e se separam de novo ao se afastarem).
- **Corpos:** um corpo dinâmico/kinematic pertence à região do foco mais próximo, se estiver a menos
  de `physicsRegionRadius` (+ o tamanho do collider); fora disso fica **dormente** — congelado, sem
  corpo no Jolt, com a velocidade guardada (`setLinearVelocity`/`addImpulse` num corpo dormente
  atualizam essa velocidade) — e volta a simular, com a mesma velocidade, quando um foco se aproxima.
  Corpos estáticos entram em todas as regiões que alcançam. `scene.isPhysicsDormant(e)`.
- `physicsRegionRadius = 0` → alcance ilimitado: uma região só com tudo (o comportamento da fase 4).
- Consultas vão para a região em volta da origem da consulta; `physicsRegionCount()`,
  `physicsOrigin(i)`, `physicsBodyCount()`. O painel World mostra as regiões e edita a distância de
  simulação; o painel Renderer conta corpos e regiões.
- **Determinismo:** mesmo build + mesma cena + mesmas entradas → mesmo resultado, bit a bit,
  passo a passo (inclusive com rebases e troca de região: tudo é decidido em ordem determinística, a
  aritmética de posição/tempo é inteira e o Jolt é determinístico mesmo com várias threads).
  `scene.stateHash()` resume posições exatas, rotações, escalas e velocidades em ordem de UUID — para
  detectar dessincronização em lockstep ou validar replays.
- **Entre plataformas/compiladores** o Jolt precisa ser compilado com `CROSS_PLATFORM_DETERMINISTIC=ON`
  (o port do vcpkg fixa `OFF`; um overlay port resolve, com algum custo de desempenho). A parte da
  engine já é inteira/IEEE estrita; não há `fast-math`.

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

Status: **Fases 1 a 6 concluídas.** (`engine/src/nova/math/`, testes em `tests/WorldSpaceTests.cpp`).

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
6. **Regiões de física e determinismo** — vários focos (jogadores distantes), cada grupo com seu
   mundo físico; corpos dormentes fora do alcance; hash de estado para lockstep/replay.
7. **Futuro: backend de física com posições inteiras nativas** — para um motor de física próprio
   em ponto fixo:
   - capacidade declarada pelo backend (ex.: `caps().globalPositions`);
   - variante inteira do contrato do `PhysicsWorld`: criação de corpos, transforms, consultas e
     eventos de contato em `GlobalPosition`/`GlobalOffset`, e `step(Duration)` em vez de `f32`;
   - na cena, com um backend inteiro: uma região só, origem zero, sem rebase e sem `float` no
     caminho (determinismo entre plataformas); com o Jolt, o caminho atual continua igual;
   - um backend esqueleto que passe nos testes de interface, para o motor próprio nascer encaixado.
   Do lado do motor: matemática de ponto fixo para rotações/quaternions, raiz e trigonometria
   determinísticas, colisão (GJK/EPA ou formas analíticas) e solver — velocidades e rotações
   podem ser ponto fixo de 32 bits; posições usam os 64 bits de `GlobalPosition`.

   **Diretriz de desenho: corpo global, forma local.**
   - Posição do corpo em `GlobalPosition` (`int64`); forma e vértices (poliedros, malhas de colisão,
     offsets) em coordenadas **locais ao corpo** de 32 bits — `float`, ou ponto fixo 16.16
     (±32 km com 15 µm de precisão) para um motor inteiro determinístico. A unidade do mundo
     (2⁻³² m) não serve para o local: em 32 bits ela cobriria só ±0,5 m.
   - Fase ampla em inteiro (AABBs em `int64` ou por célula); para cada par próximo,
     `posB − posA` exato em `int64` vira um deslocamento pequeno, e a colisão estreita (GJK/EPA,
     SAT) e o solver rodam num **referencial local ao par**, ancorado num dos corpos — o mesmo
     princípio da renderização relativa à câmera.
   - Corpos gigantes (terrenos de km) são fatiados em pedaços, cada um com origem global própria e
     vértices locais pequenos.
   - `int64` em tudo não ajuda: a precisão necessária é relativa ao tamanho do corpo, e vértices em
     `int64` dobram memória e banda e tornam a rotação (produtos de 128 bits, metade da largura
     SIMD) várias vezes mais lenta.

   **Estado atual da estrutura em relação a essa diretriz:**
   - Já segue: posição da entidade é `GlobalPosition`; colliders (`halfExtents`, `radius`,
     `offset`) são locais em `float`; vértices de malha (renderização) são locais ao objeto.
   - Ainda não segue: o contrato do `PhysicsWorld` recebe a posição do corpo em `float` relativo à
     origem da região (não em `GlobalPosition`), então a fase ampla e a colisão do Jolt rodam em
     `float` no espaço da região (preciso até alguns km, mas não exato nem determinístico entre
     plataformas); não há formas poliédricas/malha (`ShapeType` só tem Box, Sphere e Capsule);
     não há fatiamento de corpos grandes nem tipo local em ponto fixo.
   - O que fecha a lacuna: o contrato inteiro desta fase (corpo em `GlobalPosition`, forma local
     de 32 bits), as formas convexas/malha com vértices locais, e o motor próprio fazendo a colisão
     por par em referencial local.
