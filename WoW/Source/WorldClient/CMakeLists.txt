file(GLOB_RECURSE PRIVATE_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_library(WorldClient STATIC
    ${PRIVATE_SOURCES}
)

target_include_directories(WorldClient
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(WorldClient
    PRIVATE
        Extension
)

# LivingWind hands the Retail armor physics its wind (needs the WOWDECOMP_RETAIL_* defines RetailAssets carries)
if (WOWDECOMP_RETAIL_ASSETS)
    target_link_libraries(WorldClient PRIVATE RetailAssets)
endif ()
