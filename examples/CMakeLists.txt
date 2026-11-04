# 示例模块
add_library(example_basic SHARED basic.c)
target_link_libraries(example_basic PRIVATE zh_engine)
target_include_directories(example_basic PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(example_advanced SHARED advanced.c)
target_link_libraries(example_advanced PRIVATE zh_engine)
target_include_directories(example_advanced PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(example_verify SHARED verify.c)
target_link_libraries(example_verify PRIVATE zh_engine)
target_include_directories(example_verify PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_subdirectory(steps)
