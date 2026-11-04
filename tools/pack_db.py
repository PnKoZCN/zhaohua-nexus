#!/usr/bin/env python3
"""
把 data/*.h 里的 C 数组打包成二进制数据文件。

用法:
    python3 pack_db.py --out zh_db.bin --ver 3.10.5.301327 ../data/*.h

格式 (ZDB1):
    Header 32 字节:
        0x00  magic  "ZDB1"
        0x04  version (u32)      格式版本 = 1
        0x08  nTables (u32)
        0x0C  strPoolOff (u32)
        0x10  strPoolLen (u32)
        0x14  gameVerOff (u32)   游戏版本字符串偏移
        0x18  gameVerLen (u32)
        0x1C  checksum (u32)     全部表数据的 FNV-1a
    表目录 nTables × 16 字节:
        0x00  type (u32)         见 TABLE_*
        0x04  count (u32)
        0x08  offset (u32)       相对文件头
        0x0C  entrySize (u32)
    字符串池: NUL 分隔
    各表数据: 定长条目，字符串字段存的是池内偏移
"""
import re, struct, sys, os, argparse

TABLE_RETTYPE  = 1
TABLE_BLCALL   = 2
TABLE_CONST    = 3
TABLE_FRAME    = 4
TABLE_SLOT     = 5
TABLE_FIELDACC = 6
TABLE_FUNCINFO = 7
TABLE_CONSTFUNC= 8
TABLE_POOLHINT = 9
TABLE_RETFUNC  = 10
TABLE_RETPOINT = 11

# ---- 极简 C 数组解析 -----------------------------------------------------
def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    s = re.sub(r'//[^\n]*', '', s)
    return s

def find_array(src, name):
    """返回数组体文本（大括号内）。匹配 'name[]' 避免 g_const 命中 g_constfunc"""
    import re as _re
    m = _re.search(_re.escape(name) + r'\s*\[\s*\]', src)
    if not m:
        i = src.find(name)
    else:
        i = m.start()
    if i < 0:
        return None
    j = src.find('{', i)
    if j < 0:
        return None
    depth = 0
    for k in range(j, len(src)):
        c = src[k]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return src[j+1:k]
    return None

def split_entries(body):
    """把 '{a,b,c}, {d,e,f}' 拆成 ['a,b,c', 'd,e,f']"""
    out, depth, cur = [], 0, []
    for c in body:
        if c == '{':
            depth += 1
            if depth == 1:
                cur = []
                continue
        elif c == '}':
            depth -= 1
            if depth == 0:
                out.append(''.join(cur).strip())
                continue
        if depth >= 1:
            cur.append(c)
    return [e for e in out if e]

def parse_value(v):
    v = v.strip()
    if v.startswith('"'):
        return v.strip('"')
    m = re.fullmatch(r'0[xX][0-9a-fA-F]+', v)
    if m:
        return int(v, 16)
    m = re.fullmatch(r'-?\d+', v)
    if m:
        return int(v)
    if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', v):
        return v          # 宏或标识符，按 0 处理
    # {1,2,3} 形式的嵌套，转成列表
    if v.startswith('{'):
        return [parse_value(x) for x in v.strip('{}').split(',')]
    return 0

def flatten(entry):
    """把 'name, 0x123, 0x07, {1,0,0}, "void"' 拆成值列表（嵌套花括号展平）"""
    vals, depth, cur, out = 0, 0, [], []
    for c in entry:
        if c == '{':
            depth += 1
            if depth == 1:
                continue
        elif c == '}':
            depth -= 1
            if depth == 0:
                continue
        if c == ',' and depth == 0:
            out.append(''.join(cur).strip())
            cur = []
            continue
        if depth > 0 or depth == 0:
            cur.append(c)
    if cur:
        out.append(''.join(cur).strip())
    return out

# ---- 字符串池 ------------------------------------------------------------
class StrPool:
    def __init__(self):
        self.buf = bytearray(b'\0')
        self.idx = {}
    def add(self, s):
        if s is None or s == '':
            return 0
        if s in self.idx:
            return self.idx[s]
        off = len(self.buf)
        self.buf += s.encode('utf-8') + b'\0'
        self.idx[s] = off
        return off

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--ver', default='unknown')
    ap.add_argument('files', nargs='+')
    a = ap.parse_args()

    pool = StrPool()
    tables = []   # (type, count, entry_size, bytes)

    for path in a.files:
        src = strip_comments(open(path, encoding='utf-8', errors='replace').read())
        base = os.path.basename(path)

        # rettype_db.h -> g_rettype, N_RETTYPE=124
        if base == 'rettype_db.h':
            body = find_array(src, 'g_rettype')
            if body is None: continue
            ents = split_entries(body)
            buf = bytearray()
            for e in ents:
                f = flatten(e)
                nm = parse_value(f[0]) if f else ''
                off = parse_value(f[1]) if len(f) > 1 else 0
                kind = parse_value(f[2]) if len(f) > 2 else 0
                tname = parse_value(f[3]) if len(f) > 3 else ''
                buf += struct.pack('<IIII', pool.add(nm) if isinstance(nm,str) else 0,
                                   off, kind, pool.add(tname) if isinstance(tname,str) else 0)
            tables.append((TABLE_RETTYPE, len(ents), 16, bytes(buf)))
            print(f"  rettype: {len(ents)} 条")

        elif base == 'blcall_db.h':
            body = find_array(src, 'g_blcall')
            if body is None: continue
            ents = split_entries(body)
            buf = bytearray()
            for e in ents:
                f = flatten(e)
                fn = parse_value(f[0]) if f else ''
                off = parse_value(f[1]) if len(f) > 1 else 0
                tg = parse_value(f[2]) if len(f) > 2 else ''
                toff = parse_value(f[3]) if len(f) > 3 else 0
                buf += struct.pack('<IIII', pool.add(fn) if isinstance(fn,str) else 0,
                                   off, pool.add(tg) if isinstance(tg,str) else 0, toff)
            tables.append((TABLE_BLCALL, len(ents), 16, bytes(buf)))
            print(f"  blcall: {len(ents)} 条")

        elif base == 'fields_real.h':
            body = find_array(src, 'g_fieldacc')
            if body is None: continue
            ents = split_entries(body)
            buf = bytearray()
            for e in ents:
                f = flatten(e)
                fn = parse_value(f[0]) if f else ''
                off = parse_value(f[1]) if len(f) > 1 else 0
                bs  = parse_value(f[2]) if len(f) > 2 else 0
                sz  = parse_value(f[3]) if len(f) > 3 else 0
                wr  = parse_value(f[4]) if len(f) > 4 else 0
                # ★ entrySize=12：4(id)+2(off)+1(base)+1(size)+1(wr)+3(填充)
                #   之前只 pack 了 10 字节（'<IHBBBB'），每条少 2 字节，
                #   从第 2 条起全部错位 → 读端按 12 步进拿到垃圾偏移。
                buf += struct.pack('<IHBBBxxx', pool.add(fn) if isinstance(fn,str) else 0,
                                   off, bs, sz, wr)
            tables.append((TABLE_FIELDACC, len(ents), 12, bytes(buf)))
            print(f"  fieldacc: {len(ents)} 条")

        elif base == 'frame_db.h':
            bs_slot = find_array(src, 'g_slot')
            bs_frame = find_array(src, 'g_frame')
            if bs_slot is None or bs_frame is None: continue
            es = split_entries(bs_slot)
            buf = bytearray()
            for e in es:
                f = flatten(e)
                buf += struct.pack('<HBBI', parse_value(f[0]), parse_value(f[1]) if len(f)>1 else 0,
                                   0, parse_value(f[3]) if len(f)>3 else 0)
            tables.append((TABLE_SLOT, len(es), 8, bytes(buf)))
            print(f"  slot: {len(es)} 条")
            ef = split_entries(bs_frame)
            buf = bytearray()
            for e in ef:
                f = flatten(e)
                fn = parse_value(f[0]) if f else ''
                buf += struct.pack('<IIHBB', pool.add(fn) if isinstance(fn,str) else 0,
                                   parse_value(f[1]) if len(f)>1 else 0,
                                   parse_value(f[2]) if len(f)>2 else 0,
                                   parse_value(f[3]) if len(f)>3 else 0, 0)
            tables.append((TABLE_FRAME, len(ef), 12, bytes(buf)))
            print(f"  frame: {len(ef)} 条")

        elif base == 'const_db.h':
            bf = find_array(src, 'g_constfunc')
            bc = find_array(src, 'g_const')
            if bf is None or bc is None: continue
            fs = split_entries(bf) if '{' in bf else []
            if not fs:
                # g_constfunc 是字符串数组，格式 "a", "b"
                fs = [x.strip() for x in bf.split(',') if x.strip()]
            buf = bytearray()
            for e in fs:
                v = parse_value(e)
                buf += struct.pack('<I', pool.add(v) if isinstance(v,str) else 0)
            tables.append((TABLE_CONSTFUNC, len(fs), 4, bytes(buf)))
            print(f"  constfunc: {len(fs)} 条")
            ec = split_entries(bc)
            buf = bytearray()
            for e in ec:
                f = flatten(e)
                buf += struct.pack('<HBBII', parse_value(f[0]) if f else 0,
                                   parse_value(f[1]) if len(f)>1 else 0,
                                   parse_value(f[2]) if len(f)>2 else 0,
                                   parse_value(f[3]) if len(f)>3 else 0,
                                   parse_value(f[4]) if len(f)>4 else 0)
            tables.append((TABLE_CONST, len(ec), 12, bytes(buf)))
            print(f"  const: {len(ec)} 条")

        elif base == 'ret_db.h':
            br = find_array(src, 'g_retfunc')
            bp = find_array(src, 'g_retpoint')
            if br is None or bp is None: continue
            fs = [x.strip() for x in br.split(',') if x.strip()]
            buf = bytearray()
            for e in fs:
                v = parse_value(e)
                buf += struct.pack('<I', pool.add(v) if isinstance(v,str) else 0)
            tables.append((TABLE_RETFUNC, len(fs), 4, bytes(buf)))
            print(f"  retfunc: {len(fs)} 条")
            ep = split_entries(bp)
            buf = bytearray()
            for e in ep:
                f = flatten(e)
                buf += struct.pack('<HBBI', parse_value(f[0]) if f else 0,
                                   parse_value(f[1]) if len(f)>1 else 0,
                                   0, parse_value(f[3]) if len(f)>3 else 0)
            tables.append((TABLE_RETPOINT, len(ep), 8, bytes(buf)))
            print(f"  retpoint: {len(ep)} 条")

        elif base == 'poolhint_db.h':
            body = find_array(src, 'g_poolhint')
            if body is None: continue
            ents = split_entries(body)
            buf = bytearray()
            for e in ents:
                f = flatten(e)
                buf += struct.pack('<II', parse_value(f[0]), parse_value(f[1]) if len(f)>1 else 0)
            tables.append((TABLE_POOLHINT, len(ents), 8, bytes(buf)))
            print(f"  poolhint: {len(ents)} 条")

    # 组装
    n = len(tables)
    header_len = 32 + n * 16
    strpool = bytes(pool.buf)
    str_off = header_len
    data_off = str_off + len(strpool)

    dirs = bytearray()
    datas = bytearray()
    for t, cnt, esz, b in tables:
        dirs += struct.pack('<IIII', t, cnt, data_off + len(datas), esz)
        datas += b

    ver_bytes = a.ver.encode('utf-8')
    # 布局: header | 目录 | 字符串池 | 表数据 | 版本串
    ver_off = data_off + len(datas)
    # 校验和
    h = 1469598103934665603
    for x in datas:
        h ^= x; h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF

    hdr = struct.pack('<4sIII', b'ZDB1', 1, n, str_off)
    hdr += struct.pack('<II', len(strpool), ver_off)
    hdr += struct.pack('<II', len(ver_bytes), h & 0xFFFFFFFF)
    hdr += b'\0' * (32 - len(hdr))

    out = hdr + bytes(dirs) + strpool + bytes(datas) + ver_bytes
    open(a.out, 'wb').write(out)
    print(f"\n写出 {a.out}: {len(out):,} 字节")
    print(f"  {n} 张表，字符串池 {len(strpool):,} 字节")

if __name__ == '__main__':
    main()
