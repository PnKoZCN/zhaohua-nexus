add_library(t01_init SHARED t01_init.c)
target_link_libraries(t01_init PRIVATE zh_engine)
target_include_directories(t01_init PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t02_find SHARED t02_find.c)
target_link_libraries(t02_find PRIVATE zh_engine)
target_include_directories(t02_find PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t03_hook SHARED t03_hook.c)
target_link_libraries(t03_hook PRIVATE zh_engine)
target_include_directories(t03_hook PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t04_callback SHARED t04_callback.c)
target_link_libraries(t04_callback PRIVATE zh_engine)
target_include_directories(t04_callback PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t05_ret SHARED t05_ret.c)
target_link_libraries(t05_ret PRIVATE zh_engine)
target_include_directories(t05_ret PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(rescue SHARED rescue.c)
target_link_libraries(rescue PRIVATE zh_engine)
target_include_directories(rescue PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t05b_ret_tick SHARED t05b_ret_tick.c)
target_link_libraries(t05b_ret_tick PRIVATE zh_engine)
target_include_directories(t05b_ret_tick PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t05c_diag SHARED t05c_diag.c)
target_link_libraries(t05c_diag PRIVATE zh_engine)
target_include_directories(t05c_diag PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t06_field SHARED t06_field.c)
target_link_libraries(t06_field PRIVATE zh_engine)
target_include_directories(t06_field PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t07_local SHARED t07_local.c)
target_link_libraries(t07_local PRIVATE zh_engine)
target_include_directories(t07_local PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t08_multi SHARED t08_multi.c)
target_link_libraries(t08_multi PRIVATE zh_engine)
target_include_directories(t08_multi PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t09_guard SHARED t09_guard.c)
target_link_libraries(t09_guard PRIVATE zh_engine)
target_include_directories(t09_guard PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t10_db SHARED t10_db.c)
target_link_libraries(t10_db PRIVATE zh_engine)
target_include_directories(t10_db PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t11_multi_engine SHARED t11_multi_engine.c)
target_link_libraries(t11_multi_engine PRIVATE zh_engine)
target_include_directories(t11_multi_engine PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t11a_min SHARED t11a_min.c)
target_link_libraries(t11a_min PRIVATE zh_engine)
target_include_directories(t11a_min PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t11b_one SHARED t11b_one.c)
target_link_libraries(t11b_one PRIVATE zh_engine)
target_include_directories(t11b_one PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t12_redirect SHARED t12_redirect.c)
target_link_libraries(t12_redirect PRIVATE zh_engine)
target_include_directories(t12_redirect PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t13_const SHARED t13_const.c)
target_link_libraries(t13_const PRIVATE zh_engine)
target_include_directories(t13_const PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t13b_probe SHARED t13b_probe.c)
target_link_libraries(t13b_probe PRIVATE zh_engine)
target_include_directories(t13b_probe PRIVATE ${CMAKE_SOURCE_DIR}/include)

foreach(ph 1 2 3)
    add_library(t11x_p${ph} SHARED t11x_bisect.c)
    target_link_libraries(t11x_p${ph} PRIVATE zh_engine)
    target_include_directories(t11x_p${ph} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_definitions(t11x_p${ph} PRIVATE PHASE=${ph})
endforeach()

add_library(t11y_min SHARED t11y_min.c)
target_link_libraries(t11y_min PRIVATE zh_engine)
target_include_directories(t11y_min PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t11z_sp SHARED t11z_sp.c)
target_link_libraries(t11z_sp PRIVATE zh_engine)
target_include_directories(t11z_sp PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t08b_long SHARED t08b_long.c)
target_link_libraries(t08b_long PRIVATE zh_engine)
target_include_directories(t08b_long PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t08c_guard SHARED t08c_guard.c)
target_link_libraries(t08c_guard PRIVATE zh_engine)
target_include_directories(t08c_guard PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t00_idle SHARED t00_idle.c)
target_link_libraries(t00_idle PRIVATE zh_engine)
target_include_directories(t00_idle PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t00b_nowd SHARED t00b_nowd.c)
target_link_libraries(t00b_nowd PRIVATE zh_engine)
target_include_directories(t00b_nowd PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(t08d_async SHARED t08d_async.c)
target_link_libraries(t08d_async PRIVATE zh_engine)
target_include_directories(t08d_async PRIVATE ${CMAKE_SOURCE_DIR}/include)

# 多库共存测试（两个探针同时注入，共享同一站点）
add_library(mla SHARED mla.c)
target_link_libraries(mla PRIVATE zh_engine)
target_include_directories(mla PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_library(mlb SHARED mlb.c)
target_link_libraries(mlb PRIVATE zh_engine)
target_include_directories(mlb PRIVATE ${CMAKE_SOURCE_DIR}/include)

# 诊断探针：直读共享区 + tick 候选对比
add_library(mld SHARED mld.c)
target_link_libraries(mld PRIVATE zh_engine)
target_include_directories(mld PRIVATE ${CMAKE_SOURCE_DIR}/include)

# 独立诊断探针：把整份引擎静态打包进来，不依赖 libzh_engine.so。
# 用途：在共享引擎已被旧版本加载的情况下，验证引擎自身是否还工作。
if(TARGET zh_engine_static)
    add_library(mldiag SHARED mldiag.c)
    add_dependencies(mldiag zh_engine_static)
    target_include_directories(mldiag PRIVATE ${CMAKE_SOURCE_DIR}/include)
    # 必须把整个静态库都拉进来：引擎内部大量符号靠 shell 里的地址引用，
    # 没被显式调用的（汇编 handler 等）会被链接器当死代码丢掉。
    target_link_options(mldiag PRIVATE
        "LINKER:--whole-archive"
        "SHELL:$<TARGET_FILE:zh_engine_static>"
        "LINKER:--no-whole-archive")
endif()

# 收尾验证：最后一个归属者卸载后入口是否还原
add_library(mlc SHARED mlc.c)
target_link_libraries(mlc PRIVATE zh_engine)
target_include_directories(mlc PRIVATE ${CMAKE_SOURCE_DIR}/include)
