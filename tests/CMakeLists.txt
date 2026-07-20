# One trivial test wiring CTest to Qt Test. Core logic is testable without a
# QApplication, so tests link loftail_core only (never the UI).
qt_add_executable(tst_scaffold
    tst_scaffold.cpp
)

target_link_libraries(tst_scaffold PRIVATE
    loftail_core
    Qt6::Test
)

add_test(NAME tst_scaffold COMMAND tst_scaffold)

# M1 — PatternCompiler. Table-driven, links loftail_core only (no QApplication).
qt_add_executable(tst_patterncompiler
    tst_patterncompiler.cpp
)

target_link_libraries(tst_patterncompiler PRIVATE
    loftail_core
    Qt6::Test
)

add_test(NAME tst_patterncompiler COMMAND tst_patterncompiler)

# M2a — the spine. All link loftail_core only; none needs a QApplication.
foreach(unit decoder indexer logmodel)
    qt_add_executable(tst_${unit} tst_${unit}.cpp)
    target_link_libraries(tst_${unit} PRIVATE loftail_core Qt6::Test)
    add_test(NAME tst_${unit} COMMAND tst_${unit})
endforeach()

# M2b — worker-thread indexing controller. Core-only, GUILESS (QCoreApplication
# event loop drives the queued cross-thread signals).
qt_add_executable(tst_indexcontroller tst_indexcontroller.cpp)
target_link_libraries(tst_indexcontroller PRIVATE loftail_core Qt6::Test)
add_test(NAME tst_indexcontroller COMMAND tst_indexcontroller)

# M2b — the production LogView. Links the UI layer; the geometry mapping and
# clipboard builders are pure, and the header-state round-trip needs QWidget, so
# the test runs under an offscreen QApplication (forced in its own main()).
qt_add_executable(tst_logview tst_logview.cpp)
target_link_libraries(tst_logview PRIVATE loftail_ui Qt6::Test Qt6::Widgets)
add_test(NAME tst_logview COMMAND tst_logview)
set_tests_properties(tst_logview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
