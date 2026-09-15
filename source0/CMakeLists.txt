cmake_minimum_required(VERSION 3.17)
project(pge_treemap)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED yes)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# AddressSanitizer
option(ENABLE_ASAN "Enable AddressSanitizer" ON)
if (ENABLE_ASAN)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif ()

# UndefinedBehaviorSanitizer
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" ON)
if (ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
endif ()

# ThreadSanitizer
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
if (ENABLE_TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif ()

# LeakSanitizer
option(ENABLE_LSAN "Enable LeakSanitizer" ON)
if (ENABLE_LSAN)
    add_compile_options(-fsanitize=leak)
    add_link_options(-fsanitize=leak)
endif ()

find_package(X11 REQUIRED COMPONENTS Xi) # 1. Request the Xi component
find_package(OpenGL REQUIRED)
find_package(PNG REQUIRED)

file(GLOB_RECURSE SOURCE_FILES src/*.cpp)

add_executable(
    ${PROJECT_NAME}
    ${SOURCE_FILES}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    X11::X11
    X11::Xi                              # 2. Link the XInput library
    OpenGL::GL
    PNG::PNG
    pthread
)


target_include_directories(
        ${PROJECT_NAME}
        PRIVATE
	/home/kiel/src/olcPixelGameEngine3
#        ${CMAKE_CURRENT_BINARY_DIR} 
)


#     mkdir build && cd build && cmake .. -GNinja -DCMAKE_BUILD_TYPE=Release && cmake --build .
