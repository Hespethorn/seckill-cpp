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

- [ ] 删除纯调试脚本：`scripts/_dbg.sh`、`scripts/_dbg2.sh`、`scripts/_restart.sh`、`scripts/_restart2.sh`、`scripts/_restart3.sh`
- [ ] 处置验证脚本：`_verify54.sh`（延迟双删验证）、`_verify57.sh`（布隆验证）、`_validate.sh` → 内容有复用价值的**去掉 `_` 前缀转正**为 `scripts/verify-*.sh`；纯一次性调试则删
- [ ] 根目录 `server-*.log`：移入 `logs/`（已被 .gitignore 忽略）或删除
- [ ] `.gitignore` 增补两行防再犯：`/server-*.log` 与 `/scripts/_*.sh`

## 2. 悬置改动验证与提交

- [ ] WSL 里编译绿灯：`bash scripts/build-wsl.sh`（改前确认无 error/warning 新增）
- [ ] 冒烟：`bash scripts/smoke-seckill.sh`（或手动起服务跑一次秒杀链路）
- [ ] DelayDeleter 内联化 diff 自审：析构顺序注释是否仍准确、`#include <thread>/<mutex>` 齐全
- [ ] 通过后提交为一个**收尾 commit**（如 `refactor(cache): DelayDeleter 头文件内联化 + 阶段二文档同步`）

## 3. WSL 实测回填（PLAN.md §5.4 清单）

- [ ] 5.4 延迟双删收益数字（跑 `_verify54.sh` 转正版或手测：删 key 到二次 DEL 的窗口、DB 一致性）
- [ ] 5.7 布隆过滤器收益数字（`_verify57.sh`：拦截率、误判率、对读接口 RT 影响）
- [ ] 5.8 本地 L1（自实现 LRU）收益数字（`scripts/local-bench.sh`：命中率、RT、对比纯 L2）
- [ ] 回填 PLAN.md §5.4 清单 + 阶段二状态行改为「**已实测**」

## 4. 博客 5.4~5.8 补齐（自动化已保活：周一/四 10:00）

- [ ] 自动化推进到 5.4 时**必须先完成第 3 步实测**，禁止编数字；数字没齐就让自动化停下提示
- [ ] 5.4 延迟双删 / 5.5 缓存预热 / 5.6（如规划有）/ 5.7 布隆 / 5.8 多级缓存 L1，每篇对齐仓库真实代码路径 + 实测数据
- [ ] 每篇带「功能抉择」小节 + 裸内联 SVG（铁律见博客 BLOG-MAINLINES-2026.md）

## 5. 开源库打磨（收官 polish）

- [ ] README：特性补全阶段二（缓存层已落地项）、badges（CI/构建状态）、目录导览、与博客章节对照表
- [ ] 可选：GitHub CI 工作流（Ubuntu 上编译冒烟，需能装 Drogon 依赖）
- [ ] LICENSE 文件（MIT 等，需老周确认）
- [ ] 打收尾 tag（如 `v0.2.0`）——**需老周显式批准**（平时不打 tag 惯例，收官可破例一次）

## 6. PLAN.md 定格

- [ ] 阶段三/四行状态改为「**归档：不再排期（2026-09-09 收尾决定）**」或加醒目注记
- [ ] 阶段一「收尾中」→「已完成」；阶段二 →「**收官（v0.2.x，代码+博客全对齐）**」
- [ ] ADR 表补一条收尾决定记录

## 7. 最终发布

- [ ] 全部 commit 后，老周在对话里说「发布」→ 才 push 到 GitHub
- [ ] 博客 Seckill 系列对应草稿说「发布」→ 才 commit+push 到博客 source 分支

## 备注

- 本清单文件**不入最终版本库**（或收尾完成后删除），它是过程文档。
- 「重新熟悉从前业务」的打开方式：README 快速开始 → PLAN.md 阶段决策 → 各章博客 → 对应 commit 读 diff。
