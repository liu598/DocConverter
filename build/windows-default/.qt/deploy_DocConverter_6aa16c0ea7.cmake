include("D:/Docconverter_1/DocConverter/build/windows-default/.qt/QtDeploySupport.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/DocConverter-plugins.cmake" OPTIONAL)
set(__QT_DEPLOY_I18N_CATALOGS "qtbase")

qt6_deploy_runtime_dependencies(
    EXECUTABLE "D:/Docconverter_1/DocConverter/build/windows-default/DocConverter.exe"
    GENERATE_QT_CONF
)
