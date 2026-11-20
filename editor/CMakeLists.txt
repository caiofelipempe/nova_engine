find_package(imguizmo CONFIG REQUIRED)

nova_add_app(NovaEditor
    src/main.cpp
    src/EditorLayer.cpp
    src/EditorLayer.h
    src/EditorCamera.h)
target_link_libraries(NovaEditor PRIVATE imguizmo::imguizmo)
set_target_properties(NovaEditor PROPERTIES FOLDER "Apps")
