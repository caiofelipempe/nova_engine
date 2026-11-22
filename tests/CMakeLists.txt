add_executable(NovaTests PhysicsTests.cpp)
target_link_libraries(NovaTests PRIVATE Nova::Engine)
set_target_properties(NovaTests PROPERTIES FOLDER "Tests")
add_test(NAME physics COMMAND NovaTests)

add_executable(NovaSceneTests SceneTests.cpp)
target_link_libraries(NovaSceneTests PRIVATE Nova::Engine)
set_target_properties(NovaSceneTests PROPERTIES FOLDER "Tests")
add_test(NAME scene COMMAND NovaSceneTests)

add_executable(NovaAssetTests AssetTests.cpp)
target_link_libraries(NovaAssetTests PRIVATE Nova::Engine)
set_target_properties(NovaAssetTests PROPERTIES FOLDER "Tests")
add_test(NAME assets COMMAND NovaAssetTests)

add_executable(NovaShaderTests ShaderTests.cpp)
target_link_libraries(NovaShaderTests PRIVATE Nova::Engine)
nova_copy_shaders(NovaShaderTests) # engine GLSL headers in <exe>/shaders/include
set_target_properties(NovaShaderTests PROPERTIES FOLDER "Tests")
add_test(NAME shaders COMMAND NovaShaderTests)

add_executable(NovaWorldSpaceTests WorldSpaceTests.cpp)
target_link_libraries(NovaWorldSpaceTests PRIVATE Nova::Engine)
set_target_properties(NovaWorldSpaceTests PROPERTIES FOLDER "Tests")
add_test(NAME worldspace COMMAND NovaWorldSpaceTests)

add_executable(NovaTimeTests TimeTests.cpp)
target_link_libraries(NovaTimeTests PRIVATE Nova::Engine)
set_target_properties(NovaTimeTests PROPERTIES FOLDER "Tests")
add_test(NAME time COMMAND NovaTimeTests)

# The small-world scenarios of the sample game (same code as its Game/Scenarios commands).
add_executable(NovaScenarioTests ScenarioTests.cpp ${PROJECT_SOURCE_DIR}/editor/game/WorldScenarios.cpp)
target_link_libraries(NovaScenarioTests PRIVATE Nova::Engine)
set_target_properties(NovaScenarioTests PROPERTIES FOLDER "Tests")
add_test(NAME scenarios COMMAND NovaScenarioTests)

add_executable(NovaInputTests InputTests.cpp)
target_link_libraries(NovaInputTests PRIVATE Nova::Engine)
set_target_properties(NovaInputTests PROPERTIES FOLDER "Tests")
add_test(NAME input COMMAND NovaInputTests)

if(TARGET Nova::Editor)
    add_executable(NovaEditorTests EditorTests.cpp)
    target_link_libraries(NovaEditorTests PRIVATE Nova::Editor)
    set_target_properties(NovaEditorTests PROPERTIES FOLDER "Tests")
    add_test(NAME editor COMMAND NovaEditorTests)
    if(NOVA_SHARED)
        # Two builds of the same game module, swapped on disk by the hot reload test.
        foreach(version 1 2)
            nova_add_game_module(NovaTestGameModuleV${version} TestGameModule.cpp)
            target_compile_definitions(NovaTestGameModuleV${version} PRIVATE TEST_MODULE_VERSION=${version})
            set_target_properties(NovaTestGameModuleV${version} PROPERTIES FOLDER "Tests")
            add_dependencies(NovaEditorTests NovaTestGameModuleV${version})
        endforeach()
        target_compile_definitions(NovaEditorTests PRIVATE
            NOVA_TEST_MODULE_V1="$<TARGET_FILE:NovaTestGameModuleV1>"
            NOVA_TEST_MODULE_V2="$<TARGET_FILE:NovaTestGameModuleV2>")
    endif()
endif()

# A failing assert opens a dialog on Windows debug builds: never let it block the suite.
set_tests_properties(physics input scene assets shaders worldspace time scenarios PROPERTIES TIMEOUT 60)
if(TARGET NovaEditorTests)
    set_tests_properties(editor PROPERTIES TIMEOUT 60)
endif()
