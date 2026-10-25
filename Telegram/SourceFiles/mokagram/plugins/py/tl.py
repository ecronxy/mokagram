"""Mokagram SDK: кодек TL (схема api.tl слоя клиента). Нужен invoke_tl и разбору сырых запросов.

encode_request(method, params) -> bytes   (тело запроса с constructor id)
decode_result(method, data) -> значение   (ответ метода, тип берётся из схемы)
decode_object(data) -> dict               (любой объект с constructor id)
Объекты это dict с ключом '_' (имя конструктора), как в Telethon. Флаги считаются сами.
"""
import json
import struct

_SRC = None        # текст схемы JSON, ставит хост; разбирается при первом использовании
_S = None          # схема: {'c': {name: [id, [[pname, ptype]], result, is_func]}, 'i': {id: name}, 't': {type: [names]}}
_VECTOR = 0x1cb5c415
_BOOL_TRUE = 0x997275b5
_BOOL_FALSE = 0xbc799737


def load(schema_json):
    global _S
    raw = json.loads(schema_json)
    c = raw['c']
    ids = {}
    types = {}
    for name, (cid, params, result, is_func) in c.items():
        ids[cid] = name
        if not is_func:
            types.setdefault(result, []).append(name)
    _S = {'c': c, 'i': ids, 't': types}


def _schema():
    if _S is None:
        if _SRC is None:
            raise RuntimeError('схема TL не загружена')
        load(_SRC)
    return _S


def _boxed(t):
    last = t.split('.')[-1]
    return bool(last) and last[0].isupper() and t != 'X'


def _vec_inner(t):
    if t.startswith('Vector<') or t.startswith('vector<'):
        return t[7:-1], t[0] == 'V'
    return None, False


# ---- запись ----

def _w_str(out, b):
    if isinstance(b, str):
        b = b.encode('utf8')
    n = len(b)
    if n < 254:
        out += bytes([n]) + b
        pad = (-(n + 1)) % 4
    else:
        out += bytes([254, n & 255, (n >> 8) & 255, (n >> 16) & 255]) + b
        pad = (-n) % 4
    out += b'\0' * pad


def _w_value(out, t, v):
    if t in ('int',):
        out += struct.pack('<i', int(v))
    elif t == 'long':
        out += struct.pack('<q', int(v))
    elif t == 'double':
        out += struct.pack('<d', float(v))
    elif t in ('string', 'bytes'):
        _w_str(out, v)
    elif t == 'int128':
        out += _fixed(v, 16)
    elif t == 'int256':
        out += _fixed(v, 32)
    elif t == 'Bool':
        out += struct.pack('<I', _BOOL_TRUE if v else _BOOL_FALSE)
    elif t == 'true':
        pass
    elif t == '!X' or t == 'X':
        _w_object(out, v)
    else:
        inner, boxed_vec = _vec_inner(t)
        if inner is not None:
            if boxed_vec:
                out += struct.pack('<I', _VECTOR)
            out += struct.pack('<i', len(v))
            for item in v:
                _w_value(out, inner, item)
        elif _boxed(t):
            _w_object(out, v, t)
        else:
            _w_fields(out, _schema()['c'][t], v)


def _fixed(v, n):
    if isinstance(v, (bytes, bytearray)):
        return bytes(v).ljust(n, b'\0')[:n]
    if isinstance(v, int):
        return v.to_bytes(n, 'little', signed=True)
    return bytes.fromhex(v)[:n].ljust(n, b'\0')


def _w_object(out, v, expect=None):
    if not isinstance(v, dict):
        raise TypeError('ожидался объект TL (dict с ключом "_"), а не %s' % type(v).__name__)
    name = v.get('_')
    c = _schema()['c']
    if name is None and expect is not None:
        options = _schema()['t'].get(expect, [])
        if len(options) == 1:
            name = options[0]
    if name not in c:
        raise ValueError('неизвестный конструктор %r' % (name,))
    cid, params, _result, _f = c[name]
    out += struct.pack('<I', cid)
    _w_fields(out, c[name], v)


def _w_fields(out, ctor, v):
    _cid, params, _result, _f = ctor
    flagvals = {}
    # сначала считаем поля-флаги
    for pname, ptype in params:
        if ptype == '#':
            flagvals[pname] = 0
    for pname, ptype in params:
        if '?' in ptype:
            fl, rest = ptype.split('?', 1)
            fname, bit = fl.split('.')
            val = v.get(pname)
            present = (val is not None and val is not False) if rest == 'true' else (val is not None)
            if present:
                flagvals[fname] |= 1 << int(bit)
    for pname, ptype in params:
        if ptype == '#':
            out += struct.pack('<I', flagvals[pname])
            continue
        if '?' in ptype:
            fl, rest = ptype.split('?', 1)
            fname, bit = fl.split('.')
            if not (flagvals[fname] >> int(bit)) & 1 or rest == 'true':
                continue
            _w_value(out, rest, v[pname])
            continue
        if pname not in v:
            raise KeyError('нет поля %s у %s' % (pname, v.get('_')))
        _w_value(out, ptype, v[pname])


def encode_request(method, params=None):
    c = _schema()['c']
    if method not in c or not c[method][3]:
        raise ValueError('нет такого метода TL: %s' % method)
    out = bytearray()
    out += struct.pack('<I', c[method][0])
    _w_fields(out, c[method], params or {})
    return bytes(out)


def encode_object(v):
    out = bytearray()
    _w_object(out, v)
    return bytes(out)


# ---- чтение ----

class _R:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def take(self, n):
        if self.p + n > len(self.d):
            raise ValueError('неожиданный конец данных')
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def u32(self):
        return struct.unpack('<I', self.take(4))[0]

    def s32(self):
        return struct.unpack('<i', self.take(4))[0]

    def s64(self):
        return struct.unpack('<q', self.take(8))[0]

    def string(self):
        n = self.take(1)[0]
        if n == 254:
            b = self.take(3)
            n = b[0] | (b[1] << 8) | (b[2] << 16)
            head = 4
        else:
            head = 1
        data = self.take(n)
        self.take((-(n + head)) % 4)
        return data


def _r_value(r, t, flagsctx=None):
    if t == 'int':
        return r.s32()
    if t == 'long':
        return r.s64()
    if t == 'double':
        return struct.unpack('<d', r.take(8))[0]
    if t == 'string':
        return r.string().decode('utf8', 'replace')
    if t == 'bytes':
        return r.string()
    if t == 'int128':
        return r.take(16)
    if t == 'int256':
        return r.take(32)
    if t == 'true':
        return True
    if t == 'Bool':
        cid = r.u32()
        if cid == _BOOL_TRUE:
            return True
        if cid == _BOOL_FALSE:
            return False
        raise ValueError('ожидался Bool')
    if t in ('X', '!X'):
        return _r_object(r)
    inner, boxed_vec = _vec_inner(t)
    if inner is not None:
        if boxed_vec:
            if r.u32() != _VECTOR:
                raise ValueError('ожидался Vector')
        n = r.s32()
        if n < 0 or n > 1000000:
            raise ValueError('странная длина вектора')
        return [_r_value(r, inner) for _ in range(n)]
    if _boxed(t):
        return _r_object(r)
    return _r_fields(r, t)


def _r_fields(r, name):
    cid, params, _res, _f = _schema()['c'][name]
    out = {'_': name}
    flagvals = {}
    for pname, ptype in params:
        if ptype == '#':
            flagvals[pname] = r.u32()
            continue
        if '?' in ptype:
            fl, rest = ptype.split('?', 1)
            fname, bit = fl.split('.')
            if not (flagvals.get(fname, 0) >> int(bit)) & 1:
                continue
            out[pname] = True if rest == 'true' else _r_value(r, rest)
            continue
        out[pname] = _r_value(r, ptype)
    return out


def _r_object(r):
    cid = r.u32()
    if cid == _VECTOR:
        raise ValueError('вектор без известного типа')
    if cid in (_BOOL_TRUE, _BOOL_FALSE):
        return cid == _BOOL_TRUE
    s = _schema()
    name = s['i'].get(cid)
    if name is None:
        raise ValueError('неизвестный constructor id 0x%08x' % cid)
    return _r_fields(r, name)


def decode_object(data):
    r = _R(bytes(data))
    return _r_object(r)


def decode_result(method, data):
    c = _schema()['c']
    result_type = c[method][2]
    r = _R(bytes(data))
    if result_type.startswith('Vector<'):
        return _r_value(r, result_type)
    return _r_object(r)
