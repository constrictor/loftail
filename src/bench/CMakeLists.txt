# Throwaway M2a performance harness. Console only — links loftail_core (QtCore),
# never Widgets — so it measures the index/model path in isolation (§11).
qt_add_executable(bench_index
    bench_index.cpp
)

target_link_libraries(bench_index PRIVATE
    loftail_core
    Qt6::Core
)
