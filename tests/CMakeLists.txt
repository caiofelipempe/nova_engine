add_executable(NovaTests PhysicsTests.cpp)
target_link_libraries(NovaTests PRIVATE Nova::Engine)
set_target_properties(NovaTests PROPERTIES FOLDER "Tests")
add_test(NAME physics COMMAND NovaTests)
