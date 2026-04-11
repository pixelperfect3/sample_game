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
                                        
add_executable(sample_game main.mm SampleGame.cpp)                                                                                                                                                                                                         
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
