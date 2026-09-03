// 同 IP 注册频控（防批量刷号）
//
// 为什么做这一层：注册接口是"创建账号"的入口。即便验证码改成自签发 / 日志模式
// （不接短信网关，验证码直接进日志由人工核对），攻击者依然可以用一堆手机号
// 从同一个 IP 批量注册。本类用"同 IP 固定窗口内允许的成功注册数"把这个口子堵上——
// 没有它，自签发模式在真实部署下是扛不住批量注册的。
//
// 为什么计数"成功注册数"而非"尝试次数"：
//   计尝试数会被攻击者用错误验证码刷爆配额（DoS 掉正常用户的注册名额）；
//   计成功数才能直接约束"一个 IP 能建几个账号"，这才是真正的威胁面。
//
// 为什么放 Redis 而不是进程内存：与 LoginGuard 同一道理——多实例部署时，
//   进程内存的计数只对单机有效，打散到 N 台实例就形同虚设。Redis 是共享单一计数源。
//
// 为什么 fail-open（Redis 不可用时放行）：频控属"可用性"维度保护，
//   Redis 挂掉时把全部注册都拒掉，业务损失比被刷号更大。鉴权要收紧（宁可误杀），
//   限流要放松（宁可放过），二者不能混为一谈（详见 LoginGuard 注释）。
#pragma once

#include <drogon/nosql/RedisClient.h>

#include <cstdint>
#include <functional>
#include <string>

namespace seckill::auth {

// 同 IP 注册频控：固定窗口内允许的成功注册次数。
class RegisterGuard {
public:
    RegisterGuard(drogon::nosql::RedisClientPtr redis, int maxPerIp, int windowSeconds);

    // 闸门：当前 IP 是否还能注册。cb(allowed)
    //   allowed=true  → 未达上限，放行
    //   allowed=false → 已达上限，应返回 429
    // Redis 不可用时 fail-open：放行（宁可放过，不可误杀正常注册）。
    void check(const std::string &ip, std::function<void(bool)> &&cb);

    // 注册成功后调用：计数 +1，首次设置窗口 TTL（固定窗口语义）。
    // fire-and-forget：cb 仅用于收尾，可省略（默认空回调）。
    void markSuccess(const std::string &ip, std::function<void()> &&cb = {});

    int maxPerIp() const { return maxPerIp_; }
    int windowSeconds() const { return windowSeconds_; }

    static std::string keyFor(const std::string &ip) { return "reg:ip:" + ip; }

private:
    drogon::nosql::RedisClientPtr redis_;
    int maxPerIp_;
    int windowSeconds_;
};

}  // namespace seckill::auth
