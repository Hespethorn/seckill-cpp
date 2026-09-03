// 缓存 Key 的**唯一构造入口**（第五章 5.1：缓存 Key 设计规范）
//
// 为什么把 key 拼接收敛到一个文件：
//   缓存出问题时最先要回答的是"到底有哪些 key、长什么样、能不能安全删"。
//   如果 key 散落在各业务代码里用字符串硬拼，那么：
//     - 想清理某类缓存只能 `KEYS *sku*`（生产禁用 KEYS）或整库 FLUSHDB（误伤会话/验证码）；
//     - 改一次命名要全局 grep，漏一处就是"两个 key 指同一份数据"的幽灵不一致；
//     - 没人知道某个 key 是否存在，除非读完全部业务代码。
//   集中之后，本文件就是缓存的"目录"，也是压测时 redis-cli 里直接敲的那几个字符串。
//
// Key 模板（四段式，冒号分隔）：
//     <prefix>:<biz>:<version>:<type>[:<id>]
//     seckill:sku:v1:list
//     seckill:sku:v1:item:1001
//     seckill:sku:v1:stock:1001     （阶段四 7.2 启用，当前未使用）
//
// 规范六条（违反任何一条都会在某个时刻变成事故，理由见 docs/CACHE-DESIGN.md）：
//   1. 业务前缀     —— 同 Redis 上还住着 sess: / sms: / login: / reg:ip:，无前缀既没法按类清理，
//                      也会在排查时认不出归属。
//   2. 结构版本号   —— JSON 结构变了直接升 v2，新旧 key 并存到自然过期，
//                      发布瞬间不需要 FLUSHDB（那等于主动制造一次缓存雪崩）。
//   3. 全小写、短   —— key 本身占内存，且 Redis 的 key 是二进制安全但运维靠肉眼。
//   4. 不含易变维度 —— 时间戳、TTL、用户身份绝不能进 key：那会让命中率趋零、内存无上限增长。
//   5. 不含分页参数 —— 列表缓存是全量（LIMIT 100），没有分页就不把 page/size 拼进去；
//                      真要分页时再开 seckill:sku:v1:list:{page}，且必须限死最大页数。
//   6. 冒号分段     —— 运维可用 `SCAN 0 MATCH seckill:sku:v1:item:*` 精确定位一类 key。
#pragma once

#include <cstdint>
#include <string>

namespace seckill::cache {

class CacheKeys {
public:
    CacheKeys(std::string prefix, std::string version)
        : prefix_(std::move(prefix)), version_(std::move(version)) {}

    // 商品列表（全量，不分页）
    std::string list() const { return base() + ":list"; }

    // 单个商品详情
    std::string item(int64_t skuId) const {
        return base() + ":item:" + std::to_string(skuId);
    }

    // 库存计数（阶段四 7.2 预热库存后启用；当前只占位，不读写）
    std::string stock(int64_t skuId) const {
        return base() + ":stock:" + std::to_string(skuId);
    }

    // 运维通配：给 SCAN 用（生产禁用 KEYS，SCAN 才是可增量迭代的）
    std::string itemPattern() const { return base() + ":item:*"; }
    std::string allPattern() const { return base() + ":*"; }

    const std::string &prefix() const { return prefix_; }
    const std::string &version() const { return version_; }

private:
    std::string base() const { return prefix_ + ":sku:" + version_; }

    std::string prefix_;
    std::string version_;
};

}  // namespace seckill::cache
