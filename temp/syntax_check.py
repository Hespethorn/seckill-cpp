#!/usr/bin/env python3
# 交付前的轻量静态自检：括号/引号平衡、namespace 闭合、include 路径是否存在。
# 不能替代真实编译（本地没有 Drogon），但能挡掉绝大多数低级错误。
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

FILES = []
for dirpath, _, filenames in os.walk(SRC):
    for fn in filenames:
        if fn.endswith((".cc", ".h")):
            FILES.append(os.path.join(dirpath, fn))
FILES.append(os.path.join(ROOT, "src", "main.cc"))

errors = []


def strip_comments_and_strings(text):
    """粗暴地去掉注释和字符串字面量，剩下的括号才是真括号。"""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
        elif c == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
        elif c == "'":
            # 字符字面量（含 C++14 数字分隔符 ' 的情况会误判，本项目没用到）
            i += 1
            while i < n and text[i] != "'":
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


for path in FILES:
    with open(path, "r", encoding="utf-8") as f:
        raw = f.read()
    code = strip_comments_and_strings(raw)
    rel = os.path.relpath(path, ROOT)

    for open_ch, close_ch in [("{", "}"), ("(", ")"), ("[", "]")]:
        if code.count(open_ch) != code.count(close_ch):
            errors.append(
                "%s: 括号不平衡 %s=%d %s=%d"
                % (rel, open_ch, code.count(open_ch), close_ch, code.count(close_ch))
            )

    # namespace 开合数量应一致
    # 名字可选：匿名 namespace（namespace { ... }）同样要计数
    opened = len(re.findall(r"\bnamespace\s*[A-Za-z_][\w:]*\s*\{|\bnamespace\s*\{", raw))
    closed = len(re.findall(r"\}\s*//\s*namespace", raw))
    if opened != closed:
        errors.append("%s: namespace 开=%d 闭=%d（缺少 '}  // namespace xxx' 注释）"
                      % (rel, opened, closed))

    # 本项目内的 #include "xxx" 必须存在
    for m in re.finditer(r'#include\s+"([^"]+)"', raw):
        inc = m.group(1)
        candidates = [
            os.path.join(os.path.dirname(path), inc),
            os.path.join(SRC, inc),
        ]
        if not any(os.path.isfile(c) for c in candidates):
            errors.append("%s: include 找不到 %s" % (rel, inc))

    # 每个头文件都应有 #pragma once（项目约定）
    if path.endswith(".h") and "#pragma once" not in raw:
        errors.append("%s: 头文件缺少 #pragma once" % rel)

print("检查了 %d 个文件" % len(FILES))
if errors:
    print("\n发现问题：")
    for e in errors:
        print("  -", e)
    sys.exit(1)
print("未发现结构性问题（不代表能编译通过，仍需 WSL 真实编译）")
