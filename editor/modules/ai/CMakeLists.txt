sri_add_editor_module(editor_module_ai
    ai_module.cpp
    tree_graph.cpp
    tree_graph.hpp
    tree_panel.cpp
    tree_panel.hpp
)
# QtNodes is a static library: this module carries its own copy.
target_link_libraries(editor_module_ai PRIVATE QtNodes::QtNodes)
