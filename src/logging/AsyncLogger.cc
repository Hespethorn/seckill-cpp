#include "logging/AsyncLogger.h"

#include <filesystem>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace seckill::log {

AsyncLogger &AsyncLogger::instance() {
    static AsyncLogger inst;  // magic static：进程级单例，首次调用线程安全
    return inst;
}

void AsyncLogger::start(const std::string &file, spdlog::level::level_enum level) {
    if (running_.exchange(true)) {
        return;  // 已启动，幂等
    }
    // 先记下级别：enabled() 在业务线程靠它做短路，必须早于任何 log() 可见
    level_.store(static_cast<int>(level), std::memory_order_relaxed);

    try {
        // spdlog 的 file sink 不会自动建目录，先补上（如 ./logs）。
        const std::filesystem::path p(file);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
        // 主 sink：按大小轮转（5MB × 3 个文件）。落盘只发生在后台线程。
        auto fileSink =
            std::make_shared<spdlog::sinks::rotating_file_sink_mt>(file, 5 * 1024 * 1024, 3);
        fileLogger_ = std::make_shared<spdlog::logger>("seckill", std::move(fileSink));
        fileLogger_->set_level(level);
        // warn 及以上立即 flush：关键时刻的日志不能留在缓冲区里等凑批。
        fileLogger_->flush_on(spdlog::level::warn);
    } catch (const spdlog::spdlog_ex &e) {
        // 打不开日志文件（如目录无写权限）时退化为 stderr，保证日志不丢。
        fileLogger_ = spdlog::stderr_color_mt("seckill_fallback");
        fileLogger_->error("AsyncLogger: fallback to stderr, {}", e.what());
    }

    worker_ = std::thread([this] { workerLoop(); });
}

void AsyncLogger::shutdown() {
    if (!running_.exchange(false)) {
        return;
    }
    buf_.requestStop();
    if (worker_.joinable()) {
        worker_.join();  // 等缓冲排空、剩余消息写完
    }
    if (fileLogger_) {
        fileLogger_->flush();
    }
}

void AsyncLogger::log(spdlog::level::level_enum lvl, std::string msg) {
    if (!running_.load(std::memory_order_relaxed)) {
        return;  // 未启动/已停止：直接丢弃
    }
    // 满则丢（drop-newest）：push 返回 false 时丢弃计数由缓冲内部维护，业务线程不阻塞。
    buf_.push(LogMessage{lvl, std::move(msg)});
}

void AsyncLogger::workerLoop() {
    std::array<LogMessage, kCapacity> batch;
    for (;;) {
        const std::size_t n = buf_.waitAndPopBatch(batch);
        if (n == 0) {
            break;  // 已 stop 且排空
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (fileLogger_) {
                // 用 "{}" 显式占位：把 msg 当参数而不是格式串，避免消息里的
                // 花括号被 fmt 当占位符解析导致 format error。
                fileLogger_->log(batch[i].level, "{}", batch[i].msg);
            }
        }
    }
}

AsyncLogger::~AsyncLogger() {
    shutdown();
}

}  // namespace seckill::log
