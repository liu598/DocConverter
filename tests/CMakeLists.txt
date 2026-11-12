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

# Converter 会从 exe 同级的 scripts/ 里找 pdf2docx 的调用脚本，
# 主程序由主 CMakeLists 的 POST_BUILD 拷过去，测试目标这边也要来一份，
# 否则 PDF → 文本的用例会静默退化到 LibreOffice 路径。
add_custom_command(TARGET test_converter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/scripts/pdf2docx_convert.py"
            "$<TARGET_FILE_DIR:test_converter>/scripts/pdf2docx_convert.py"
)

# 让测试进程能找到 Qt6Core.dll / Qt6Test.dll，不用手动改 PATH
if(WIN32)
    set_tests_properties(converter PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:Qt6::Core>"
    )
endif()
