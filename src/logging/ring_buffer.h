#pragma once
#include <array>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <utility>

namespace seckill::log {

// 互斥锁版 MPMC 环形缓冲（正确性优先）。
//
// 为什么选「互斥锁 + 条件变量」而不是无锁队列：
//   - 秒杀场景日志级别默认 warn，写入频率低，锁竞争远不是瓶颈；
//   - 无锁队列（SPSC/MPSC 原子队列）的正确性调试成本高一个量级，
//     而日志模块一旦出错会把整条业务链路带崩，得不偿失；
//   - 等压测数据证明「日志本身成了瓶颈」，再换无锁版本，届时用数据说话。
//
// 语义约定：
//   - push：不满即写入；满则丢弃（返回 false）并累计丢弃计数，业务线程绝不阻塞；
//   - waitAndPopBatch：阻塞直到「有数据」或「收到 stop」；一次批量取空，
//     让后台线程一次拿一批、减少唤醒与锁获取次数；
//   - requestStop：置停止位并唤醒，后台线程排空后自然退出。
template <typename T, std::size_t Capacity>
class RingBuffer {
public:
    // 满则丢（drop-newest）：返回 false 表示本次被丢弃。
    bool push(T item) {
        std::lock_guard<std::mutex> lk(mu_);
        if (count_ == Capacity) {
            ++dropped_;
            return false;
        }
        buf_[(head_ + count_) % Capacity] = std::move(item);
        ++count_;
        cv_.notify_one();
        return true;
    }

    // 阻塞直到有数据；返回本次取出的条数。返回 0 仅发生在「已 stop 且队列已空」。
    std::size_t waitAndPopBatch(std::array<T, Capacity> &out) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return count_ > 0 || stop_; });
        std::size_t n = 0;
        while (count_ > 0) {
            out[n] = std::move(buf_[head_]);
            head_ = (head_ + 1) % Capacity;
            --count_;
            ++n;
        }
        return n;
    }

    void requestStop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
    }

    std::size_t dropped() const {
        std::lock_guard<std::mutex> lk(mu_);
        return dropped_;
    }

private:
    std::array<T, Capacity> buf_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::size_t dropped_ = 0;
    bool stop_ = false;
    mutable std::mutex mu_;
    std::condition_variable cv_;
};

}  // namespace seckill::log
