add_executable(openkey_adapter_tests
    test_openkey_adapter.cpp
    ../src/openkey_adapter.cpp
    ../src/openkey_engine_globals.cpp
    ../src/surrounding_text_utils.cpp
    ../Sources/OpenKey/engine/ConvertTool.cpp
    ../Sources/OpenKey/engine/Engine.cpp
    ../Sources/OpenKey/engine/Macro.cpp
    ../Sources/OpenKey/engine/Vietnamese.cpp
)

target_include_directories(openkey_adapter_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Sources/OpenKey/engine"
)

target_link_libraries(openkey_adapter_tests PRIVATE
    Fcitx5::Utils
)

add_test(NAME openkey_adapter_tests COMMAND openkey_adapter_tests)
