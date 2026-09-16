# seckill-cpp 收尾检查清单（SEAL-CHECKLIST）

> 2026-09-09 老周拍板：**博客系列最后写完 + 仓库打磨成正式开源库后收官**。
> 范围理解：代码定格**阶段二（v0.2.x，5.1~5.8 已实现）**，博客补到第五章完；阶段三（MQ）/四（微服务）**不再推进**（除非改主意，先改本文件再动）。
> **2026-09-16 复确认**：中间曾一度决定"继续推进阶段三/四直到 v1.0.0"，当日即撤回，
> **维持 09-09 原决定**——阶段三/四归档，代码定格阶段二，本项目以 `v0.2.0` 收官。
> 阶段六/七/八的章节规划保留在博客 Master Plan 中作为"未来可续"的记录，但**不排期、不实现**。
> 目标用途：作为「重新熟悉从前业务」的完整可复习标本 + 拿得出手的开源库。
> 铁律：所有代码改动先 WSL 编译绿灯再 commit；不打 tag 不 push，发布需老周显式批准。

## 0. 现状快照（2026-09-09 17:20）

- 仓库：`D:\GitHub\seckill-cpp` → GitHub `Hespethorn/seckill-cpp`（source 分支体系）
- 最近提交：缓存阶段二 5.4~5.8 实现（`cc5f557`/`e4ce2d4`/`d764a4c`/`4a0c246`）
- PLAN.md 状态：阶段一收尾中；阶段二**代码完成**，5.4/5.7/5.8 收益数字**待 WSL 实测回填**（§5.4 清单）；博客 5.1~5.3 已写、5.4~5.8 待产出
- 工作区未提交改动（7 文件，+137/−93）：
  - `README.md`、`config.json`、`docs/PLAN.md`、`src/main.cc`、`src/service/LocalLruCache.h`
  - `src/service/SkuCache.{h,cc}`：DelayDeleter 从前向声明改为**头文件内联 class**（实现从 .cc 移入 .h）
- 调试残留（未跟踪，gitignore 未覆盖）：`scripts/_dbg.sh` `_dbg2.sh` `_restart.sh` `_restart2.sh` `_restart3.sh` `_validate.sh` `_verify54.sh` `_verify57.sh` + 根目录 `server-*.log` ×6（54/bloom/clean/dbg2/debug/default/ttl）

## 1. 清理调试残留（先做，独立于编译）

- [x] 删除纯调试脚本：`scripts/_dbg.sh`、`_dbg2.sh`、`_restart.sh`、`_restart2.sh`、`_restart3.sh`
- [x] 处置验证脚本：`_verify54.sh` → **`scripts/verify-54-double-delete.sh`**、`_verify57.sh` → **`scripts/verify-57-bloom.sh`**（转正 + 修掉原脚本提取 stats 的正则 bug）；`_validate.sh` 属一次性调试，已删
- [x] 根目录 `server-*.log` ×7：已删除
- [x] `.gitignore` 增补四行：`/server-*.log`、`/*.server.log`、`/scripts/_*.sh`、`/temp/`

## 2. 悬置改动验证与提交

- [x] DelayDeleter 内联化 diff 自审：`.h` 内已补全 `#include <chrono>/<condition_variable>/<deque>/<mutex>/<thread>`，代码逐字搬迁、行为零改动
- [x] 附带 `main.cc` 真 bug 修复：布隆配置读取误用 `c` 改为 `cc`（此前 `bloom_*` 三项读不到、恒走默认值）
- [x] 提交：`7b3ee16 chore: 阶段二收尾清理——调试残留转正/删除 + DelayDeleter 内联化 + main.cc 配置 bug 修复`
- [x] **WSL 编译绿灯**（2026-09-16）：`bash scripts/build-wsl.sh` → `[17/17] Linking CXX executable src/seckill-cpp`，构建产物 `./build/src/seckill-cpp`。`main.cc` 的 `cc` 修复与 `SkuCache` 内联化均编译通过（增量 17 步）。
- [ ] 冒烟：`bash scripts/smoke-seckill.sh`（**待老周执行**）

## 3. WSL 实测回填（PLAN.md §5.4 清单）

- [ ] 5.4 延迟双删（`bash scripts/verify-54-double-delete.sh`，脚本自动开关配置、退出时 trap 恢复）
- [ ] 5.7 布隆过滤器（`bash scripts/verify-57-bloom.sh`：拦截率、误判率、对读接口 RT 影响）
- [ ] 5.8 本地 L1（自实现 LRU）收益数字（`bash scripts/local-bench.sh`：命中率、RT、对比纯 L2）
- [ ] 回填 PLAN.md §5.4 清单 + 阶段二状态行改为「**已实测**」

## 4. 博客 5.4~5.8 补齐（自动化已保活：周一/四 10:00）

- [ ] 自动化推进到 5.4 时**必须先完成第 3 步实测**，禁止编数字；数字没齐就让自动化停下提示
- [ ] 5.4 延迟双删 / 5.5 缓存预热 / 5.6（如规划有）/ 5.7 布隆 / 5.8 多级缓存 L1，每篇对齐仓库真实代码路径 + 实测数据
- [ ] 每篇带「功能抉择」小节 + 裸内联 SVG（铁律见博客 BLOG-MAINLINES-2026.md）

## 5. 开源库打磨（收官 polish）

- [x] README：特性按阶段一/二分组补全（缓存四项增强 + 实测数字）、进度表注明阶段三/四归档、项目结构与脚本清单更新、LICENSE 段
- [ ] 可选：GitHub CI 工作流（Ubuntu 上编译冒烟，需能装 Drogon 依赖）——**暂不做**：Drogon 源码编译耗时长（十几分钟/次），收益不抵 CI 成本，改为在本机 WSL 保证绿灯
- [x] LICENSE 文件（MIT，Copyright 2026 Hespethorn）
- [ ] 打收尾 tag `v0.2.0`——**需老周显式批准**（平时不打 tag 惯例，收官破例一次）
  - 注：`v1.0.0` 原先误打在阶段二收尾提交 `47290ea` 上（已推到远端），语义与规划不符——该版本号应属阶段四。本地 tag 已删除；远端 tag 处理见 §7。

## 6. PLAN.md 定格

- [x] 阶段三/四行状态改为「**归档：不再排期**」，并在 §2 顶部加醒目注记
- [x] 阶段一「收尾中」→「**已完成**」；阶段二 →「**收官**」
- [x] ADR 表补 ADR-10（项目收官决定，含三条理由与代价），正文附完整论证
- [x] 第六/七/八章清单加「归档：不再排期」标记

## 7. 最终发布

- [ ] 全部 commit 后，老周在对话里说「发布」→ 才 push 到 GitHub
- [ ] 博客 Seckill 系列对应草稿说「发布」→ 才 commit+push 到博客 source 分支
- [ ] **远端 `v1.0.0` tag 需处理**：它已推到 origin 且指向阶段二收尾（语义错误）。建议方案：
      `git push origin :refs/tags/v1.0.0` 删除远端 tag，收官时改推 `v0.2.0`。
      因涉及远端重写，**需老周明确同意后执行**；若你希望保留 v1.0.0 不动，就在收官文档里注明"该 tag 实为阶段二里程碑"。

## 备注

- 本清单文件**不入最终版本库**（或收尾完成后删除），它是过程文档。
- 「重新熟悉从前业务」的打开方式：README 快速开始 → PLAN.md 阶段决策 → 各章博客 → 对应 commit 读 diff。
