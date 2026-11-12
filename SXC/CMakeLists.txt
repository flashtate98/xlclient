# SXC: our own layer in the client (orchestration/docs/decisions/0005-fork-first.md). A static library
# linked into the client executables from here, so the root CMakeLists.txt carries only the
# add_subdirectory line that pulls this folder in.

# Linking targets defined in other directories.
cmake_policy(SET CMP0079 NEW)

# The SDK checkout (WarcraftXL/CMakeLists.txt sets it; the same default here).
if (NOT WXL_SDK_DIR)
    set(WXL_SDK_DIR "${CMAKE_SOURCE_DIR}/../sdk")
endif ()

file(GLOB_RECURSE SXC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
# The bridge is a mod of its own (below), not part of the executable.
list(FILTER SXC_SOURCES EXCLUDE REGEX "/Bridge/")

add_library(SXC STATIC
    ${SXC_SOURCES}
)

target_include_directories(SXC
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        # Dear ImGui (ThirdParty/imgui/VERSION): its backends include "imgui.h" from here, and modules
        # drawing overlay panels may too.
        ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/imgui
    PRIVATE
        # The SDK's boundary types (wxl/abi.h) for our services (SXC/Api).
        "${WXL_SDK_DIR}/include"
)

# The overlay's Vulkan side: the Khronos headers without prototypes, as the backend uses them; ImGui
# loads its Vulkan functions through the backend's vkGetInstanceProcAddr (SXC/Ui/UiVulkan.h).
target_link_libraries(SXC
    PUBLIC
        VulkanHeaders
)
target_compile_definitions(SXC
    PUBLIC
        IMGUI_IMPL_VULKAN_NO_PROTOTYPES
)

# The build number the client sends to the login server, which compares it against the realm list
# (tswow's gamebuild patch; tswow can set it per dataset).
set(SXC_GAME_BUILD 12340 CACHE STRING "Build number sent to the login server (stock 12340)")
if (TARGET WowServices)
    target_compile_definitions(WowServices PRIVATE SXC_GAME_BUILD=${SXC_GAME_BUILD})
endif ()

# sxc.bridge (SXC/Bridge): the mod that publishes our services (SXC/Api) through the WarcraftXL host.
# Lands where the runtime looks: bin/Win/Mods/sxc.bridge/{mod.json, bin/x64/sxc-bridge.dll}.
set(SXC_BRIDGE_DIR "${CMAKE_SOURCE_DIR}/bin/Win/Mods/sxc.bridge")
add_library(sxc-bridge SHARED "${CMAKE_CURRENT_SOURCE_DIR}/Bridge/Bridge.cpp")
target_include_directories(sxc-bridge PRIVATE ${CMAKE_SOURCE_DIR} "${WXL_SDK_DIR}/include")
set_target_properties(sxc-bridge PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SXC_BRIDGE_DIR}/bin/x64")
foreach (config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${config} config)
    set_target_properties(sxc-bridge PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${SXC_BRIDGE_DIR}/bin/x64")
endforeach ()
add_custom_command(TARGET sxc-bridge POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/Bridge/mod.json" "${SXC_BRIDGE_DIR}/mod.json")
# Built with the client.
if (TARGET WowClient)
    add_dependencies(WowClient sxc-bridge)
endif ()

# Every executable that links the engine libraries, which call into SXC (the root's list of
# executables, including tools that exist only on some upstream branches).
foreach (target Wow WowClient MapViewer M2Viewer GxTest MpqTest BlpTest DbcTest AssetCheck)
    if (TARGET ${target})
        target_link_libraries(${target} PRIVATE SXC)
    endif ()
endforeach ()
