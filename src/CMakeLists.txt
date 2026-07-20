# src/core holds all UI-free logic (unit-testable without a QApplication);
# src/ui holds the Qt Widgets layer.
add_subdirectory(core)
add_subdirectory(ui)

# M2a throwaway performance harness (console). The exact-geometry scrolling
# prototype (src/proto) has been retired now that the production LogView (src/ui)
# exists; the bench harness is kept because it still measures the §11 targets.
add_subdirectory(bench)

qt_add_executable(loftail
    main.cpp
)

target_link_libraries(loftail PRIVATE
    loftail_core
    loftail_ui
)
