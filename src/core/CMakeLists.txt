# loftail_core: UI-free logic. Links QtCore only — never QtWidgets — so that
# parsing/indexing code stays unit-testable without a QApplication instance
# (see CLAUDE.md conventions).
add_library(loftail_core STATIC
    Version.cpp
    Version.h
)

target_link_libraries(loftail_core PUBLIC Qt6::Core)

target_include_directories(loftail_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(loftail_core PRIVATE
    LOFTAIL_VERSION="${PROJECT_VERSION}"
)
