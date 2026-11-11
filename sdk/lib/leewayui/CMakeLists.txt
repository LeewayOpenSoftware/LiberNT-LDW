add_library(leewayui_static STATIC
    ribbon.cpp
    frame.cpp
    theme.cpp
    dpi.cpp
    gamestats.cpp
    gamesave.cpp
    gameanim.cpp
    gameshell.cpp
)

target_include_directories(leewayui_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/sdk/include>
)

add_definitions(-DUNICODE -D_UNICODE)

set_target_properties(leewayui_static PROPERTIES
    CXX_STANDARD 14
    FOLDER "Libraries"
)

add_dependencies(leewayui_static psdk)
