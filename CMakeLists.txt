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
target_link_libraries(sample_game PRIVATE sama_3d)                                                                                                                                                                                                     
                                              
if(APPLE)                                                                                                                                                                                                                                          
    target_link_libraries(sample_game PRIVATE                                                                                                                                                                                                          
        "-framework Cocoa"               
        "-framework Metal"                                                                                                                                                                                                                         
        "-framework QuartzCore"                                                                                                                                                                                                                    
        "-framework IOKit"     
        "-framework CoreFoundation"                                                                                                                                                                                                                
    )                                                           
endif()

# Copy any game assets next to the binary
file(COPY assets DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
