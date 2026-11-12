cmake_minimum_required(VERSION 3.21)

# 版本号的唯一来源：改这里，exe 资源、程序标题、安装包都跟着变
project(DocConverter VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/deploy" CACHE PATH "安装/部署目录")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 如果 Qt 没有加入系统 PATH，请在生成时通过
# -DCMAKE_PREFIX_PATH="D:/Qt/6.11.1/msvc2022_64" 指定 Qt 安装路径
find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)

# ---------------------------------------------------------------------------
# 生成版本头文件，避免版本号散落在多个文件里各写一遍
# ---------------------------------------------------------------------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/DocConverterVersion.h"
    @ONLY
)

# ---------------------------------------------------------------------------
# 生成资源脚本：图标 + VERSIONINFO（右键属性里能看到版本和产品名）
# ---------------------------------------------------------------------------
set(APP_RC "")
if(WIN32)
    # 注意：rc.exe 里反斜杠会被当转义符（\a 会变成响铃），必须用正斜杠
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/app_icon.ico" APP_ICON_RC_PATH)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc"
        @ONLY
    )
    set(APP_RC "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc")
endif()

add_executable(DocConverter WIN32
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/converter.cpp
    src/converter.h
    src/converterworker.cpp
    src/converterworker.h
    src/installer.cpp
    src/installer.h
    src/installerworker.cpp
    src/installerworker.h
    resources/icons.qrc
    ${APP_RC}
)

target_include_directories(DocConverter PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_link_libraries(DocConverter PRIVATE Qt6::Widgets)

# 测试：ctest --test-dir build --output-on-failure
# 不需要测试时可以 -DBUILD_TESTING=OFF 关掉
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# 只部署运行时需要的 python 脚本，构建脚本（build_release.ps1）不进安装包
add_custom_command(TARGET DocConverter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:DocConverter>/scripts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/scripts/pdf2docx_convert.py"
            "$<TARGET_FILE_DIR:DocConverter>/scripts/pdf2docx_convert.py"
)

# ---------------------------------------------------------------------------
# 安装 / 部署
#   用 `cmake --install build --config Release` 产出一个干净目录，
#   直接拿去打包，不会把 CMakeCache.txt、build.ninja 之类的中间产物带进去
# ---------------------------------------------------------------------------
install(TARGETS DocConverter RUNTIME DESTINATION .)
install(FILES "${CMAKE_SOURCE_DIR}/scripts/pdf2docx_convert.py" DESTINATION scripts)

# VC++ 运行库（msvcp140 / vcruntime140）：目标机器没装 VC++ Redistributable
# 时程序会直接启动失败，所以跟着一起打包
if(MSVC)
    include(InstallRequiredSystemLibraries)
    if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION .)
    endif()
endif()

if(WIN32)
    # 用 windeployqt 直接铺到安装目录（dll 与 exe 同级）。
    # 不用 qt_generate_deploy_app_script：Qt 6.11 那个脚本会把运行库放进 bin/，
    # 而 exe 在根目录时就找不到 Qt6Core.dll 了。
    get_target_property(_qmake_location Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake_location}" DIRECTORY)
    # 必须是原生反斜杠路径：带正斜杠的 exe 路径传给 execute_process 会返回「参数错误」
    file(TO_NATIVE_PATH "${_qt_bin_dir}/windeployqt.exe" WINDEPLOYQT_EXE)

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployApp.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/DeployApp.cmake"
        @ONLY
    )
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/generated/DeployApp.cmake")
endif()
