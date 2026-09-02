add_executable(seckill-cpp
    main.cc
    controllers/HealthController.cc
    controllers/SeckillController.cc
    controllers/UserController.cc
    controllers/SmsController.cc
    service/SeckillService.cc
    service/password.cc
    service/Jwt.cc
    service/SessionStore.cc
    service/LoginGuard.cc
    service/SmsSender.cc
    service/SmsService.cc
    service/UserService.cc
    logging/AsyncLogger.cc
)

# 头文件搜索根放在 src/，这样 controller/service 互相 include 时用 "service/SeckillService.h" 即可。
target_include_directories(seckill-cpp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 依赖说明：
#   OpenSSL —— 密码哈希 PBKDF2（password.cc）、JWT 的 HMAC-SHA256（Jwt.cc）。
#   验证码送达已改为自签发 / 日志模式，不再依赖 libcurl，也不再引入短信网关 SDK。
# 没有引入的：jwt-cpp（自实现 HS256）、redis-plus-plus（改用 Drogon 内置异步客户端）。
#   理由见各文件头部注释。
find_package(OpenSSL REQUIRED)

target_link_libraries(seckill-cpp PRIVATE Drogon::Drogon)
target_link_libraries(seckill-cpp PRIVATE spdlog::spdlog)
target_link_libraries(seckill-cpp PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# 若你的 Drogon 编译时未把 MySQL 客户端带进 Drogon::Drogon，可解除下一行：
# find_package(MySQL REQUIRED)
# target_link_libraries(seckill-cpp PRIVATE MySQL::MySQLClient)
