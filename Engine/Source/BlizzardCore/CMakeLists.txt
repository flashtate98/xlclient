file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(BlizzardCore STATIC
    ${PRIVATE_SOURCES}
)

# The Retail model loader lives in Engine/Source/Retail; SFile and Model2 call into it.
if (WOWDECOMP_RETAIL_MODELS)
    target_sources(BlizzardCore PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Source/Retail/LocalSource.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Source/Retail/RetailModel.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Source/Retail/Db2.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Source/Retail/Tables.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Source/Retail/Appearance.cpp
    )
endif()

target_include_directories(BlizzardCore
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(BlizzardCore
    PUBLIC
        blast
        lzma
        zlib
)

if(WIN32)
    target_link_libraries(BlizzardCore
        PUBLIC
            wininet
    )
endif()

target_include_directories(BlizzardCore
    PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Source/SoundEngine/fmodex/lib/ogg_vorbis/ogg/include
        ${CMAKE_SOURCE_DIR}/Engine/Source/SoundEngine/fmodex/lib/ogg_vorbis/vorbis/include
)
