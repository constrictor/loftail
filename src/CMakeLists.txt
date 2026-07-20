# src/core holds all UI-free logic (unit-testable without a QApplication);
# src/ui holds the Qt Widgets layer.
add_subdirectory(core)
add_subdirectory(ui)

qt_add_executable(loftail
    main.cpp
)

target_link_libraries(loftail PRIVATE
    loftail_core
    loftail_ui
)
