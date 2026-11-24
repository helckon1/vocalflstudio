cmake_minimum_required(VERSION 3.22)
project(VocalAtmosphere VERSION 1.0.0)
set(CMAKE_CXX_STANDARD 17)
include(FetchContent)
FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.6 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)
juce_add_plugin(VocalAtmosphere
 COMPANY_NAME "Vocal Atmosphere" BUNDLE_ID "audio.vocalatmosphere.plugin"
 PLUGIN_MANUFACTURER_CODE VAtm PLUGIN_CODE Va01
 FORMATS VST3 PRODUCT_NAME "Vocal Atmosphere"
 IS_SYNTH FALSE NEEDS_MIDI_INPUT FALSE NEEDS_MIDI_OUTPUT FALSE
 COPY_PLUGIN_AFTER_BUILD FALSE)
juce_generate_juce_header(VocalAtmosphere)
target_sources(VocalAtmosphere PRIVATE Source/PluginProcessor.cpp Source/PluginProcessor.h)
target_compile_definitions(VocalAtmosphere PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(VocalAtmosphere PRIVATE juce::juce_audio_utils juce::juce_dsp PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
