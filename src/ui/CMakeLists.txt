# loftail_ui: the Qt Widgets layer. Links QtWidgets and loftail_core.
add_library(loftail_ui STATIC
    MainWindow.cpp
    MainWindow.h
    # M2b — the production custom record view
    LogView.cpp
    LogView.h
    # M3 — the Log Format dialog
    LogFormatDialog.cpp
    LogFormatDialog.h
    # M4 — the Filters side pane and the Find bar
    FilterPane.cpp
    FilterPane.h
    FindBar.cpp
    FindBar.h
)

target_link_libraries(loftail_ui PUBLIC
    Qt6::Widgets
    loftail_core
)

target_include_directories(loftail_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
