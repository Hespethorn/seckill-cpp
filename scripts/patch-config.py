#!/usr/bin/env python3
"""原位替换 config.json 里某个键的值，不改动文件其余任何字节。

为什么不用 json.load + json.dump：
    config.json 是 CRLF 换行，且 plugins[].config._note 里带一长串中文说明。
    json.dump 会把整个文件重排成 LF + 统一缩进 —— 一次配置微调就产生几十行
    无意义的 git diff，回看历史时根本分不清"真正改了哪一行"。

实现：纯字符串定位（不用正则），找到 "key" 之后的 ':'，再按值的第一个字符判断形态：
    '['  -> 括号配对，取到与之匹配的 ']'
    其他 -> 标量，取到本行行尾或下一个 ',' 为止
只覆盖"字符串数组"与"标量"两种，够本项目用；不处理嵌套对象。

用法：
    python3 scripts/patch-config.py config.json trust_ips '[]'
    python3 scripts/patch-config.py config.json trust_ips '["127.0.0.1"]'
    python3 scripts/patch-config.py config.json require_sms_on_register false
"""
import json
import sys

WS = ' \t\r\n'


def value_span(raw, key):
    """返回 raw 中该 key 的值所占的 [start, end) 区间。"""
    needle = '"' + key + '"'
    k = raw.find(needle)
    if k < 0:
        raise KeyError('key not found: ' + key)
    c = raw.index(':', k) + 1
    i = c
    while i < len(raw) and raw[i] in WS:
        i += 1
    if i >= len(raw):
        raise ValueError('no value after key: ' + key)
    if raw[i] == '[':
        depth = 0
        j = i
        while j < len(raw):
            if raw[j] == '[':
                depth += 1
            elif raw[j] == ']':
                depth -= 1
                if depth == 0:
                    return i, j + 1
            j += 1
        raise ValueError('unbalanced [ for key: ' + key)
    j = i
    while j < len(raw) and raw[j] not in ',\r\n':
        j += 1
    e = j
    while e > i and raw[e - 1] in ' \t':
        e -= 1
    return i, e


def main(argv):
    if len(argv) != 4:
        sys.stdout.write(__doc__)
        return 2
    path, key, repl = argv[1], argv[2], argv[3]
    json.loads(repl)  # repl 必须是合法 JSON 片段

    # newline='' —— 不做通用换行转换，原样保留 CRLF
    with open(path, 'r', encoding='utf-8', newline='') as f:
        raw = f.read()

    a, b = value_span(raw, key)
    new = raw[:a] + repl + raw[b:]

    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(new)

    # 写回后整体再验一次，避免改坏
    with open(path, 'r', encoding='utf-8') as f:
        json.load(f)

    print('OK  ' + key + ' -> ' + repl)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
