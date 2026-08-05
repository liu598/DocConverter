cmake_minimum_required(VERSION 3.16)
project(DocConverter VERSION 1.0 LANGUAGES CXX)

set(CMAKE_INSTALL_PREFIX
    "${CMAKE_SOURCE_DIR}/deploy")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# 如果 Qt 没有加入系统 PATH，请在生成时通过
# -DCMAKE_PREFIX_PATH="C:/Qt/6.7.0/msvc2019_64" 指定 Qt 安装路径
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(DocConverter WIN32
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/converter.cpp
    src/converter.h
    src/installer.cpp
    src/installer.h
    resources/app.rc
    resources/icons.qrc
)

target_link_libraries(DocConverter PRIVATE Qt6::Widgets)

# 把 scripts 目录（pdf2docx 调用脚本）拷贝到编译输出目录，跟exe放一起
add_custom_command(TARGET DocConverter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/scripts"
            "$<TARGET_FILE_DIR:DocConverter>/scripts"
)

#Qt自动部署
if(WIN32)

    qt_generate_deploy_app_script(
        TARGET DocConverter
        OUTPUT_SCRIPT deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )


    install(
        SCRIPT ${deploy_script}
    )


endif()
