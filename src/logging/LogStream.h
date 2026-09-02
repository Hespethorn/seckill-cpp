#pragma once
#include <sstream>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "logging/AsyncLogger.h"

namespace seckill::log {

// 流式日志：把 `<<` 拼出来的内容暂存进 ostringstream，临时对象析构时
// 一次性 push 进 AsyncLogger 的环形缓冲。
//
// 设计要点：
//   - 业务线程的成本 = 一次字符串拼接 + 一次环形缓冲 push，文件 IO 全在后台线程；
//   - 析构即提交，所以必须配合 SK_LOG_* 宏使用（临时对象在语句结束时析构）；
//   - 禁止拷贝/移动：日志对象必须是语句内的临时量，不能被存下来延迟提交。
class LogStream {
public:
    explicit LogStream(spdlog::level::level_enum lvl) : lvl_(lvl) {}

    ~LogStream() {
        // 析构即提交。注意这里不做级别判断——判断在宏里已经做过了（短路）。
        AsyncLogger::instance().log(lvl_, oss_.str());
    }

    LogStream(const LogStream &) = delete;
    LogStream &operator=(const LogStream &) = delete;
    LogStream(LogStream &&) = delete;
    LogStream &operator=(LogStream &&) = delete;

    template <typename T>
    LogStream &operator<<(T &&v) {
        oss_ << std::forward<T>(v);
        return *this;
    }

private:
    spdlog::level::level_enum lvl_;
    std::ostringstream oss_;
};

}  // namespace seckill::log

// ── 日志宏 ────────────────────────────────────────────────────────────────
//
// 用法（与 Drogon 内置 LOG_* 风格一致，都是流式）：
//     SK_LOG_WARN  << "SOLD_OUT skuId=" << skuId;
//     SK_LOG_ERROR << "DB_ERROR " << ex.what() << " userId=" << userId;
//
// 两个关键设计：
//
// 1) 命名带 SK_ 前缀：Drogon / trantor 已经提供了全局的 LOG_INFO / LOG_WARN /
//    LOG_ERROR 等宏，若业务侧再定义同名宏会直接重定义冲突。SK_ 前缀既避免冲突，
//    又一眼能区分"走异步通道的日志"和"Drogon 框架自己的同步日志"。
//
// 2) 级别短路：先问 AsyncLogger::enabled(lvl)，不打就直接跳过整条语句，
//    连字符串都不拼。秒杀洪峰期这一句能省掉大量"注定被过滤"的字符串拼接开销。
//    用 if (...) {} else 的形式包裹，避免宏在 if/else 场景下出现 dangling else。
//
#define SK_LOG(level)                                                     \
    if (!::seckill::log::AsyncLogger::instance().enabled(level)) {        \
    } else                                                                \
        ::seckill::log::LogStream(level)

#define SK_LOG_TRACE    SK_LOG(spdlog::level::trace)
#define SK_LOG_DEBUG    SK_LOG(spdlog::level::debug)
#define SK_LOG_INFO     SK_LOG(spdlog::level::info)
#define SK_LOG_WARN     SK_LOG(spdlog::level::warn)
#define SK_LOG_ERROR    SK_LOG(spdlog::level::err)
#define SK_LOG_CRITICAL SK_LOG(spdlog::level::critical)
