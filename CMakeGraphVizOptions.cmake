# CMake reads these options automatically with `--graphviz`.

set(GRAPHVIZ_GRAPH_NAME "cwrap target dependencies")
set(GRAPHVIZ_GRAPH_HEADER
    "graph [ rankdir = \"LR\", bgcolor = \"transparent\" ];\nnode [ fontsize = \"11\" ];\nedge [ fontsize = \"9\" ];"
)

set(GRAPHVIZ_EXECUTABLES TRUE)
set(GRAPHVIZ_STATIC_LIBS TRUE)
set(GRAPHVIZ_SHARED_LIBS FALSE)
set(GRAPHVIZ_MODULE_LIBS FALSE)
set(GRAPHVIZ_INTERFACE_LIBS TRUE)
set(GRAPHVIZ_OBJECT_LIBS FALSE)
set(GRAPHVIZ_UNKNOWN_LIBS FALSE)
set(GRAPHVIZ_EXTERNAL_LIBS TRUE)
set(GRAPHVIZ_CUSTOM_TARGETS FALSE)

set(GRAPHVIZ_IGNORE_TARGETS
    "cwrap_build_tests"
    "cwrap_unit_.*"
    "cwrap_vendor_acutest"
)

set(GRAPHVIZ_GENERATE_PER_TARGET FALSE)
set(GRAPHVIZ_GENERATE_DEPENDERS FALSE)
