constexpr auto kFwScript = R"PY(
# Каркас плагинов Mokagram: команды, watchers, горячая перезагрузка, экран ошибки.
# Выполняется в тех же глобалах, что и host.py (видит _plugins, _say, _hook_error и т.д.).
import asyncio
import concurrent.futures
import shlex
import threading

_FW_PENDING = []        # [(ctx, peer, text, deadline)] ждут настоящий id отправленного сообщения
_fw_pool = concurrent.futures.ThreadPoolExecutor(max_workers=6, thread_name_prefix='moka-cb')
_fw_loop = [None]
_fw_prefix_cache = [0.0, '.']
_fw_mtimes = {}
_fw_shown_errors = set()


def _fw_api():
    return sys.modules['mokagram_sdk.api']


def _fw_setting(key, default):
    if key == 'plugins.dev_reload' and os.environ.get('MOKAGRAM_DEV_RELOAD') == '1':
        return True
    try:
        res = json.loads(_moka_native('client_setting_get', json.dumps({'key': key})))
        if res.get('ok'):
            return res.get('v', default)
    except Exception:
        pass
    return default


def _fw_prefix():
    now = time.monotonic()
    if now - _fw_prefix_cache[0] > 5:
        value = str(_fw_setting('plugins.prefix', '.') or '.').strip()
        _fw_prefix_cache[1] = value[:2] if value else '.'
        _fw_prefix_cache[0] = now
    return _fw_prefix_cache[1]


# ---- asyncio для async-команд ----

def _fw_get_loop():
    if _fw_loop[0] is None:
        loop = asyncio.new_event_loop()
        threading.Thread(target=loop.run_forever, name='moka-async', daemon=True).start()
        _fw_loop[0] = loop
    return _fw_loop[0]


async def _fw_guard(entry, coro, label, timeout):
    try:
        await asyncio.wait_for(coro, timeout)
    except asyncio.TimeoutError:
        _say('[%s] %s: не уложился в %d с, прерван' % (entry['id'], label, timeout))
        _toast('«%s»: %s завис и был прерван' % (entry['meta'].get('__name__') or entry['id'], label))
    except BaseException as e:
        _hook_error(entry, label, e)
        _toast('«%s»: ошибка в %s: %s' % (entry['meta'].get('__name__') or entry['id'], label, e))


def _fw_run_async(entry, coro, label, timeout=60):
    asyncio.run_coroutine_threadsafe(_fw_guard(entry, coro, label, timeout), _fw_get_loop())


def _fw_call(entry, fn, arg, timeout):
    """Колбэк плагина в отдельном потоке с таймаутом: зависший плагин не держит общий рабочий поток."""
    fut = _fw_pool.submit(fn, arg)
    try:
        return fut.result(timeout)
    except concurrent.futures.TimeoutError:
        _hook_slow(entry, getattr(fn, '__name__', 'callback'), timeout, timeout)
        raise TimeoutError('не уложился в %d с' % timeout)


# ---- контекст команды ----

class _Done:
    """Уже выполненное действие: можно и вызвать, и await."""

    def __await__(self):
        return iter(())


_DONE = _Done()


class Context:
    def __init__(self, entry, cur, prefix, name):
        self._entry = entry
        self._cur = cur
        self._result = None
        self._async = False
        self._msg_id = None
        self._queued = None
        self._sent = False
        self.prefix = prefix
        self.name = name
        self.peer = cur.get('peer')
        self.text = str(cur.get('text', ''))
        self.entities = list(cur.get('entities') or [])
        self.reply_to = cur.get('reply_to')
        orig = self.text
        m = re.match(r'\s*' + re.escape(prefix) + r'\S+\s*', orig)
        start = m.end() if m else len(orig)
        self.args = orig[start:].strip()
        end = start + len(self.args)
        self.args_entities = []
        for e in self.entities:
            try:
                o, l = int(e['offset']), int(e['length'])
            except Exception:
                continue
            if o >= start and o + l <= end:
                item = dict(e)
                item['offset'] = o - start
                self.args_entities.append(item)
        try:
            self.args_list = shlex.split(self.args)
        except ValueError:
            self.args_list = self.args.split()

    @property
    def reply_message(self):
        if not self.reply_to:
            return None
        return _fw_api().get_message(self.peer, self.reply_to)

    def edit(self, text, entities=None):
        """Результат команды: в обычном режиме подменяет текст, который уйдёт (сообщение не дублируется).
        В async-режиме отправляет сообщение, а следующие вызовы правят его."""
        text = str(text)
        if not self._async:
            self._result = (text, entities)
            return _DONE
        if self._msg_id is not None:
            _fw_api().edit_message(self.peer, self._msg_id, text, entities)
        elif not self._sent:
            self._sent = True
            self._queued = None
            if _fw_api().send_message(self.peer, text, reply_to=self.reply_to, entities=entities):
                _FW_PENDING.append((self, str(self.peer), text.strip(), time.monotonic() + 30))
        else:
            self._queued = (text, entities)
        return _DONE

    respond = edit

    def reply(self, text, entities=None):
        """Новое сообщение в чат (команда при этом не отправляется)."""
        _fw_api().send_message(self.peer, str(text), reply_to=self.reply_to, entities=entities)
        return _DONE


def _fw_on_sent(payload):
    peer, text = str(payload.get('peer')), str(payload.get('text', '')).strip()
    now = time.monotonic()
    for item in list(_FW_PENDING):
        ctx, p, t, deadline = item
        if now > deadline:
            _FW_PENDING.remove(item)
        elif p == peer and t == text:
            _FW_PENDING.remove(item)
            ctx._msg_id = int(payload['id'])
            if ctx._queued is not None:
                text2, ents = ctx._queued
                ctx._queued = None
                _fw_api().edit_message(ctx.peer, ctx._msg_id, text2, ents)
            break


def _fw_norm(res):
    if res is None:
        return None
    if isinstance(res, str):
        return (res, None)
    if isinstance(res, dict) and isinstance(res.get('text'), str):
        return (res['text'], res.get('entities'))
    if isinstance(res, (tuple, list)) and len(res) == 2 and isinstance(res[0], str):
        return (res[0], res[1])
    return None


def _fw_modify(text, entities):
    value = {'text': text}
    if isinstance(entities, list):
        value['entities'] = entities
    return json.dumps({'value': value}, ensure_ascii=False)


# ---- встроенные команды ----

_FW_BUILTIN = {}


def _fw_builtin(name, description):
    def deco(fn):
        _FW_BUILTIN[name] = (fn, description, '')
        return fn
    return deco


def _fw_all_commands():
    out = []
    for entry in list(_plugins.values()):
        if entry.get('plugin') is None:
            continue
        for cname, item in entry.get('commands', {}).items():
            out.append((cname, item[1], entry))
    return out


@_fw_builtin('help', 'список команд: .help или .help имя')
def _fw_cmd_help(ctx):
    prefix = ctx.prefix
    cmds = _fw_all_commands()
    want = ctx.args.strip().lstrip(prefix).lstrip('/').lower()
    if want:
        for cname, desc, entry in cmds:
            if cname == want:
                return '%s%s: %s\n(плагин «%s»)' % (prefix, cname, desc or 'описания нет',
                                                   entry['meta'].get('__name__') or entry['id'])
        for cname, (_f, desc, _p) in _FW_BUILTIN.items():
            if cname == want:
                return '%s%s: %s' % (prefix, cname, desc)
        return 'Команды «%s» нет. Список: %shelp' % (want, prefix)
    lines = ['Команды (%s — префикс):' % prefix]
    for cname, (_f, desc, _p) in sorted(_FW_BUILTIN.items()):
        lines.append('%s%s: %s' % (prefix, cname, desc))
    by_plugin = {}
    for cname, desc, entry in cmds:
        by_plugin.setdefault(entry['meta'].get('__name__') or entry['id'], []).append((cname, desc))
    for pname in sorted(by_plugin):
        lines.append('')
        lines.append('«%s»' % pname)
        for cname, desc in sorted(by_plugin[pname]):
            lines.append('%s%s: %s' % (prefix, cname, desc or '-'))
    return '\n'.join(lines)


# ---- запуск команд из send_message_hook ----

def _run_command(cur):
    """False, если это не команда. Иначе JSON ответа хука: отмена или подмена текста (edit-in-place)."""
    orig = str(cur.get('text', ''))
    text = orig.strip()
    if not text:
        return False
    prefix = _fw_prefix()
    used = None
    for p in (prefix, '/'):
        if p and text.startswith(p):
            used = p
            break
    if used is None:
        return False
    head = text[len(used):].split(None, 1)
    if not head:
        return False
    cmd = head[0].split('@')[0].lower()
    entry = handler = None
    for e in list(_plugins.values()):
        item = e.get('commands', {}).get(cmd)
        if item is not None and e.get('plugin') is not None:
            entry, handler = e, item[0]
            break
    if handler is None:
        if used == prefix and cmd in _FW_BUILTIN:
            entry = {'id': '_builtin', 'meta': {'__name__': 'Mokagram'}}
            handler = _FW_BUILTIN[cmd][0]
        else:
            return False
    ctx = Context(entry, cur, used, cmd)
    started = time.monotonic()
    try:
        res = handler(ctx)
        if asyncio.iscoroutine(res):
            ctx._async = True
            _fw_run_async(entry, res, 'команда %s' % cmd)
            return json.dumps({'cancel': True})
    except BaseException as e:
        if entry['id'] != '_builtin':
            _hook_error(entry, 'command', e)
        _toast('Команда %s: ошибка: %s' % (cmd, e))
        return json.dumps({'cancel': True})
    spent = time.monotonic() - started
    if spent > 0.5 and entry['id'] != '_builtin':
        _hook_slow(entry, 'command', spent, 0.5)
    out = _fw_norm(res) or ctx._result
    if out is None:
        return json.dumps({'cancel': True})
    return _fw_modify(out[0], out[1])


# ---- watchers ----

def _fw_watch_match(f, m):
    kind = str(m.get('peer_kind', ''))
    chats = f.get('chats')
    if chats:
        want = set()
        for c in (chats if isinstance(chats, (list, tuple, set)) else [chats]):
            want.add({'private': 'user', 'pm': 'user', 'dm': 'user'}.get(str(c), str(c)))
        if kind not in want and str(m.get('peer')) not in want:
            return False
    if f.get('out') is not None and bool(m.get('out')) != bool(f['out']):
        return False
    if f.get('incoming') and m.get('out'):
        return False
    ids = f.get('chat_ids')
    if ids and str(m.get('peer')) not in {str(i) for i in ids}:
        return False
    authors = f.get('from_ids')
    if authors and str(m.get('from')) not in {str(i) for i in authors}:
        return False
    media = f.get('media')
    if media is not None:
        mk = str(m.get('media', ''))
        if media is True and not mk:
            return False
        if media is False and mk:
            return False
        if isinstance(media, str) and mk != media:
            return False
        if isinstance(media, (list, tuple, set)) and mk not in media:
            return False
    rx = f.get('_rx')
    if rx is not None and not rx.search(str(m.get('text', ''))):
        return False
    return True


def _fw_add_watcher(entry, fn, filters):
    f = dict(filters)
    pattern = f.get('regex')
    if pattern:
        f['_rx'] = re.compile(pattern, re.I | re.S)
    entry.setdefault('watchers', []).append((fn, f))
    _fw_sync_events(entry)
    return True


def _fw_sync_events(entry):
    if not entry.get('watchers'):
        return
    if not _granted(entry['id'], 'messages.read'):
        _say('[%s] watcher: нет права messages.read' % entry['id'])
        return
    names = set(entry.get('events') or ())
    if 'new_message' not in names:
        names.add('new_message')
        entry['events'] = names
        _moka_native('events_subscribe', json.dumps({'pid': entry['id'], 'events': sorted(names)}))


def _fw_event(name, payload):
    if name == 'message_sent':
        _fw_on_sent(payload)
        return
    if name != 'new_message':
        return
    for entry in list(_plugins.values()):
        ws = entry.get('watchers')
        if not ws or entry.get('plugin') is None:
            continue
        for fn, f in ws:
            try:
                if not _fw_watch_match(f, payload):
                    continue
                res = fn(dict(payload))
                if asyncio.iscoroutine(res):
                    _fw_run_async(entry, res, 'watcher')
            except BaseException as e:
                _hook_error(entry, 'watcher', e)


# ---- сбор команд и watchers из декораторов ----

def _fw_collect(entry, inst):
    entry['watchers'] = []
    for attr in dir(type(inst)):
        fn = getattr(inst, attr, None)
        if not callable(fn):
            continue
        cmd = getattr(fn, '_moka_cmd', None)
        if cmd:
            names = [cmd[0]] + list(cmd[2])
            for n in names:
                n = str(n).strip().lower().lstrip('/')
                if n and n.replace('_', '').isalnum() and len(n) <= 32:
                    entry.setdefault('commands', {})[n] = (fn, str(cmd[1])[:100])
        w = getattr(fn, '_moka_watcher', None)
        if w is not None:
            _fw_add_watcher(entry, fn, w)


# ---- горячая перезагрузка и экран ошибки ----

def _fw_error_screen(entry, title, text):
    key = (entry['id'], text[:200])
    if key in _fw_shown_errors:
        return
    _fw_shown_errors.add(key)
    try:
        _moka_native('dialog_show', json.dumps({
            'pid': entry['id'], 'title': str(title)[:120], 'message': str(text)[:3800], 'fields': [],
            'buttons': [{'id': 'ok', 'text': 'Закрыть', 'style': 'primary', 'keep_open': False}],
            'cb': '', 'close_cb': ''}, ensure_ascii=False))
    except Exception:
        pass


def _fw_watch_loop():
    while True:
        time.sleep(2)
        try:
            if not _fw_setting('plugins.dev_reload', False) or not os.path.isdir(_folder):
                continue
            for name in sorted(os.listdir(_folder)):
                if not name.endswith('.plugin'):
                    continue
                path = os.path.join(_folder, name)
                m = os.stat(path).st_mtime
                old = _fw_mtimes.get(name)
                _fw_mtimes[name] = m
                if old is None or old == m:
                    continue
                with open(path, 'rb') as f:
                    raw = f.read()
                info = json.loads(_moka_parse(raw.decode('utf8', 'replace')))
                if not info['error'] and info['meta'].get('__id__'):
                    _record(name, raw, info['meta'])   # разработчик сам правит файл: хеш обновляем
                pid = _moka_load_file(name)
                entry = _plugins.get(pid)
                if entry is not None and entry['state'] == 'loaded':
                    _toast('Плагин «%s» перезагружен' % (entry['meta'].get('__name__') or pid))
                elif entry is not None:
                    _fw_shown_errors.discard((entry['id'], (entry.get('error') or '')[:200]))
                    _fw_error_screen(entry, 'Плагин «%s» не загрузился' % (entry['meta'].get('__name__') or pid),
                                     entry.get('error') or 'ошибка')
        except BaseException:
            pass


def _fw_start():
    for name in os.listdir(_folder) if os.path.isdir(_folder) else []:
        if name.endswith('.plugin'):
            try:
                _fw_mtimes[name] = os.stat(os.path.join(_folder, name)).st_mtime
            except OSError:
                pass
    threading.Thread(target=_fw_watch_loop, name='moka-reload', daemon=True).start()
    _moka_native('events_subscribe', json.dumps({'pid': '_fw', 'events': ['message_sent']}))
)PY";
