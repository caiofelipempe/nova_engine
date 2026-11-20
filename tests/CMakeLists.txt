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

add_executable(NovaInputTests InputTests.cpp)
target_link_libraries(NovaInputTests PRIVATE Nova::Engine)
set_target_properties(NovaInputTests PROPERTIES FOLDER "Tests")
add_test(NAME input COMMAND NovaInputTests)

if(TARGET Nova::Editor)
    add_executable(NovaEditorTests EditorTests.cpp)
    target_link_libraries(NovaEditorTests PRIVATE Nova::Editor)
    set_target_properties(NovaEditorTests PROPERTIES FOLDER "Tests")
    add_test(NAME editor COMMAND NovaEditorTests)
endif()

# A failing assert opens a dialog on Windows debug builds: never let it block the suite.
set_tests_properties(physics input scene assets PROPERTIES TIMEOUT 60)
if(TARGET NovaEditorTests)
    set_tests_properties(editor PROPERTIES TIMEOUT 60)
endif()
