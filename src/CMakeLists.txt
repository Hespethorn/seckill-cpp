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

# 依赖说明（本阶段引入的最后两个外部库）：
#   OpenSSL —— 密码哈希 PBKDF2（password.cc）、JWT 的 HMAC-SHA256（Jwt.cc）、
#              腾讯云 TC3 签名的 HMAC 链（SmsSender.cc）。
#   libcurl —— 短信 HTTP 请求（SmsSender.cc）。
#   两者都是 Drogon / hiredis 的底层依赖，本来就在系统里，不算新增负担。
# 没有引入的：jwt-cpp（自实现 HS256）、redis-plus-plus（改用 Drogon 内置异步客户端）、
#             tencentcloud-sdk-cpp（自实现 TC3 签名）。理由见各文件头部注释。
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

target_link_libraries(seckill-cpp PRIVATE Drogon::Drogon)
target_link_libraries(seckill-cpp PRIVATE spdlog::spdlog)
target_link_libraries(seckill-cpp PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(seckill-cpp PRIVATE CURL::libcurl)

# 若你的 Drogon 编译时未把 MySQL 客户端带进 Drogon::Drogon，可解除下一行：
# find_package(MySQL REQUIRED)
# target_link_libraries(seckill-cpp PRIVATE MySQL::MySQLClient)
