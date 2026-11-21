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

Status: **Fase 1 concluída** (`engine/src/nova/math/`, testes em `tests/WorldSpaceTests.cpp`).

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
