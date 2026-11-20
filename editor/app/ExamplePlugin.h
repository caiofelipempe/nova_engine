#pragma once

// Example of game-specific editor tooling. Everything here uses only the public engine/editor API,
// the same way a game would extend its own editor:
//  - a game component and a game script described with reflection (inspector + save/load for free),
//  - a panel that reacts to editor signals,
//  - a command with a shortcut.
#include <nova/editor/Editor.h>

// A game component: marks an entity as a target worth points.
struct ScoreTarget {
    int points = 10;
    bool active = true;
};

// A game script: bobs the entity up and down while the game runs.
struct Bob : nova::Script {
    float amplitude = 0.3f;
    float speed = 2.0f;

    void onCreate() override { m_baseY = transform().position.y; }
    void onUpdate(float dt) override {
        m_time += dt;
        transform().position.y = m_baseY + amplitude * std::sin(m_time * speed);
    }

private:
    float m_baseY = 0.0f;
    float m_time = 0.0f;
};

// Reflection for the game types above (call once at startup).
void reflectGameTypes();

class ExamplePlugin final : public nova::editor::Plugin {
public:
    void onRegister(nova::editor::Registry& registry) override;
};
