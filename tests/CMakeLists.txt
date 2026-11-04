# 测试：host 端可跑的部分（不依赖游戏）
#
# 注意：引擎主体需要真实进程才能测试，这里只测纯逻辑部分。
# 真机测试见 tests/ondevice/。

add_executable(test_strerror test_strerror.c)
target_link_libraries(test_strerror PRIVATE zh_engine)
target_include_directories(test_strerror PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME strerror COMMAND test_strerror)

add_executable(test_datatable test_datatable.c)
target_include_directories(test_datatable PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/data)
add_test(NAME datatable COMMAND test_datatable)

add_executable(test_insn test_insn.c)
target_include_directories(test_insn PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME insn COMMAND test_insn)

add_executable(test_layer test_layer.c)
target_link_libraries(test_layer PRIVATE zh_engine)
target_include_directories(test_layer PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME layer COMMAND test_layer)

add_executable(test_chain test_chain.c)
target_link_libraries(test_chain PRIVATE zh_engine)
target_include_directories(test_chain PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME chain COMMAND test_chain)

add_executable(test_guard test_guard.c)
target_link_libraries(test_guard PRIVATE zh_engine)
target_include_directories(test_guard PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME guard COMMAND test_guard)

add_executable(test_db test_db.c)
target_link_libraries(test_db PRIVATE zh_engine)
target_include_directories(test_db PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME db COMMAND test_db ${CMAKE_SOURCE_DIR}/data/zh_db_test.bin)

add_executable(test_engine test_engine.c)
target_link_libraries(test_engine PRIVATE zh_engine)
target_include_directories(test_engine PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME engine COMMAND test_engine)

add_executable(test_typed test_typed.c)
target_link_libraries(test_typed PRIVATE zh_engine)
target_include_directories(test_typed PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME typed COMMAND test_typed)

add_executable(test_tbl test_tbl.c)
target_link_libraries(test_tbl PRIVATE zh_engine)
target_include_directories(test_tbl PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/data)
add_test(NAME tbl COMMAND test_tbl ${CMAKE_SOURCE_DIR}/data/zh_db_full.bin)

add_executable(test_sites test_sites.c)
target_link_libraries(test_sites PRIVATE zh_engine)
target_include_directories(test_sites PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME sites COMMAND test_sites)

add_executable(test_dispatch test_dispatch.c)
target_link_libraries(test_dispatch PRIVATE zh_engine)
target_include_directories(test_dispatch PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME dispatch COMMAND test_dispatch)
