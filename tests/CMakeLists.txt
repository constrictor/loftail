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
