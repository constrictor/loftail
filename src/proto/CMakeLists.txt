# Throwaway M2a scrolling prototype of LogView in exact geometry mode. A separate
# executable from the product (src/loftail) to keep it clearly disposable; it will
# be superseded by the production LogView in M2b.
qt_add_executable(loftail_proto
    main.cpp
    LogViewProto.cpp
    LogViewProto.h
)

target_link_libraries(loftail_proto PRIVATE
    loftail_core
    Qt6::Widgets
)
