cmake_minimum_required(VERSION 3.19)
project(SilenceInstaller LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network)

qt_standard_project_setup()

qt_add_executable(SilenceInstallerClient
    src/client/main.cpp
)

target_link_libraries(SilenceInstallerClient
    PRIVATE
        Qt::Core
        Qt::Network
)

qt_add_executable(SilenceInstallerServer
    src/server/main.cpp
)

target_link_libraries(SilenceInstallerServer
    PRIVATE
        Qt::Core
        Qt::Network
)

include(GNUInstallDirs)

install(TARGETS SilenceInstallerClient SilenceInstallerServer
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET SilenceInstallerClient
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
