cmake_minimum_required(VERSION 3.22)

project(HanChi VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- 拉取 JUCE 8（云端/本地构建均自动下载，无需手动安装） ----
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.8
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(HanChi
    PRODUCT_NAME               "HanChi"
    COMPANY_NAME               "A-Yan Studio"
    BUNDLE_ID                  "com.ayanstudio.hanchi"
    PLUGIN_MANUFACTURER_CODE   Ayan
    PLUGIN_CODE                Hnch
    FORMATS                    VST3
    IS_SYNTH                   FALSE
    NEEDS_MIDI_INPUT           FALSE
    NEEDS_MIDI_OUTPUT          FALSE
    IS_MIDI_EFFECT             FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD    FALSE
    VST3_CATEGORIES            Fx DSP
)

juce_generate_juce_header(HanChi)

target_sources(HanChi PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

target_compile_definitions(HanChi PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

# 源文件为 UTF-8 编码；强制 MSVC 以 UTF-8 读取源文件、并以 UTF-8 作为执行字符集，
# 否则中文字符串会被按系统代码页(GBK)编译，运行时显示为乱码。
if (MSVC)
    target_compile_options(HanChi PRIVATE /utf-8)
endif()

target_link_libraries(HanChi
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
