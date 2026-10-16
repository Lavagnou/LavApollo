# windows specific packaging
install(TARGETS sunshine RUNTIME DESTINATION "." COMPONENT application)

# Hardening: include zlib1.dll (loaded via LoadLibrary() in openssl's libcrypto.a)
install(FILES "${ZLIB}" DESTINATION "." COMPONENT application)

# ViGEmBus installer
set(VIGEMBUS_INSTALLER "${CMAKE_BINARY_DIR}/vigembus_installer.exe")
file(DOWNLOAD
        "https://github.com/nefarius/ViGEmBus/releases/download/v1.21.442.0/ViGEmBus_1.21.442_x64_x86_arm64.exe"
        ${VIGEMBUS_INSTALLER}
        SHOW_PROGRESS
        EXPECTED_HASH SHA256=155c50f1eec07bdc28d2f61a3e3c2c6c132fee7328412de224695f89143316bc
        TIMEOUT 60
)
install(FILES ${VIGEMBUS_INSTALLER}
        DESTINATION "scripts"
        RENAME "vigembus_installer.exe"
        COMPONENT gamepad)

# Adding tools
install(TARGETS dxgi-info RUNTIME DESTINATION "tools" COMPONENT dxgi)
install(TARGETS audio-info RUNTIME DESTINATION "tools" COMPONENT audio)

# Mandatory tools
install(TARGETS sunshinesvc RUNTIME DESTINATION "tools" COMPONENT application)
# Windows.Graphics.Capture from the service: ipc_session_t launches it from <install>/tools
install(TARGETS sunshine_wgc_capture RUNTIME DESTINATION "tools" COMPONENT application)
# The built-in "Benchmark" app runs it from <install>/tools (process.cpp)
install(TARGETS lavapollo-benchmark-scene RUNTIME DESTINATION "tools" COMPONENT application)

# Drivers
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/drivers/sudovda"
        DESTINATION "drivers"
        COMPONENT sudovda)

# VHF virtual gamepad driver (Nonary's libvirtualgamepad). Bundled only when the release workflow
# prepared a package signed with LavApollo's certificate (scripts/prepare-vhf-gamepad.ps1): without
# the signing secrets, LavApollo ships without it and ViGEmBus remains the only gamepad driver.
set(LAVAPOLLO_VHF_GAMEPAD_PACKAGE_DIR "" CACHE PATH
    "Signed VHF gamepad driver package, as produced by scripts/prepare-vhf-gamepad.ps1")
if(LAVAPOLLO_VHF_GAMEPAD_PACKAGE_DIR)
    foreach(_vhf_file IN ITEMS
            driver/VibeshineVhfGamepad.inf
            driver/VibeshineVhfGamepad.dll
            driver/VibeshineVhfGamepad.cat
            driver/LavApolloVhfGamepad.cer
            tools/VibeshineVhfGamepadDeviceSetup.exe)
        if(NOT EXISTS "${LAVAPOLLO_VHF_GAMEPAD_PACKAGE_DIR}/${_vhf_file}")
            message(FATAL_ERROR "VHF gamepad package is missing ${_vhf_file}: "
                                "${LAVAPOLLO_VHF_GAMEPAD_PACKAGE_DIR}")
        endif()
    endforeach()
    unset(_vhf_file)
    message(STATUS "Bundling the VHF gamepad driver from ${LAVAPOLLO_VHF_GAMEPAD_PACKAGE_DIR}")
    install(DIRECTORY "${LAVAPOLLO_VHF_GAMEPAD_PACKAGE_DIR}/driver"
                      "${LAVAPOLLO_VHF_GAMEPAD_PACKAGE_DIR}/tools"
            DESTINATION "drivers/vhf-gamepad"
            COMPONENT vhf_gamepad)
    install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/drivers/vhf-gamepad/install.ps1"
            DESTINATION "drivers/vhf-gamepad"
            COMPONENT vhf_gamepad)
endif()

# Mandatory scripts
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/service/"
        DESTINATION "scripts"
        COMPONENT assets)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/migration/"
        DESTINATION "scripts"
        COMPONENT assets)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/path/"
        DESTINATION "scripts"
        COMPONENT assets)

# Configurable options for the service
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/autostart/"
        DESTINATION "scripts"
        COMPONENT autostart)

# scripts
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/firewall/"
        DESTINATION "scripts"
        COMPONENT firewall)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/gamepad/"
        DESTINATION "scripts"
        COMPONENT gamepad)

# Sunshine assets
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/assets/"
        DESTINATION "${SUNSHINE_ASSETS_DIR}"
        COMPONENT assets)

# copy assets (excluding shaders) to build directory, for running without install
file(COPY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/assets/"
        DESTINATION "${CMAKE_BINARY_DIR}/assets"
        PATTERN "shaders" EXCLUDE)
# use junction for shaders directory
cmake_path(CONVERT "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/assets/shaders"
        TO_NATIVE_PATH_LIST shaders_in_build_src_native)
cmake_path(CONVERT "${CMAKE_BINARY_DIR}/assets/shaders" TO_NATIVE_PATH_LIST shaders_in_build_dest_native)
execute_process(COMMAND cmd.exe /c mklink /J "${shaders_in_build_dest_native}" "${shaders_in_build_src_native}")

set(CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}\\\\apollo.ico")

# The name of the directory that will be created in C:/Program files/
set(CPACK_PACKAGE_INSTALL_DIRECTORY "${CPACK_PACKAGE_NAME}")

# Setting components groups and dependencies
set(CPACK_COMPONENT_GROUP_CORE_EXPANDED true)

# sunshine binary
set(CPACK_COMPONENT_APPLICATION_DISPLAY_NAME "${CMAKE_PROJECT_NAME}")
set(CPACK_COMPONENT_APPLICATION_DESCRIPTION "${CMAKE_PROJECT_NAME} main application and required components.")
set(CPACK_COMPONENT_APPLICATION_GROUP "Core")
set(CPACK_COMPONENT_APPLICATION_REQUIRED true)
set(CPACK_COMPONENT_APPLICATION_DEPENDS assets)

# service auto-start script
set(CPACK_COMPONENT_AUTOSTART_DISPLAY_NAME "Launch on Startup")
set(CPACK_COMPONENT_AUTOSTART_DESCRIPTION "If enabled, launches LavApollo automatically on system startup.")
set(CPACK_COMPONENT_AUTOSTART_GROUP "Core")

# assets
set(CPACK_COMPONENT_ASSETS_DISPLAY_NAME "Required Assets")
set(CPACK_COMPONENT_ASSETS_DESCRIPTION "Shaders, default box art, and web UI.")
set(CPACK_COMPONENT_ASSETS_GROUP "Core")
set(CPACK_COMPONENT_ASSETS_REQUIRED true)

# drivers
set(CPACK_COMPONENT_SUDOVDA_DISPLAY_NAME "SudoVDA")
set(CPACK_COMPONENT_SUDOVDA_DESCRIPTION "Driver required for Virtual Display to function.")
set(CPACK_COMPONENT_SUDOVDA_GROUP "Drivers")
set(CPACK_COMPONENT_SUDOVDA_REQUIRED true)
set(CPACK_COMPONENT_VHF_GAMEPAD_DISPLAY_NAME "VHF Virtual Gamepad")
set(CPACK_COMPONENT_VHF_GAMEPAD_DESCRIPTION
    "Virtual gamepad driver (Xbox, DualShock 4, DualSense, Switch Pro), an alternative to ViGEmBus.")
set(CPACK_COMPONENT_VHF_GAMEPAD_GROUP "Drivers")

# audio tool
set(CPACK_COMPONENT_AUDIO_DISPLAY_NAME "audio-info")
set(CPACK_COMPONENT_AUDIO_DESCRIPTION "CLI tool providing information about sound devices.")
set(CPACK_COMPONENT_AUDIO_GROUP "Tools")

# display tool
set(CPACK_COMPONENT_DXGI_DISPLAY_NAME "dxgi-info")
set(CPACK_COMPONENT_DXGI_DESCRIPTION "CLI tool providing information about graphics cards and displays.")
set(CPACK_COMPONENT_DXGI_GROUP "Tools")

# firewall scripts
set(CPACK_COMPONENT_FIREWALL_DISPLAY_NAME "Add Firewall Exclusions")
set(CPACK_COMPONENT_FIREWALL_DESCRIPTION "Scripts to enable or disable firewall rules.")
set(CPACK_COMPONENT_FIREWALL_GROUP "Scripts")

# gamepad scripts
set(CPACK_COMPONENT_GAMEPAD_DISPLAY_NAME "Virtual Gamepad")
set(CPACK_COMPONENT_GAMEPAD_DESCRIPTION "Scripts to install and uninstall Virtual Gamepad.")
set(CPACK_COMPONENT_GAMEPAD_GROUP "Scripts")

# include specific packaging
include(${CMAKE_MODULE_PATH}/packaging/windows_nsis.cmake)
include(${CMAKE_MODULE_PATH}/packaging/windows_wix.cmake)
