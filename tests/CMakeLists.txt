# Converter 的单元测试与引擎集成测试
find_package(Qt6 6.2 REQUIRED COMPONENTS Core Test)

add_executable(test_converter
    test_converter.cpp
    ${CMAKE_SOURCE_DIR}/src/converter.cpp
    ${CMAKE_SOURCE_DIR}/src/converter.h
)

target_include_directories(test_converter PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_converter PRIVATE Qt6::Core Qt6::Test)

add_test(NAME converter COMMAND test_converter)
set_tests_properties(converter PROPERTIES TIMEOUT 600)

# 让测试进程能找到 Qt6Core.dll / Qt6Test.dll，不用手动改 PATH
if(WIN32)
    set_tests_properties(converter PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:Qt6::Core>"
    )
endif()
