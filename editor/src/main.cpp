#include "EditorLayer.h"

int main() {
    return nova::run<EditorLayer>({.name = "Nova Editor", .width = 1600, .height = 900, .maximized = true});
}
