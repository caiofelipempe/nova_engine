#pragma once

// Example of game-specific editor tooling. Everything here uses only the public editor API, the
// same way a game would extend its own editor: a custom component with an inspector, a panel and
// a command with a shortcut.
#include <nova/editor/Editor.h>

// A game component: marks an entity as a target worth points.
struct ScoreTarget {
    int points = 10;
    bool active = true;
};

class ExamplePlugin final : public nova::editor::Plugin {
public:
    void onRegister(nova::editor::Registry& registry) override;
};
