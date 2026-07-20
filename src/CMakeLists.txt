# src/core holds all UI-free logic (unit-testable without a QApplication);
# src/ui holds the Qt Widgets layer.
add_subdirectory(core)
add_subdirectory(ui)

# M2a throwaway targets: the performance harness (console) and the exact-geometry
# scrolling prototype of LogView. Both are disposable and superseded in M2b.
add_subdirectory(bench)
add_subdirectory(proto)

qt_add_executable(loftail
    main.cpp
)

target_link_libraries(loftail PRIVATE
    loftail_core
    loftail_ui
)
