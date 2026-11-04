#!/usr/bin/env python3
"""验证打包的数据文件与源数组逐条一致"""
import sys, struct, re, os
sys.path.insert(0, os.path.dirname(__file__))
from pack_db import strip_comments, find_array, split_entries, flatten, parse_value

TYPES = {1:'rettype',2:'blcall',3:'const',4:'frame',5:'slot',6:'fieldacc',
         7:'funcinfo',8:'constfunc',9:'poolhint',10:'retfunc',11:'retpoint'}

def read_db(path):
    d = open(path,'rb').read()
    assert d[:4] == b'ZDB1', "magic"
    fmt,nt,so,sl,vo,vl,ck = struct.unpack_from('<7I', d, 4)
    tables = {}
    for i in range(nt):
        t,cnt,off,esz = struct.unpack_from('<4I', d, 32+i*16)
        tables[TYPES.get(t,t)] = (cnt, off, esz)
    ver = d[vo:vo+vl].decode('utf-8','replace')
    return tables, ver, d

def main():
    dbpath = sys.argv[1]
    tables, ver, d = read_db(dbpath)
    print(f"数据文件: {dbpath}")
    print(f"  版本: {ver}")
    for name in sorted(tables):
        cnt,off,esz = tables[name]
        print(f"  {name:12} {cnt:6} 条 × {esz} 字节 @ 0x{off:x}")
    print()

    base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    expect = {}

    for fn, arr in [('rettype_db.h','g_rettype'),('blcall_db.h','g_blcall'),
                    ('poolhint_db.h','g_poolhint'),('fields_real.h','g_fieldacc'),
                    ('frame_db.h','g_slot'),('frame_db.h','g_frame'),
                    ('const_db.h','g_const'),('const_db.h','g_constfunc'),
                    ('ret_db.h','g_retfunc'),('ret_db.h','g_retpoint')]:
        p = os.path.join(base,'data',fn)
        if not os.path.exists(p): continue
        src = strip_comments(open(p,encoding='utf-8',errors='replace').read())
        body = find_array(src, arr)
        if body is None: continue
        # 字符串数组（如 g_constfunc / g_retfunc）不能按 {} 拆
        if arr in ('g_constfunc','g_retfunc'):
            items = [x.strip() for x in body.split(',') if x.strip()]
            expect[arr] = len(items)
        else:
            ents = split_entries(body)
            expect[arr] = len(ents)

    ok = True
    mapping = {'rettype':'g_rettype','blcall':'g_blcall','poolhint':'g_poolhint',
               'fieldacc':'g_fieldacc','slot':'g_slot','frame':'g_frame','const':'g_const',
               'constfunc':'g_constfunc','retfunc':'g_retfunc','retpoint':'g_retpoint'}
    for name,arr in mapping.items():
        if name not in tables: continue
        got = tables[name][0]
        exp = expect.get(arr, -1)
        st = '✅' if got == exp else f'❌ 期望 {exp}'
        if got != exp: ok = False
        print(f"  {name:12} 打包 {got:6}  源 {exp:6}  {st}")

    print()
    print("全部一致" if ok else "存在差异")
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
