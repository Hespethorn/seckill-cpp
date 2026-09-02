cmake_minimum_required(VERSION 3.16)
project(seckill-cpp LANGUAGES CXX)

# 统一用 C++17：Drogon 的回调式 orm 在 17 下即可编译，
# 不依赖 C++20 协程（协程版的 commit() 返回 Task，API 形态不同，见 README 取舍小节）。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Drogon 提供 Drogon::Drogon target；其 orm 模块在启用 MySQL 时自动链入 mysqlclient。
find_package(Drogon REQUIRED)

# 异步日志依赖（self 实现的环形缓冲只负责排队，落盘 sink 复用 spdlog）。
find_package(spdlog REQUIRED)

# 登录模块依赖：OpenSSL 负责 PBKDF2 哈希 / JWT 的 HMAC-SHA256 / 腾讯云 TC3 签名，
# libcurl 负责短信 HTTP 请求。两者都是 Drogon 的既有底层依赖，不额外增加部署负担。
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

add_subdirectory(src)
