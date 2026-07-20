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
