#!/usr/bin/env bash
# 3.11 验证：反代真实 IP 解析（RealIpResolver 插件）
#
# 对应改动：
#   config.json              —— 注册 drogon::plugin::RealIpResolver
#   src/controllers/UserController.cc    —— clientIp() 改用 GetRealAddr()
#   src/controllers/SeckillController.cc —— remote= 日志同步改用插件
#
# 用法（在 WSL 仓库根目录）：
#   bash scripts/verify-311-real-ip.sh
#
# 注：本脚本在 Windows 侧被沙箱安全策略拦截（wsl.exe 在 Program Blacklist），
#     需在 WSL 终端内直接执行。

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

PASS=0
FAIL=0
ok()  { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
bad() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }

echo "=================================================================="
echo " 3.11 反代真实 IP 解析验证（RealIpResolver）"
echo " 仓库：$ROOT"
echo "=================================================================="
echo

# ---------------------------------------------------------------- ① 配置
echo "== ① 静态检查：config.json =="

if grep -q '"drogon::plugin::RealIpResolver"' config.json; then
    ok "已注册 RealIpResolver 插件"
else
    bad "config.json 未找到 RealIpResolver（插件不会加载）"
fi

if grep -q '"trust_ips"' config.json; then
    ok "trust_ips 已配置"
    # 用 JSON 解析器读值。注意：不要用 sed/tr 抓多行 JSON —— CRLF 换行符会把结果搞散。
    # 空数组是合法且常见（直连部署）的取值，必须与"解析失败"区分开，否则会误报失败。
    TRUST="$(python3 -c "import json;c=json.load(open('config.json'));v=[x for p in c.get('plugins',[]) if 'RealIpResolver' in p.get('name','') for x in p.get('config',{}).get('trust_ips',[])];print(' / '.join(v) if v else '（空数组 —— 不信任任何来源的 XFF，直连部署的正确配置）')" 2>/dev/null)"
    if [ -n "$TRUST" ]; then
        echo "        当前值：$TRUST"
    else
        echo "        当前值：（解析失败，请手动查看 config.json 的 plugins 段）"
    fi
    echo "        ↑ 语义：「我信任谁的 X-Forwarded-For」——你声明谁是代理，就等于让那个来源决定 key。"
    echo "          直连 / 无前置反代填 []（本项目当前即如此）；反代与 Drogon 同机填 127.0.0.1，"
    echo "          Docker 网络填网段。两个方向都别填错：填窄了频控全量误伤，填宽了伪造当场生效。"
else
    bad "trust_ips 缺失（插件将不采信任何 X-Forwarded-For）"
fi

if python3 -c "import json,sys; json.load(open('config.json'))" 2>/dev/null; then
    ok "config.json 是合法 JSON"
else
    bad "config.json JSON 语法错误"
fi

# ---------------------------------------------------------------- ② 代码
echo
echo "== ② 静态检查：clientIp() 实现 =="

UI="src/controllers/UserController.cc"

if [ -f "$UI" ]; then
    if grep -q 'RealIpResolver::GetRealAddr' "$UI"; then
        ok "clientIp() 已走官方插件"
    else
        bad "clientIp() 未调用 RealIpResolver::GetRealAddr"
    fi

    if grep -q 'xff.substr(0, xff.find' "$UI"; then
        bad "仍存在「取 X-Forwarded-For 首段」的旧实现（应已删除）"
    else
        ok "旧的手动取首段逻辑已移除"
    fi

    if grep -q '#include <drogon/plugins/RealIpResolver.h>' "$UI"; then
        ok "已 include 插件头文件"
    else
        bad "缺少 #include <drogon/plugins/RealIpResolver.h>"
    fi
else
    bad "找不到 $UI"
fi

if grep -q 'RealIpResolver::GetRealAddr' src/controllers/SeckillController.cc 2>/dev/null; then
    ok "SeckillController 的 remote= 日志已同步改用插件"
else
    echo "  [SKIP] SeckillController 未使用插件（可接受，非必需）"
fi

# ---------------------------------------------------------------- ③ 编译
echo
echo "== ③ 编译 =="

if [ ! -f scripts/build-wsl.sh ]; then
    bad "找不到 scripts/build-wsl.sh"
else
    if bash scripts/build-wsl.sh > /tmp/build-311.log 2>&1; then
        ok "编译通过（完整日志：/tmp/build-311.log）"
    else
        bad "编译失败，末尾 20 行："
        tail -20 /tmp/build-311.log | sed 's/^/        /'
    fi
fi

# ---------------------------------------------------------------- ④ 端到端
echo
echo "== ④ 端到端验证（需服务与 Redis 在跑）=="
echo
echo "  单独跑这个脚本（它自己起服务、两相跑完、自己收尾）："
echo "      bash scripts/e2e-311-real-ip.sh"
echo
echo "  ⚠️ 不要手敲 curl 循环。这里有两个已经踩过的坑："
echo
echo "  ① reg:ip:<ip> 只在【注册真正成功】时才写入（RegisterGuard::markSuccess）。"
echo "     不先把 auth.require_sms_on_register 关掉，请求会止步于 CODE_EXPIRED，"
echo "     Redis 里一条 reg:ip:* 都不会出现 —— 扫出空集，看着\"没问题\"，其实什么都没验证到。"
echo
echo "  ② RealIpResolver 采不采信 XFF，取决于【TCP 对端是否落在 trust_ips 里】"
echo "     （上游 lib/src/RealIpResolver.cc）："
echo "       peer 不在 trust_ips -> 完全不看 XFF，直接用 peerAddr"
echo "       peer 在   trust_ips -> 从右往左扫 XFF，取第一个不可信 IP"
echo "     而本机 curl 的对端恰恰就是 127.0.0.1。若 trust_ips 里写着 127.0.0.1，"
echo "     插件会把你当作\"可信代理\"、进而采信你伪造的 XFF。"
echo "     【这不是漏洞，是配置契约】——trust_ips 的语义就是\"我信任谁的 XFF\"。"
echo
echo "  所以 e2e 脚本分两相跑，把这个契约演示清楚："
echo "     A 相  trust_ips = []          -> 伪造无效，key 恒为 reg:ip:127.0.0.1"
echo "     B 相  trust_ips = [127.0.0.1] -> 伪造生效，出现 reg:ip:10.1.1.{1,2,3}"
echo

# ---------------------------------------------------------------- 汇总
echo "=================================================================="
echo " 静态检查与编译：$PASS 项通过，$FAIL 项失败"
echo "=================================================================="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
