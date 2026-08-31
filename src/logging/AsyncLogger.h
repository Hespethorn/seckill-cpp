#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>

#include "logging/ring_buffer.h"

namespace seckill::log {

// 一条待落盘的日志：级别在业务线程决定，后台线程按级别输出。
struct LogMessage {
    spdlog::level::level_enum level;
    std::string msg;
};

// 异步日志器：业务线程只做「构造字符串 + push 环形缓冲」，落盘集中在后台线程批量做。
//
// 与 Drogon 内置同步日志（trantor，LOG_INFO 等）的区别：
//   - 内置日志在调用线程上同步落盘，日志打得多会直接拖慢 IO 线程；
//   - 本模块业务线程的成本 ≈ 一次内存拷贝 + 一个锁，文件 IO 全部发生在后台线程。
//
// 队列满时的丢弃策略是 drop-newest（丢新消息）：秒杀洪峰期宁可丢日志
// 也不阻塞业务线程，丢弃计数可通过 dropped() 感知，配合监控告警。
class AsyncLogger {
public:
    static constexpr std::size_t kCapacity = 4096;  // 环形缓冲槽位（条）

    static AsyncLogger &instance();

    // 启动：绑定日志文件与级别，拉起后台线程。幂等；须在首次 log() 之前调用。
    // 打不开文件时自动退化为 stderr（日志不丢）。
    void start(const std::string &file, spdlog::level::level_enum level);
    void shutdown();

    // 业务线程入口：非阻塞，满则丢。
    void log(spdlog::level::level_enum lvl, std::string msg);

    // 被丢弃的日志条数（自启动以来累计）。
    std::size_t dropped() const { return buf_.dropped(); }

private:
    AsyncLogger() = default;
    ~AsyncLogger();
    AsyncLogger(const AsyncLogger &) = delete;
    AsyncLogger &operator=(const AsyncLogger &) = delete;

    void workerLoop();

    RingBuffer<LogMessage, kCapacity> buf_;
    std::shared_ptr<spdlog::logger> fileLogger_;
    std::thread worker_;
    std::atomic<bool> running_{false};
};

}  // namespace seckill::log
