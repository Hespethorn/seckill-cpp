#include "UserService.h"

#include <drogon/drogon.h>  // drogon::app().getLoop()：PBKDF2 在工作线程算完切回 IO 线程
#include <drogon/orm/Result.h>

#include <thread>

#include "logging/LogStream.h"
#include "password.h"

namespace seckill::auth {
namespace {

std::string describe(const std::exception_ptr &eptr) {
    if (!eptr) return "unknown";
    try {
        std::rethrow_exception(eptr);
    } catch (const std::exception &ex) {
        return ex.what();
    } catch (...) {
        return "non-std exception";
    }
}

// MySQL 唯一键冲突的报错文本（不同客户端库措辞略异，用子串匹配更稳）
bool isDuplicateEntry(const std::string &err) {
    return err.find("Duplicate entry") != std::string::npos ||
           err.find("ER_DUP_ENTRY") != std::string::npos ||
           err.find("uk_phone") != std::string::npos;
}

// 在工作线程里跑 PBKDF2，算完切回 IO 线程。
// 12 万次迭代是纯 CPU 活儿，放 IO 线程会把同线程排队的请求一起拖慢。
void asyncHashPassword(const std::string &password,
                       const std::string &salt,
                       std::function<void(std::string)> &&cb) {
    std::thread([password, salt, cb = std::move(cb)]() {
        std::string hash = pbkdf2Hex(password, salt);
        drogon::app().getLoop()->queueInLoop(
            [cb = std::move(cb), hash = std::move(hash)]() mutable {
                cb(std::move(hash));
            });
    }).detach();
}

}  // namespace

UserService::UserService(drogon::orm::DbClientPtr db,
                         std::shared_ptr<SessionStore> sessions,
                         std::shared_ptr<Jwt> jwt,
                         std::shared_ptr<LoginGuard> guard,
                         std::shared_ptr<RegisterGuard> registerGuard,
                         std::shared_ptr<SmsService> sms,
                         Config cfg)
    : db_(std::move(db)),
      sessions_(std::move(sessions)),
      jwt_(std::move(jwt)),
      guard_(std::move(guard)),
      registerGuard_(std::move(registerGuard)),
      sms_(std::move(sms)),
      cfg_(cfg) {}

void UserService::registerUser(const std::string &phone,
                               const std::string &password,
                               const std::string &code,
                               const std::string &ip,
                               std::function<void(bool, const std::string &)> &&cb) {
    // 同 IP 注册频控闸门：固定窗口内只允许有限个成功注册。
    // 放在最前面——被限的 IP 连手机号格式校验都不该再消耗后续资源。
    // IP 来自控制器（手动解析 X-Forwarded-For 首段，无则取 TCP 对端地址）。
    registerGuard_->check(ip,
        [this, phone, password, code, ip, cb = std::move(cb)](bool allowed) mutable {
            if (!allowed) {
                SK_LOG_WARN << "REGISTER_IP_LIMITED ip=" << ip;
                cb(false, "REGISTER_IP_LIMITED");
                return;
            }

            if (!SmsService::isValidPhone(phone)) {
                cb(false, "INVALID_PHONE");
                return;
            }
            if (static_cast<int>(password.size()) < cfg_.minPasswordLength) {
                cb(false, "WEAK_PASSWORD");
                return;
            }

            // 查重 → 加盐哈希 → 落库，整体抽成一个可延迟执行的闭包：
            // 前面可能要先过一道验证码校验，闭包让"先校验再注册"的时序写起来是直的。
            // ip 一并捕获，注册成功时交给 RegisterGuard 计一次数。
            std::function<void()> doRegister = [this, phone, password, ip, cb]() {
                // 1) 应用层查重（快路径）。真正的兜底是 uk_phone 唯一索引——
                //    两个同手机号请求若在这里竞态都查不到，第二个 INSERT 会撞唯一键。
                //    注意：Drogon 对空结果集走的是成功回调（size()==0），不是异常回调。
                db_->execSqlAsync(
                    "SELECT id FROM user WHERE phone = ?",
                    [this, phone, password, ip, cb](const drogon::orm::Result &r) {
                        if (r.size() > 0) {
                            cb(false, "PHONE_REGISTERED");
                            return;
                        }
                        const std::string salt = genSalt();
                        if (salt.empty()) {
                            SK_LOG_ERROR << "REG_SALT_FAILED phone=" << phone;
                            cb(false, "HASH_FAILED");
                            return;
                        }
                        asyncHashPassword(password, salt,
                            [this, phone, salt, ip, cb](std::string hash) {
                                if (hash.empty()) {
                                    SK_LOG_ERROR << "REG_HASH_FAILED phone=" << phone;
                                    cb(false, "HASH_FAILED");
                                    return;
                                }
                                db_->execSqlAsync(
                                    "INSERT INTO user (phone, password_hash, salt, status, "
                                    "create_time) VALUES (?, ?, ?, 1, NOW())",
                                    [cb, phone, ip, this](const drogon::orm::Result &) {
                                        SK_LOG_INFO << "REGISTER_OK phone=" << phone;
                                        // 注册成功：同 IP 窗口计数 +1（固定窗口 TTL 在
                                        // RegisterGuard 里首次自动设置）。失败什么都不做，
                                        // 因为此时已无新账号产生。
                                        registerGuard_->markSuccess(ip);
                                        cb(true, "OK");
                                    },
                                    [cb, phone](const std::exception_ptr &eptr) {
                                        const std::string err = describe(eptr);
                                        // 唯一键冲突不是系统故障，是"并发下被抢先注册了"，
                                        // 必须映射成业务拒绝码，否则客户端会当成 500 去重试。
                                        if (isDuplicateEntry(err)) {
                                            cb(false, "PHONE_REGISTERED");
                                            return;
                                        }
                                        SK_LOG_ERROR << "REGISTER_FAILED phone=" << phone
                                                     << " err=" << err;
                                        cb(false, "DB_ERROR:" + err);
                                    },
                                    phone, hash, salt);
                            });
                    },
                    [cb, phone](const std::exception_ptr &eptr) {
                        SK_LOG_ERROR << "REGISTER_QUERY_FAILED phone=" << phone
                                     << " err=" << describe(eptr);
                        cb(false, "DB_ERROR:" + describe(eptr));
                    },
                    phone);
            };

            if (!cfg_.requireSmsOnRegister) {
                doRegister();
                return;
            }
            sms_->verifyCode(phone, code,
                [cb, doRegister](SmsService::VerifyResult r) {
                    switch (r) {
                        case SmsService::VerifyResult::Ok:
                            doRegister();
                            return;
                        case SmsService::VerifyResult::EmptyCode:
                            cb(false, "CODE_EXPIRED");
                            return;
                        case SmsService::VerifyResult::WrongCode:
                            cb(false, "CODE_WRONG");
                            return;
                        case SmsService::VerifyResult::TooManyAttempts:
                            cb(false, "CODE_TOO_MANY_ATTEMPTS");
                            return;
                        default:
                            cb(false, "CODE_INVALID");
                            return;
                    }
                });
        });
}

void UserService::login(const std::string &phone,
                        const std::string &password,
                        std::function<void(bool, const std::string &, const std::string &)> &&cb) {
    if (!SmsService::isValidPhone(phone)) {
        cb(false, "", "INVALID_PHONE");
        return;
    }

    // 先问锁定状态：被锁期间连密码都不该去查、去算（省一次 DB + 一次 PBKDF2）
    guard_->checkLocked(phone,
        [this, phone, password, cb](bool locked, int remainSeconds) {
            if (locked) {
                cb(false, "", "ACCOUNT_LOCKED:" + std::to_string(remainSeconds));
                return;
            }

            db_->execSqlAsync(
                "SELECT id, password_hash, salt, status FROM user WHERE phone = ?",
                [this, phone, password, cb](const drogon::orm::Result &r) {
                    if (r.size() == 0) {
                        // 用户不存在时也照常记一次失败：否则攻击者可以用
                        // "哪些手机号存在"来探测用户库（账号枚举）。
                        guard_->onFailure(phone, [cb](const FailResult &) {
                            cb(false, "", "USER_NOT_FOUND");
                        });
                        return;
                    }
                    const auto row = r[0];
                    const int64_t userId = row["id"].as<int64_t>();
                    const std::string storedHash = row["password_hash"].as<std::string>();
                    const std::string salt = row["salt"].as<std::string>();
                    const int status = row["status"].as<int>();
                    if (status != 1) {
                        cb(false, "", "ACCOUNT_DISABLED");
                        return;
                    }

                    asyncHashPassword(password, salt,
                        [this, phone, userId, storedHash, cb](std::string computed) {
                            if (computed.empty()) {
                                cb(false, "", "HASH_FAILED");
                                return;
                            }
                            if (!constantTimeEquals(computed, storedHash)) {
                                guard_->onFailure(phone,
                                    [cb](const FailResult &fr) {
                                        using K = FailResult::Kind;
                                        if (fr.kind == K::Counting) {
                                            // 明确告诉客户端还能试几次：
                                            // 这既是体验，也是给攻击者的明确信号——别撞了。
                                            cb(false, "",
                                               "WRONG_PASSWORD:" + std::to_string(fr.failCount));
                                        } else {
                                            cb(false, "",
                                               "ACCOUNT_LOCKED:" + std::to_string(fr.lockSeconds));
                                        }
                                    });
                                return;
                            }

                            // 登录成功：清零失败计数（失败计数不清锁定标记，
                            // 因为被锁期间根本走不到这里）
                            guard_->onSuccess(phone, [] {});

                            const std::string jti = genJti();
                            const std::string token = jwt_->sign(userId, jti);
                            sessions_->save(jti, userId,
                                [cb, token, userId](bool ok) {
                                    if (!ok) {
                                        cb(false, "", "REDIS_ERROR:session save failed");
                                        return;
                                    }
                                    SK_LOG_INFO << "LOGIN_OK userId=" << userId;
                                    cb(true, token, "OK");
                                });
                        });
                },
                [cb, phone](const std::exception_ptr &eptr) {
                    SK_LOG_ERROR << "LOGIN_QUERY_FAILED phone=" << phone
                                 << " err=" << describe(eptr);
                    cb(false, "", "DB_ERROR:" + describe(eptr));
                },
                phone);
        });
}

void UserService::logout(const std::string &token,
                         std::function<void(bool, const std::string &)> &&cb) {
    JwtClaims claims;
    if (!jwt_->verify(token, claims)) {
        cb(false, "TOKEN_INVALID");
        return;
    }
    // 吊销 = 删掉 Redis 里的权威态。JWT 本身不动（它无法被"改"），
    // 但此后每次鉴权都会因为 EXISTS 失败而被拒。
    sessions_->revoke(claims.jti, [cb](bool deleted) {
        cb(true, deleted ? "OK" : "ALREADY_LOGGED_OUT");
    });
}

void UserService::authenticate(const std::string &token,
                               std::function<void(int64_t)> &&cb) {
    JwtClaims claims;
    if (!jwt_->verify(token, claims)) {
        cb(0);
        return;
    }
    sessions_->exists(claims.jti, [cb, userId = claims.userId](bool alive) {
        cb(alive ? userId : 0);
    });
}

}  // namespace seckill::auth
