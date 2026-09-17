#!/usr/bin/env bash
# 3.11 端到端验证：伪造 X-Forwarded-For 到底能不能换掉"同 IP 注册频控"的 key
#
# 用法（WSL 仓库根目录）：
#   bash scripts/e2e-311-real-ip.sh
#
# 前置：
#   sudo service mysql start && sudo service redis-server start
#   bash scripts/build-wsl.sh          # 确保 build/src/seckill-cpp 是最新的
#
# ============================================================================
# 为什么必须用脚本跑，而不是手敲几条 curl
# ============================================================================
#
# ① reg:ip:<ip> 这个 key **只在注册真正成功时写入**（RegisterGuard::markSuccess）。
#    所以必须先把 config.json 的 auth.require_sms_on_register 关掉。
#    否则请求止步于 CODE_EXPIRED，Redis 里一条 reg:ip:* 都不会出现 ——
#    扫出来是空集，看着"没问题"，其实什么都没验证到。（手敲版第一次就踩了这个。）
#
# ② RealIpResolver 采不采信 X-Forwarded-For，取决于 **TCP 对端是否落在 trust_ips 里**
#    （见上游 lib/src/RealIpResolver.cc）：
#
#      peer 不在 trust_ips  ->  完全不看 XFF，直接用 peerAddr
#      peer 在   trust_ips  ->  从右往左扫 XFF，跳过不可解析的与可信的，
#                               取第一个不可信 IP
#
#    而本机 curl 的对端恰恰就是 127.0.0.1。若 trust_ips 里写着 127.0.0.1，
#    插件会把你当作"可信代理"、进而采信你伪造的 XFF。
#
#    **这不是漏洞，是配置契约** —— trust_ips 的语义就是"我信任谁的 XFF"：
#    你声明谁是代理，就等于让那个来源决定频控 key。
#    所以脚本分两相跑，把这个契约显式演示出来：
#
#      A 相  trust_ips = []            -> 伪造 XFF 无效，key 恒为 127.0.0.1
#                                          （直连部署、前置无反向代理时的正确配置）
#      B 相  trust_ips = ["127.0.0.1"] -> 伪造 XFF 生效，出现 10.1.1.x 三个 key
#                                          （只有真有同机反向代理时才该这么配）
#
#    两相一对比，"填错 trust_ips 会不会放大风险"这个问题就有了可复现的答案。
#
# ③ 全程自己起服务、自己收尾：无论成败都还原 config.json（并逐字节校验）并停掉服务。
#    改配置只走 scripts/patch-config.py 原位替换，不重排文件（保住 CRLF 与中文注释）。
# ============================================================================
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

ROOT="$PWD"
CONF="$ROOT/config.json"
BAK="$ROOT/config.json.e2e311.bak"
BIN="$ROOT/build/src/seckill-cpp"
PATCH="$ROOT/scripts/patch-config.py"
BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"
SVC_LOG="$ROOT/logs/e2e-311-service.log"
PROC_PAT='build/src/seckill-cpp'

PASS=0
FAIL=0
PHONES=""

ok()  { echo "    [PASS] $1"; PASS=$((PASS + 1)); }
bad() { echo "    [FAIL] $1"; FAIL=$((FAIL + 1)); }
hr()  { echo "=================================================================="; }

# ---------------------------------------------------------------- 收尾
cleanup() {
    echo
    echo "==> 收尾"

    pkill -f "$PROC_PAT" 2>/dev/null || true
    sleep 1
    if pgrep -f "$PROC_PAT" >/dev/null 2>&1; then
        pkill -9 -f "$PROC_PAT" 2>/dev/null || true
        sleep 1
    fi

    # 本次产生的 reg:ip:* 有 1h TTL，留着会干扰后续测试
    local k
    k=$(redis-cli --scan --pattern 'reg:ip:*' 2>/dev/null | tr '\n' ' ')
    if [ -n "$k" ]; then
        redis-cli DEL $k >/dev/null 2>&1 || true
        echo "    已清理 reg:ip:* ：$k"
    fi
    echo "    服务已停止"

    if [ -f "$BAK" ]; then
        cp -f "$BAK" "$CONF"
        if cmp -s "$BAK" "$CONF"; then
            echo "    config.json 已还原（与备份逐字节一致）"
        else
            echo "    ⚠️  config.json 还原后与备份不一致，请手动检查！" >&2
        fi
        rm -f "$BAK"
    fi

    if [ -n "$PHONES" ]; then
        echo
        echo "    本次测试在库中新增了这些账号（如不想留，执行）："
        echo "      mysql -h127.0.0.1 -useckill -pseckill seckill -e \\"
        echo "        \"DELETE FROM user WHERE phone IN (${PHONES%,});\""
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

stop_service() {
    pkill -f "$PROC_PAT" 2>/dev/null || true
    local i
    for i in $(seq 1 20); do
        pgrep -f "$PROC_PAT" >/dev/null 2>&1 || return 0
        sleep 0.5
    done
    pkill -9 -f "$PROC_PAT" 2>/dev/null || true
    sleep 1
}

start_service() {
    mkdir -p "$ROOT/logs"
    "$BIN" >"$SVC_LOG" 2>&1 &
    local pid=$!
    local i
    for i in $(seq 1 40); do
        if curl -sf "${BASE_URL}/api/health" >/dev/null 2>&1; then
            return 0
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "    服务进程已退出。日志末尾："
            tail -15 "$SVC_LOG" 2>/dev/null | sed 's/^/        /'
            return 1
        fi
        sleep 0.5
    done
    echo "    20s 内未就绪。日志末尾："
    tail -15 "$SVC_LOG" 2>/dev/null | sed 's/^/        /'
    return 1
}

# ---------------------------------------------------------------- 第 0 步
hr
echo " 3.11 端到端验证：伪造 X-Forwarded-For × 同 IP 注册频控"
echo " 仓库：$ROOT"
hr
echo
echo "== 第 0 步：环境自检 =="

if [ -x "$BIN" ]; then
    ok "可执行文件存在：$BIN"
else
    bad "找不到可执行文件 $BIN —— 先跑 bash scripts/build-wsl.sh"
    exit 1
fi

if [ -f "$PATCH" ]; then
    ok "配置补丁工具存在：scripts/patch-config.py"
else
    bad "缺少 scripts/patch-config.py"
    exit 1
fi

if redis-cli ping >/dev/null 2>&1; then
    ok "Redis 可达（redis-cli ping）"
else
    bad "Redis 不可达 —— 先跑 sudo service redis-server start"
    exit 1
fi

if mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill -e 'SELECT 1' >/dev/null 2>&1; then
    ok "MySQL 可达（seckill/seckill@127.0.0.1）"
else
    bad "MySQL 不可达 —— 先跑 sudo service mysql start"
    exit 1
fi

# ---------------------------------------------------------------- 第 1 步
echo
echo "== 第 1 步：备份 config.json，临时关掉注册验证码 =="

cp -f "$CONF" "$BAK"
if cmp -s "$CONF" "$BAK"; then
    ok "已备份到 config.json.e2e311.bak"
else
    bad "备份失败"
    exit 1
fi

if python3 "$PATCH" "$CONF" require_sms_on_register false >/dev/null 2>&1 &&
   grep -q '"require_sms_on_register": false' "$CONF"; then
    ok "auth.require_sms_on_register = false（注册不再需要验证码）"
else
    bad "改写 require_sms_on_register 失败"
    exit 1
fi

echo "    注意：只在本次测试期间关闭，收尾时自动还原。"

# ---------------------------------------------------------------- 相运行
run_phase() {
    local label="$1" trust="$2" pfx="$3" mode="$4"

    echo
    hr
    echo " $label"
    echo " trust_ips = $trust"
    hr

    if ! python3 "$PATCH" "$CONF" trust_ips "$trust" >/dev/null 2>&1; then
        bad "写入 trust_ips 失败"
        return 1
    fi

    stop_service
    if ! start_service; then
        bad "服务未能就绪"
        return 1
    fi
    ok "服务已就绪（$BASE_URL/api/health 通）"

    # 清掉残留，保证观察到的 key 都是本次产生的
    local old
    old=$(redis-cli --scan --pattern 'reg:ip:*' 2>/dev/null | tr '\n' ' ')
    if [ -n "$old" ]; then
        redis-cli DEL $old >/dev/null 2>&1 || true
    fi
    echo "    已清理残留 reg:ip:* ：${old:-（无）}"

    # 发 3 个注册请求，每次伪造不同的 XFF
    local off n_succ=0 i ph resp
    off=$(( $(date +%s) % 90000000 ))
    for i in 1 2 3; do
        ph="${pfx}$(printf '%08d' $((off + i)))"
        PHONES="$PHONES'$ph',"
        resp=$(curl -s -X POST "${BASE_URL}/api/user/register" \
                 -H 'Content-Type: application/json' \
                 -H "X-Forwarded-For: 10.1.1.$i" \
                 -d "{\"phone\":\"$ph\",\"password\":\"pass1234\"}")
        echo "    [$i] XFF=10.1.1.$i  phone=$ph"
        echo "        -> $resp"
        case "$resp" in
            *'"code":0'*) n_succ=$((n_succ + 1)) ;;
        esac
    done

    if [ "$n_succ" -ne 3 ]; then
        bad "3 次注册只成功 $n_succ 次 —— 未成功的先看上面响应体（如上一步漏关验证码会得到 CODE_EXPIRED）"
        return 1
    fi
    ok "3 次注册全部成功（code=0），markSuccess 必然已执行"

    # 看 Redis
    local keys
    keys=$(redis-cli --scan --pattern 'reg:ip:*' 2>/dev/null | sort)
    echo "    Redis 中的 reg:ip:* ："
    if [ -z "$keys" ]; then
        echo "        （空）"
    else
        local k
        while read -r k; do
            [ -n "$k" ] || continue
            echo "        $k  →  计数 $(redis-cli GET "$k" 2>/dev/null)"
        done <<< "$keys"
    fi

    local n_keys=0
    [ -n "$keys" ] && n_keys=$(printf '%s\n' "$keys" | wc -l | tr -d ' ')

    case "$mode" in
    ignore)
        # A 相：对端 127.0.0.1 不在 trust_ips 内 → XFF 应被完全忽略
        if [ "$n_keys" -ne 1 ]; then
            bad "期望恰好 1 个 key，实际 $n_keys 个 —— 伪造的 XFF 不该产生新 key"
            return 1
        fi
        if [ "$keys" = "reg:ip:127.0.0.1" ]; then
            ok "唯一的 key 就是 TCP 对端 127.0.0.1（伪造的 XFF 被彻底忽略）"
        else
            bad "唯一的 key 是 $keys，期望 reg:ip:127.0.0.1"
            return 1
        fi
        local cnt
        cnt=$(redis-cli GET reg:ip:127.0.0.1 2>/dev/null)
        if [ "$cnt" = "3" ]; then
            ok "计数 = 3：3 次注册全部落在同一个 key 上（同一窗口内被我一个人占用）"
        else
            bad "计数 = $cnt，期望 3"
        fi
        if printf '%s\n' "$keys" | grep -q '10\.1\.1\.'; then
            bad "出现了 10.1.1.x 的 key —— 伪造生效了，修复无效"
        else
            ok "没有任何 10.1.1.x 的 key —— 伪造 XFF 完全没起作用"
        fi
        ;;
    honor)
        # B 相：对端 127.0.0.1 在 trust_ips 内 → 插件把我当可信代理，XFF 被采信
        if [ "$n_keys" -ne 3 ]; then
            bad "期望 3 个 key（每跳一个伪造 IP），实际 $n_keys 个"
            return 1
        fi
        ok "出现 3 个 key —— 插件采信了「可信代理」发来的 XFF，这正是 trust_ips 的契约"
        expected="reg:ip:10.1.1.1
reg:ip:10.1.1.2
reg:ip:10.1.1.3"
        if [ "$keys" = "$expected" ]; then
            ok "三个 key 恰好是 reg:ip:10.1.1.{1,2,3}，与伪造值一一对应"
        else
            echo "    实际 key：$(printf '%s ' $keys)"
            echo "    期望 key：reg:ip:10.1.1.1 / .2 / .3"
            bad "key 与伪造值不是一一对应"
        fi
        echo
        echo "    ⚠️  这一相不是「修复失败」：它演示的是「谁在 trust_ips 里，谁就能决定 key」"
        echo "        这条契约。脚本从本机发起，对端正是 127.0.0.1，而它被声明成了可信代理 ——"
        echo "        所以它的 XFF 被信任了。真实部署里若没有同机反代，trust_ips 应当是 []（即 A 相）。"
        ;;
    esac

    return 0
}

run_phase "A 相：trust_ips = []（无代理 / 直连部署的正确配置）" \
          '[]' '139' ignore

run_phase "B 相：trust_ips = [\"127.0.0.1\"]（声明本机为可信代理）" \
          '["127.0.0.1"]' '137' honor

# ---------------------------------------------------------------- 汇总
echo
hr
echo " 端到端验证：$PASS 项通过，$FAIL 项失败"
echo " 说明：两相都应为 PASS。A 相证明「不可信来源的伪造无效」，"
echo "       B 相证明「trust_ips 里的来源其 XFF 会被采信」，合起来才是完整结论。"
echo " 静态检查与编译请另跑：bash scripts/verify-311-real-ip.sh"
hr
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
