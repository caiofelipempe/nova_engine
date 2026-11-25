nova_add_app(Sandbox src/main.cpp src/Scenarios.cpp src/Shapes.cpp)
set_target_properties(Sandbox PROPERTIES FOLDER "Apps")

# The game's own ray tracing shaders (the RT AO pass): compiled to <exe>/shaders.
nova_add_game_shaders(Sandbox SHADERS shaders/rtao.rgen shaders/rtao.rmiss
                                       ../assets/shaders/fog.frag    # a post effect reading depth (--fog)
                                       ../assets/shaders/wave.vert)  # a material vertex shader (--wave)
