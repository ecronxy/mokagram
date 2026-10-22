"""Делает tl_names.py из mtproto/scheme/api.tl: имя -> constructor id для обновлений и методов.
Нужен хосту, чтобы переводить __hook_filters__ (имена) в uint32 для фильтра в C++.
Запускается из gen_inc.sh; схема одна на клиент и плагины, при смене слоя просто перегенерировать.
"""
import os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
schema = os.path.normpath(os.path.join(here, '..', '..', '..', 'mtproto', 'scheme', 'api.tl'))

line_re = re.compile(r'^([A-Za-z0-9_.]+)#([0-9a-f]{1,8})\s.*=\s*([A-Za-z0-9_.<>]+);\s*$')
names = {}
in_functions = False
with open(schema, encoding='utf8') as f:
    for raw in f:
        line = raw.strip()
        if line.startswith('---functions---'):
            in_functions = True
            continue
        if line.startswith('---types---'):
            in_functions = False
            continue
        m = line_re.match(line)
        if not m:
            continue
        name, cid, result = m.group(1), int(m.group(2), 16), m.group(3)
        if in_functions or result in ('Update', 'Updates'):
            names[name] = cid
names['rpc_error'] = 0x2144ca19

out = ['# Создано gen_tl.py из api.tl, руками не править.', '_MOKA_TL = {']
for name in sorted(names):
    out.append("    '%s': 0x%08x," % (name, names[name]))
out.append('}')
sys.stdout.write('\n'.join(out) + '\n')
