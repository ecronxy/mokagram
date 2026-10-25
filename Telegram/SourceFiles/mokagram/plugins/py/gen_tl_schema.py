"""Полная схема TL (api.tl + mtproto.tl) в компактном JSON для tl.py. Запускается из gen_inc.sh."""
import json, os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
base = os.path.normpath(os.path.join(here, '..', '..', '..', 'mtproto', 'scheme'))
line_re = re.compile(r'^([A-Za-z0-9_.]+)#([0-9a-f]{1,8})\s+(.*?)=\s*([A-Za-z0-9_.<>!]+)\s*;\s*$')
ctors = {}
for fname in ('mtproto.tl', 'api.tl'):
    is_func = False
    with open(os.path.join(base, fname), encoding='utf8') as f:
        for raw in f:
            line = raw.strip()
            if line.startswith('---functions---'):
                is_func = True
                continue
            if line.startswith('---types---'):
                is_func = False
                continue
            if not line or line.startswith('//'):
                continue
            m = line_re.match(line)
            if not m:
                continue
            name, cid, rest, result = m.group(1), int(m.group(2), 16), m.group(3), m.group(4)
            if name in ('vector', 'int', 'long', 'double', 'string', 'bytes'):
                continue
            params = []
            for tok in rest.split():
                if tok.startswith('{') or tok.endswith('}'):
                    continue
                if ':' not in tok:
                    continue
                pname, ptype = tok.split(':', 1)
                params.append([pname, ptype])
            ctors[name] = [cid, params, result, 1 if is_func else 0]
json.dump({'c': ctors}, sys.stdout, separators=(',', ':'), ensure_ascii=False)
