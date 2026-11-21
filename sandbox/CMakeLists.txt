nova_add_app(Sandbox src/main.cpp)
set_target_properties(Sandbox PROPERTIES FOLDER "Apps")

# The sandbox ships its game shaders precompiled (SPIR-V built by glslc, no compiler needed at
# runtime). The editor compiles the same files at runtime instead, with hot reload.
nova_add_game_shaders(Sandbox
    MATERIALS ${PROJECT_SOURCE_DIR}/assets/shaders/hologram.frag
    SHADERS   ${PROJECT_SOURCE_DIR}/assets/shaders/vignette.frag)
