add_executable(seckill-cpp
    main.cc
    controllers/HealthController.cc
    controllers/SeckillController.cc
    service/SeckillService.cc
)

# 头文件搜索根放在 src/，这样 controller/service 互相 include 时用 "service/SeckillService.h" 即可。
target_include_directories(seckill-cpp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(seckill-cpp PRIVATE Drogon::Drogon)

# 若你的 Drogon 编译时未把 MySQL 客户端带进 Drogon::Drogon，可解除下一行：
# find_package(MySQL REQUIRED)
# target_link_libraries(seckill-cpp PRIVATE MySQL::MySQLClient)
