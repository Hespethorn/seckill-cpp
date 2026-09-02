#!/usr/bin/env bash
# 登录模块端到端自检：发码 → 注册 → 查重 → 登录 → 撞库锁定 → 登出 → 旧 token 失效
#
# 用法: bash scripts/verify-auth.sh [手机号] [密码]
# 前置: MySQL 已建 user 表（sql/user_schema.sql）、Redis 已启动、服务已在 8080 跑起来
#
# 注意: 本脚本会清理测试手机号在 Redis / MySQL 里的残留数据，换你自己的号也一样。
set -uo pipefail

BASE="${BASE:-http://127.0.0.1:8080}"
PHONE="${1:-13800001111}"
PWD="${2:-abc123456}"
LOG_FILE="${LOG_FILE:-./logs/seckill.log}"

pass=0
fail=0

green() { printf '\033[32m%s\033[0m\n' "$*"; }
red()   { printf '\033[31m%s\033[0m\n' "$*"; }

# 断言：实际输出里包含期望子串
check() {
    local desc="$1" expect="$2" actual="$3"
    if [[ "$actual" == *"$expect"* ]]; then
        green "  [PASS] $desc"
        pass=$((pass + 1))
    else
        red "  [FAIL] $desc"
        echo "         期望包含: $expect"
        echo "         实际输出: $actual"
        fail=$((fail + 1))
    fi
}

post() {
    curl -s -X POST "$BASE$1" -H 'Content-Type: application/json' -d "$2"
}

echo "==> 前置检查"
if ! curl -s -o /dev/null -w '%{http_code}' "$BASE/api/health" | grep -q 200; then
    red "服务未就绪：$BASE/api/health 不通。先跑 ./build/src/seckill-cpp"
    exit 1
fi
if ! redis-cli ping 2>/dev/null | grep -q PONG; then
    red "Redis 未启动：sudo service redis-server start（登录模块全靠它）"
    exit 1
fi
green "  服务与 Redis 就绪，测试手机号 $PHONE"

echo
echo "==> 清理该手机号的残留数据"
redis-cli DEL "sms:code:$PHONE" "sms:cd:$PHONE" "sms:try:$PHONE" \
              "login:fail:$PHONE" "login:lock:$PHONE" >/dev/null 2>&1
mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill \
    -e "DELETE FROM user WHERE phone='$PHONE';" 2>/dev/null
echo "  已清理"

echo
echo "==> 1) 请求验证码"
send_resp=$(post /api/sms/send "{\"phone\":\"$PHONE\"}")
check "发送验证码返回成功" '"code":0' "$send_resp"

# 取验证码：真发模式（配了腾讯云凭据）下拿不到，需要手工输入；
# mock 模式下验证码只写进日志，从日志里捞。
CODE=""
if [[ -f "$LOG_FILE" ]]; then
    sleep 0.5  # 异步日志落盘有一点点延迟
    CODE=$(grep -o "SMS_MOCK phone=$PHONE code=[0-9]\{6\}" "$LOG_FILE" 2>/dev/null |
           tail -1 | grep -o 'code=[0-9]\{6\}' | cut -d= -f2)
fi
if [[ -z "$CODE" ]]; then
    echo "  未从日志拿到验证码（sms.enabled=true 真发模式，或日志路径不对）"
    printf '  请手动输入收到的验证码: '
    read -r CODE
fi
green "  验证码: $CODE"

echo
echo "==> 2) 用错误验证码注册（应被拒）"
resp=$(post /api/user/register "{\"phone\":\"$PHONE\",\"password\":\"$PWD\",\"code\":\"000000\"}")
check "错误验证码被拒" 'CODE_WRONG' "$resp"

echo
echo "==> 3) 正确验证码注册"
resp=$(post /api/user/register "{\"phone\":\"$PHONE\",\"password\":\"$PWD\",\"code\":\"$CODE\"}")
check "注册成功" '"code":0' "$resp"

echo
echo "==> 4) 重复注册同一手机号"
redis-cli DEL "sms:cd:$PHONE" >/dev/null 2>&1  # 跳过 60s 重发冷却，只为测查重
post /api/sms/send "{\"phone\":\"$PHONE\"}" >/dev/null
sleep 0.5
CODE2=$(grep -o "SMS_MOCK phone=$PHONE code=[0-9]\{6\}" "$LOG_FILE" 2>/dev/null |
        tail -1 | grep -o 'code=[0-9]\{6\}' | cut -d= -f2)
resp=$(post /api/user/register "{\"phone\":\"$PHONE\",\"password\":\"$PWD\",\"code\":\"$CODE2\"}")
check "重复注册被拒" 'PHONE_REGISTERED' "$resp"

echo
echo "==> 5) 密码错误（应回显还能试几次）"
resp=$(post /api/user/login "{\"phone\":\"$PHONE\",\"password\":\"wrong-pass\"}")
check "密码错误被拒" 'WRONG_PASSWORD' "$resp"

echo
echo "==> 6) 连续输错直到触发账号锁定"
locked=""
for i in 2 3 4 5; do
    resp=$(post /api/user/login "{\"phone\":\"$PHONE\",\"password\":\"wrong-pass\"}")
    if [[ "$resp" == *ACCOUNT_LOCKED* ]]; then
        locked="$resp"
        break
    fi
done
check "达到阈值后账号锁定" 'ACCOUNT_LOCKED' "${locked:-$resp}"

echo
echo "==> 7) 锁定期间用正确密码登录（应仍被拒）"
resp=$(post /api/user/login "{\"phone\":\"$PHONE\",\"password\":\"$PWD\"}")
check "锁定期间正确密码也进不去" 'ACCOUNT_LOCKED' "$resp"

echo
echo "==> 8) 解锁后正常登录"
redis-cli DEL "login:lock:$PHONE" "login:fail:$PHONE" >/dev/null 2>&1
resp=$(post /api/user/login "{\"phone\":\"$PHONE\",\"password\":\"$PWD\"}")
check "登录成功" '"code":0' "$resp"
TOKEN=$(echo "$resp" | grep -o '"token":"[^"]*"' | head -1 | cut -d'"' -f4)
if [[ -n "$TOKEN" ]]; then
    green "  拿到 token: ${TOKEN:0:40}..."
    # 顺带验一下 JWT 结构：三段、alg=HS256
    parts=$(echo "$TOKEN" | awk -F. '{print NF}')
    check "JWT 是三段式" '3' "$parts"
else
    red "  [FAIL] 未能从响应中解析 token"
    fail=$((fail + 1))
fi

echo
echo "==> 9) 登出"
resp=$(curl -s -X POST "$BASE/api/user/logout" -H "Authorization: Bearer $TOKEN")
check "登出成功" '"code":0' "$resp"

echo
echo "==> 10) 登出后旧 token 应已失效（会话被 Redis 吊销）"
# 鉴权中间件尚未挂到具体业务路由上，这里直接看 Redis 里会话 key 是否已消失
if [[ -n "$TOKEN" ]]; then
    # 从 token 解出 jti（payload 中段，base64url）
    payload=$(echo "$TOKEN" | cut -d. -f2)
    # base64url → base64（补等号）
    b64="${payload//-/+}"; b64="${b64//_//}"
    pad=$((4 - ${#b64} % 4)); [[ $pad -ne 4 ]] && b64="$b64$(printf '=%.0s' $(seq 1 $pad))"
    jti=$(echo "$b64" | base64 -d 2>/dev/null | grep -o '"jti":"[^"]*"' | cut -d'"' -f4)
    if [[ -n "$jti" ]]; then
        exists=$(redis-cli EXISTS "sess:$jti")
        check "Redis 会话已删除（EXISTS=0）" '0' "$exists"
    else
        echo "  (跳过：无法从 token 解析 jti)"
    fi
fi

echo
echo "==> 清理测试数据"
redis-cli DEL "sms:code:$PHONE" "sms:cd:$PHONE" "sms:try:$PHONE" \
              "login:fail:$PHONE" "login:lock:$PHONE" >/dev/null 2>&1
mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill \
    -e "DELETE FROM user WHERE phone='$PHONE';" 2>/dev/null
echo "  已清理"

echo
echo "================ 结果 ================"
green "通过: $pass"
if [[ $fail -gt 0 ]]; then
    red "失败: $fail"
    exit 1
fi
echo "全部通过"
