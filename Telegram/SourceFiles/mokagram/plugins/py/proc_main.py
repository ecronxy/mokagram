# Mokagram: загрузчик дочернего процесса-хоста плагинов (Linux, под bwrap).
# Запускается как `python3 -I -c <этот файл>` с сокетом клиента на дескрипторе 3.
# Всё внутри функции, чтобы служебные имена не попали в __main__, который видят плагины.
# Кадр: 4 байта длины, затем тип (1), id (8), id внешнего вызова (8), число строк (4),
# затем строки (длина 4 + байты UTF-8). Типы: 1 CALL, 2 RET, 3 NATIVE, 4 NRET, 5 BOOT.


def _moka_proc_main():
    import itertools
    import os
    import socket
    import struct
    import sys
    import threading
    import traceback
    import __main__ as main_module

    sock = socket.socket(fileno=3)
    sock.setblocking(True)
    send_lock = threading.Lock()
    pending = {}
    local = threading.local()
    ids = itertools.count(1)
    g = main_module.__dict__

    def read_exact(n):
        buf = bytearray()
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return bytes(buf)

    def send(kind, ident, parent, strings):
        parts = [struct.pack('>BQQI', kind, ident, parent, len(strings))]
        for s in strings:
            b = s.encode('utf-8', 'replace')
            parts.append(struct.pack('>I', len(b)))
            parts.append(b)
        body = b''.join(parts)
        with send_lock:
            sock.sendall(struct.pack('>I', len(body)) + body)

    def parse(body):
        kind, ident, parent, count = struct.unpack_from('>BQQI', body, 0)
        pos = 21
        strings = []
        for _ in range(count):
            (n,) = struct.unpack_from('>I', body, pos)
            pos += 4
            strings.append(body[pos:pos + n].decode('utf-8', 'replace'))
            pos += n
        return kind, ident, parent, strings

    def native(op, js):
        if not isinstance(op, str) or not isinstance(js, str):
            raise TypeError('_moka_native(op: str, json: str)')
        ident = next(ids)
        slot = [threading.Event(), '']
        pending[ident] = slot
        try:
            send(3, ident, getattr(local, 'call', 0), [op, js])
        except OSError:
            os._exit(1)
        slot[0].wait()
        return slot[1]

    g['_moka_native'] = native

    def run_call(ident, strings):
        local.call = ident
        value = ''
        try:
            func = g.get(strings[0])
            if func is not None:
                result = func(*strings[1:])
                if isinstance(result, str):
                    value = result
                elif isinstance(result, int):
                    value = str(int(result))
        except BaseException:
            traceback.print_exc(file=sys.__stderr__)
        drain = ''
        if strings[0] != '_moka_drain':
            try:
                drain = g['_moka_drain']()
            except BaseException:
                pass
        try:
            send(2, ident, 0, [value, drain])
        except OSError:
            os._exit(1)

    def boot(ident, strings):
        error = ''
        try:
            for i in range(0, len(strings) - 1, 2):
                exec(compile(strings[i + 1], strings[i], 'exec'), g)
        except BaseException:
            error = traceback.format_exc()
            traceback.print_exc(file=sys.__stderr__)
        send(2, ident, 0, ['' if error else '1', error])

    while True:
        head = read_exact(4)
        if head is None:
            os._exit(0)
        (size,) = struct.unpack('>I', head)
        body = read_exact(size)
        if body is None:
            os._exit(0)
        kind, ident, parent, strings = parse(body)
        if kind == 1:
            threading.Thread(target=run_call, args=(ident, strings), daemon=True).start()
        elif kind == 4:
            slot = pending.pop(ident, None)
            if slot is not None:
                slot[1] = strings[0] if strings else ''
                slot[0].set()
        elif kind == 5:
            boot(ident, strings)


_moka_proc_main()
