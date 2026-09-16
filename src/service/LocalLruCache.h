// 5.8 本地 LRU 缓存（自实现）——多级缓存的第一级（L1）
//
// 为什么需要 L1：L2 是 Redis，一次命中也要一次网络往返（约 0.2~0.5ms）。
// 对"同一进程反复读同一批热 key"的流量（洪峰期所有人刷同一个 sku 列表），
// 这个往返是纯开销——把最热的几百上千个 key 放进进程内 LRU，读路径变成
// 一次 hash 查找。三级结构：L1 本地 LRU → L2 Redis → L3 MySQL。
//
// 一致性怎么保（这是本地缓存最被人质疑的点，必须讲清）：
//   - 多实例部署下 L1 无法精确失效（DEL 只到本机）。本项目是单进程演示
//     （threads_num=4 共用同一进程内存），下单失效（invalidate）时**同步删本地**，
//     与 Redis 的 DEL 同时发生，一致性窗口与"只有 Redis"时相同。
//   - 若将来多实例，L1 只放"几乎不变"的数据（商品名/图/上下架状态），
//     stock 这类强实时字段不下 L1——那是阶段四 7.10 拆 stock 计数后的事。
//   - 兜底：L1 条目的 TTL 与 Redis 一致（setex 时传入），过期自动失效，
//     不会出现"Redis 已删、本地永久残留"。
//
// 为什么不引 libcache 之类的库：LRU 的全部内涵是"哈希表 + 双向链表"，
// 标准库就有（unordered_map + list），约 80 行。自实现还能精确控制
// "锁内只做指针搬运、不做大字符串拷贝"这个性能关键点。
//
// 线程安全模型：单 std::mutex 保护整表。读命中的临界区只有
// "hash 查找 + list splice（搬到头部）+ shared_ptr 引用计数自增"，
// 不拷贝 value——value 用 shared_ptr<const string> 共享，锁外再取内容，
// 避免 13KB 的列表 JSON 在锁内 memcpy 拖长临界区。
#pragma once

#include <chrono>
#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace seckill::cache {

class LocalLruCache {
public:
    using Clock = std::chrono::steady_clock;

    explicit LocalLruCache(std::size_t capacity)
        : capacity_(capacity > 0 ? capacity : 1) {}

    // 命中且未过期：内部把条目搬到 LRU 头部（最近使用），返回共享 value。
    // 未命中 / 已过期：返回 nullptr。过期条目顺手清除（惰性淘汰）。
    std::shared_ptr<const std::string> get(const std::string &key) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = map_.find(key);
        if (it == map_.end()) return nullptr;
        if (Clock::now() >= it->second->expireAt) {
            list_.erase(it->second);
            map_.erase(it);
            return nullptr;
        }
        // move 到头部 = "最近使用"。lock 内只做指针搬运，不碰 value 内容。
        list_.splice(list_.begin(), list_, it->second);
        return it->second->value;
    }

    // 写入 / 刷新：新条目放头部；已有条目就地刷新（不重建节点，迭代器仍有效）。
    // 超容量时淘汰尾部（最久未使用）。过期清理是惰性的（get 时顺手删）。
    void put(const std::string &key, std::string value, int ttlSeconds) {
        const auto expireAt = Clock::now() + std::chrono::seconds(ttlSeconds);
        auto sharedValue = std::make_shared<const std::string>(std::move(value));
        std::lock_guard<std::mutex> lk(m_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second->value = std::move(sharedValue);
            it->second->expireAt = expireAt;
            list_.splice(list_.begin(), list_, it->second);
            return;
        }
        list_.emplace_front(Item{key, std::move(sharedValue), expireAt});
        map_[key] = list_.begin();
        if (map_.size() > capacity_) evictTail();
    }

    // 删除（下单失效时与 Redis DEL 同步调用）。
    void del(const std::string &key) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = map_.find(key);
        if (it == map_.end()) return;
        list_.erase(it->second);
        map_.erase(it);
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(m_);
        return map_.size();
    }

private:
    struct Item {
        std::string key;
        std::shared_ptr<const std::string> value;
        Clock::time_point expireAt;
    };
    using List = std::list<Item>;

    void evictTail() {
        // 容量已满且无过期可清时，逐出最久未使用的条目
        const std::string victim = list_.back().key;
        map_.erase(victim);
        list_.pop_back();
    }

    mutable std::mutex m_;
    std::size_t capacity_;
    List list_;                                        // 头部=最近使用
    std::unordered_map<std::string, List::iterator> map_;
};

}  // namespace seckill::cache
