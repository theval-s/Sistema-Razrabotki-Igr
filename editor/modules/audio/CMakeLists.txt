sri_add_editor_module(editor_module_audio
    audio_module.cpp
    audio_panel.cpp
    audio_panel.hpp
    audio_probe.hpp
)
target_link_libraries(editor_module_audio PRIVATE Qt6::Widgets Qt6::Core)
