cmake_minimum_required(VERSION 3.20)                                                                                                                                                                                                               
project(SampleGame CXX)                                                                                                                                                                                                                                
                                                                                                                                                                                                                                                   
set(CMAKE_CXX_STANDARD 20)                                                                                                                                                                                                                         
set(CMAKE_CXX_STANDARD_REQUIRED ON)                                                                                                                                                                                                                
                                                                
include(FetchContent)
FetchContent_Declare(                                                                                                                                                                                                                              
    sama             
    GIT_REPOSITORY https://github.com/pixelperfect3/sama.git                                                                                                                                                                                       
    GIT_TAG        main  # pin to a specific commit for production
)                                                                 
FetchContent_MakeAvailable(sama)

# On Android, bgfx's entry_android.cpp needs the native_app_glue header.
# The native_app_glue target's PUBLIC include dir doesn't reach bgfx since
# bgfx doesn't link against it — add the include dir explicitly.
if(SAMA_ANDROID AND CMAKE_ANDROID_NDK)
    target_include_directories(bgfx PUBLIC
        "${CMAKE_ANDROID_NDK}/sources/android/native_app_glue")
endif()

# ---------------------------------------------------------------------------
# Platform-specific sources and linking
# ---------------------------------------------------------------------------
if(SAMA_ANDROID)
    # Android: shared library loaded by NativeActivity.
    # engine_android transitively brings in engine_game, engine_core,
    # engine_rendering, engine_scene, engine_ecs, etc.
    add_library(sample_game SHARED main_android.cpp SampleGame.cpp)
    target_link_libraries(sample_game PRIVATE
        engine_android
        engine_physics
        engine_audio
        engine_animation
        engine_assets
        engine_ui
        EGL
        GLESv2
    )
    # Ensure ANativeActivity_onCreate is not stripped.
    set_target_properties(sample_game PROPERTIES
        LINK_FLAGS "-u ANativeActivity_onCreate"
        OUTPUT_NAME "sample_game"
    )
else()
    # Desktop (macOS): executable with Objective-C++ entry point
    # imgui_tables.cpp is needed by the latest ImGui but missing from
    # Sama's engine_debug; add it here as a workaround.
    add_executable(sample_game main.mm SampleGame.cpp)
    # imgui_tables.cpp is needed by latest ImGui but missing from Sama's
    # engine_debug; compile it into our target as a workaround.
    target_sources(sample_game PRIVATE
        "${FETCHCONTENT_BASE_DIR}/bgfx_cmake-src/bgfx/3rdparty/dear-imgui/imgui_tables.cpp")
    target_link_libraries(sample_game PRIVATE sama_3d engine_ui)

    if(APPLE)
        target_link_libraries(sample_game PRIVATE
            "-framework Cocoa"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework IOKit"
            "-framework CoreFoundation"
        )
    endif()

    # Copy any game assets next to the binary (for bare-executable dev builds)
    file(COPY assets DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
    file(COPY levels DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

    # --- macOS app bundle ---
    set_target_properties(sample_game PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/packaging/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Sample Game"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.pixelperfect3.samplegame"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    )

    # Copy assets and levels into the app bundle's Resources directory
    set(BUNDLE_RESOURCES_DIR "$<TARGET_BUNDLE_DIR:sample_game>/Contents/Resources")
    add_custom_command(TARGET sample_game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets" "${BUNDLE_RESOURCES_DIR}/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/levels" "${BUNDLE_RESOURCES_DIR}/levels"
        COMMENT "Copying assets and levels into app bundle"
    )
endif()
