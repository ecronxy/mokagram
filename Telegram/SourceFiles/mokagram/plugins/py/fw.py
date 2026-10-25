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
                _fw_private('%s%s' % (prefix, cname), '%s\n(плагин «%s»)' % (desc or 'описания нет',
                            entry['meta'].get('__name__') or entry['id']))
                return
        for cname, (_f, desc, _p) in _FW_BUILTIN.items():
            if cname == want:
                _fw_private('%s%s' % (prefix, cname), desc)
                return
        _fw_private('Справка', 'Команды «%s» нет. Список: %shelp' % (want, prefix))
        return
    lines = ['Префикс команд: %s' % prefix, '']
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
    _fw_private('Команды', '\n'.join(lines))


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
    inst = entry.get('plugin')
    base = sys.modules['mokagram_sdk.base_plugin'].BasePlugin
    wants_album = inst is not None and getattr(type(inst), 'on_album', None) is not getattr(base, 'on_album', None)
    if not entry.get('watchers') and not wants_album:
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
    _fw_album_collect(payload)
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
    _fw_sync_events(entry)


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
                if old == m:
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
    threading.Thread(target=_fw_sched_loop, name='moka-sched', daemon=True).start()
    _fw_sync_updates()
    _moka_native('events_subscribe', json.dumps({'pid': '_fw', 'events': ['message_sent']}))


# =====================================================================================
# Этап 2: планировщик (cron и постоянные интервалы), встроенные команды
# =====================================================================================

_fw_cbs = {}
_fw_cb_n = [0]


def _fw_cb(fn):
    """Колбэк самого каркаса (pid '_fw'), приходит через _moka_invoke."""
    _fw_cb_n[0] += 1
    token = 'f%d' % _fw_cb_n[0]
    _fw_cbs[token] = fn
    return token


def _fw_private(title, text, buttons=None, on_button=None):
    """Окно только для вас (в чат ничего не уходит)."""
    cb = _fw_cb(lambda p: on_button(p.get('button')) if on_button else None) if on_button else ''
    try:
        _moka_native('dialog_show', json.dumps({
            'pid': '_fw', 'title': str(title)[:120], 'message': str(text)[:3800], 'fields': [],
            'buttons': buttons or [{'id': 'ok', 'text': 'Закрыть', 'style': 'primary', 'keep_open': False}],
            'cb': cb, 'close_cb': ''}, ensure_ascii=False))
    except Exception:
        pass


def _cron_field(spec, lo, hi):
    out = set()
    for part in spec.split(','):
        step = 1
        if '/' in part:
            part, s = part.split('/', 1)
            step = max(1, int(s))
        if part == '*':
            a, b = lo, hi
        elif '-' in part:
            a, b = (int(x) for x in part.split('-', 1))
        else:
            a = b = int(part)
        out.update(v for v in range(a, b + 1, step) if lo <= v <= hi)
    if not out:
        raise ValueError('пустое поле cron')
    return out


class _Cron:
    def __init__(self, expr):
        f = str(expr).split()
        if len(f) != 5:
            raise ValueError('cron: нужно 5 полей (минута час день месяц день_недели)')
        self.minute = _cron_field(f[0], 0, 59)
        self.hour = _cron_field(f[1], 0, 23)
        self.dom = _cron_field(f[2], 1, 31)
        self.month = _cron_field(f[3], 1, 12)
        self.dow = {d % 7 for d in _cron_field(f[4], 0, 7)}
        self.dom_star = f[2] == '*'
        self.dow_star = f[4] == '*'

    def match(self, t):
        if t.tm_min not in self.minute or t.tm_hour not in self.hour or t.tm_mon not in self.month:
            return False
        dom_ok = t.tm_mday in self.dom
        dow_ok = ((t.tm_wday + 1) % 7) in self.dow
        if self.dom_star and self.dow_star:
            return True
        if self.dom_star:
            return dow_ok
        if self.dow_star:
            return dom_ok
        return dom_ok or dow_ok

    def fired_between(self, a, b):
        """Был ли запуск в (a, b]. Пропущенное смотрим не глубже двух недель."""
        t = (int(max(a, b - 14 * 86400)) // 60 + 1) * 60
        while t <= b:
            if self.match(time.localtime(t)):
                return True
            t += 60
        return False


def _sched_path(pid):
    return os.path.join(_data_dir(pid), 'schedule.json')


def _sched_load(pid):
    try:
        with open(_sched_path(pid), encoding='utf8') as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def _sched_save(pid, data):
    try:
        os.makedirs(_data_dir(pid), exist_ok=True)
        tmp = _sched_path(pid) + '.tmp'
        with open(tmp, 'w', encoding='utf8') as f:
            json.dump(data, f)
        os.replace(tmp, _sched_path(pid))
    except Exception as e:
        _say('[%s] расписание не сохранено: %s' % (pid, e))


def _fw_job(entry, kind, spec, fn, name):
    """Регистрирует задачу: kind 'cron' (spec строка) или 'interval' (spec секунды). Расписание помнит запуски на диске."""
    name = str(name or getattr(fn, '__name__', 'job'))[:64]
    jobs = entry.setdefault('jobs', [])
    jobs[:] = [j for j in jobs if j['name'] != name]
    if kind == 'cron':
        spec_obj = _Cron(spec)
    else:
        spec_obj = max(5, int(spec))
    store = _sched_load(entry['id'])
    now = time.time()
    last = store.get(name)
    job = {'name': name, 'kind': kind, 'spec': spec_obj, 'fn': fn, 'last': last if last else now, 'running': False}
    jobs.append(job)
    if not last:
        store[name] = now
        _sched_save(entry['id'], store)
    return job


def _fw_run_job(entry, job):
    job['running'] = True
    try:
        res = job['fn']()
        if asyncio.iscoroutine(res):
            _fw_run_async(entry, res, 'задача %s' % job['name'], 120)
    except BaseException as e:
        _hook_error(entry, 'job:%s' % job['name'], e)
    finally:
        job['running'] = False


def _fw_sched_loop():
    while True:
        time.sleep(15)
        try:
            now = time.time()
            for entry in list(_plugins.values()):
                if entry.get('plugin') is None:
                    continue
                for job in list(entry.get('jobs', ())):
                    if job['running']:
                        continue
                    if job['kind'] == 'cron':
                        due = job['spec'].fired_between(job['last'], now)
                    else:
                        due = now - job['last'] >= job['spec']
                    if not due:
                        continue
                    job['last'] = now
                    store = _sched_load(entry['id'])
                    store[job['name']] = now
                    _sched_save(entry['id'], store)
                    _fw_pool.submit(_fw_run_job, entry, job)
        except BaseException:
            pass


# ---- встроенные команды: ответ только вам, в чат не уходит ----

def _fw_state_name(entry):
    return {'loaded': 'работает', 'error': 'ошибка', 'disabled': 'выключен', 'needs_review': 'ждёт подтверждения',
            'paused': 'на паузе'}.get(entry.get('state'), str(entry.get('state')))


@_fw_builtin('plugins', 'список плагинов и их состояние')
def _fw_cmd_plugins(ctx):
    lines = []
    for entry in _plugins.values():
        m = entry.get('meta', {})
        lines.append('%s v%s: %s%s' % (m.get('__name__') or entry['id'], m.get('__version__', '?'), _fw_state_name(entry),
                                       (' (%s)' % entry['error']) if entry.get('error') else ''))
    _fw_private('Плагины', '\n'.join(lines) or 'Плагинов нет')


@_fw_builtin('logs', 'последние строки журнала плагинов: .logs или .logs 30')
def _fw_cmd_logs(ctx):
    try:
        n = max(5, min(200, int(ctx.args or 30)))
    except ValueError:
        n = 30
    lines = list(_fw_ring)[-n:]
    _fw_private('Журнал плагинов', '\n'.join(l[:300] for l in lines) or 'Пусто')


def _fw_setting_kind(entry, key):
    st = entry.get('settings') or {}
    return st.get(key)


@_fw_builtin('config', 'настройки плагинов: .config, .config плагин, .config плагин ключ значение')
def _fw_cmd_config(ctx):
    a = ctx.args_list
    if not a:
        names = [(e['meta'].get('__name__') or e['id']) + ' (' + e['id'] + ')' for e in _plugins.values() if e.get('settings')]
        _fw_private('Настройки', 'Плагины с настройками:\n' + '\n'.join(names) if names else 'Ни у кого нет настроек')
        return
    entry = _plugins.get(a[0])
    if entry is None or not entry.get('settings'):
        _fw_private('Настройки', 'Плагина «%s» с настройками нет' % a[0])
        return
    keys = list(entry['settings'].keys())
    if len(a) == 1:
        rows = []
        for k in keys:
            res = json.loads(_moka_native('setting_get', json.dumps({'key': 'plugin.%s.%s' % (entry['id'], k)})))
            rows.append('%s = %s' % (k, res.get('v') if res.get('ok') else '?'))
        _fw_private('Настройки: ' + a[0], '\n'.join(rows) + '\n\nИзменить: .config %s ключ значение' % a[0])
        return
    key, value = a[1], ' '.join(a[2:])
    if key not in entry['settings']:
        _fw_private('Настройки', 'Ключа «%s» нет. Есть: %s' % (key, ', '.join(keys)))
        return
    low = value.lower()
    parsed = value
    if low in ('on', 'true', 'да', 'вкл', '1'):
        parsed = True
    elif low in ('off', 'false', 'нет', 'выкл', '0'):
        parsed = False
    elif re.fullmatch(r'-?\d+', value):
        parsed = int(value)
    res = json.loads(_moka_native('setting_set', json.dumps({'key': 'plugin.%s.%s' % (entry['id'], key), 'value': parsed})))
    _fw_private('Настройки', 'Готово' if res.get('ok') else 'Не вышло: %s' % res.get('error', ''))


@_fw_builtin('unloadmod', 'удалить плагин: .unloadmod id')
def _fw_cmd_unloadmod(ctx):
    pid = ctx.args.strip()
    entry = _plugins.get(pid)
    if entry is None:
        _fw_private('Удаление', 'Плагина «%s» нет. Список: .plugins' % pid)
        return
    name = entry['meta'].get('__name__') or pid
    _fw_private('Удалить плагин?', 'Плагин «%s» и его файл будут удалены.' % name,
                [{'id': 'yes', 'text': 'Удалить', 'style': 'red'}, {'id': 'no', 'text': 'Отмена', 'style': 'cancel'}],
                lambda b: _moka_remove(pid) if b == 'yes' else None)


def _fw_install_downloaded(path):
    res = json.loads(_moka_native('install_plugin_file', json.dumps({'path': path})))
    if not res.get('ok'):
        _fw_private('Установка', 'Не вышло: %s' % res.get('error', ''))


@_fw_builtin('loadmod', 'установить плагин: ответом на файл .plugin или .loadmod ссылка')
def _fw_cmd_loadmod(ctx):
    import tempfile
    tmp = tempfile.mkdtemp(prefix='moka_loadmod_')
    path = os.path.join(tmp, 'plugin.plugin')
    url = ctx.args.strip()
    if url.startswith(('http://', 'https://')):
        def work():
            try:
                resp = _fw_http_request('GET', url, None, None, 15, 1 << 20)
                if resp['status'] != 200:
                    raise RuntimeError('HTTP %s' % resp['status'])
                with open(path, 'wb') as f:
                    f.write(resp['content'])
                _fw_install_downloaded(path)
            except BaseException as e:
                _fw_private('Установка', 'Не скачалось: %s' % e)
        _fw_pool.submit(work)
        return
    if not ctx.reply_to:
        _fw_private('Установка', 'Ответьте командой на файл .plugin или укажите ссылку.')
        return

    def done(p):
        if p.get('ok'):
            _fw_install_downloaded(p['path'])
        else:
            _fw_private('Установка', 'Файл не скачался: %s' % p.get('error', ''))
    token = _fw_cb(done)
    res = json.loads(_moka_native('download_media', json.dumps({
        'pid': '_fw', 'token': token, 'peer': str(ctx.peer), 'id': str(ctx.reply_to), 'path': path})))
    if not res.get('ok'):
        _fw_private('Установка', 'Не вышло: %s' % res.get('error', ''))


# =====================================================================================
# Этап 3: http, общая база, секреты, язык, зависимости
# =====================================================================================

import ipaddress
import socket
import urllib.error
import urllib.parse
import urllib.request


def _fw_check_url(url):
    p = urllib.parse.urlparse(url)
    if p.scheme not in ('http', 'https') or not p.hostname:
        raise ValueError('допустимы только http и https')
    for info in socket.getaddrinfo(p.hostname, p.port or (443 if p.scheme == 'https' else 80)):
        ip = ipaddress.ip_address(info[4][0])
        if (ip.is_private or ip.is_loopback or ip.is_link_local or ip.is_reserved or ip.is_multicast
                or ip.is_unspecified):
            raise PermissionError('адрес во внутренней сети закрыт')


class _FwRedirect(urllib.request.HTTPRedirectHandler):
    max_redirections = 5

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        _fw_check_url(newurl)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def _fw_http_request(method, url, headers, body, timeout, maxsize, save_to=None):
    """Один запрос в потоке пула. Возвращает {'status', 'headers', 'content'} (content пуст, если save_to)."""
    _fw_check_url(url)
    req = urllib.request.Request(url, data=body, method=method, headers=dict(headers or {}))
    if 'User-Agent' not in req.headers and 'User-agent' not in req.headers:
        req.add_header('User-Agent', 'Mokagram')
    opener = urllib.request.build_opener(_FwRedirect())
    try:
        resp = opener.open(req, timeout=max(1, min(int(timeout), 60)))
        status = resp.status
    except urllib.error.HTTPError as e:
        resp, status = e, e.code
    total = 0
    chunks = []
    out = open(save_to, 'wb') if save_to else None
    try:
        while True:
            chunk = resp.read(65536)
            if not chunk:
                break
            total += len(chunk)
            if total > maxsize:
                raise ValueError('ответ больше %d МБ' % (maxsize >> 20))
            (out.write if out else chunks.append)(chunk)
    finally:
        if out:
            out.close()
    return {'status': status, 'headers': dict(resp.headers.items()), 'content': b''.join(chunks)}


_FW_DB = {'data': None}
_fw_db_lock = threading.Lock()
_fw_db_subs = []     # [(pid, prefix, fn)]
_FW_DB_QUOTA = 4 << 20


def _fw_db_path():
    return os.path.join(_data_root(), 'plugin_data', '_shared', 'db.json')


def _fw_db_load():
    if _FW_DB['data'] is None:
        try:
            with open(_fw_db_path(), encoding='utf8') as f:
                data = json.load(f)
            _FW_DB['data'] = data if isinstance(data, dict) else {}
        except Exception:
            _FW_DB['data'] = {}
    return _FW_DB['data']


def _fw_db_save():
    try:
        path = _fw_db_path()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        raw = json.dumps(_FW_DB['data'], ensure_ascii=False)
        if len(raw) > _FW_DB_QUOTA:
            return False
        tmp = path + '.tmp'
        with open(tmp, 'w', encoding='utf8') as f:
            f.write(raw)
        os.replace(tmp, path)
        return True
    except Exception as e:
        _say('общая база не сохранена: %s' % e)
        return False


def _fw_db_set(pid, key, value, delete=False):
    with _fw_db_lock:
        data = _fw_db_load()
        old = data.get(key)
        if delete:
            data.pop(key, None)
        else:
            data[key] = value
        if not _fw_db_save():
            if old is None:
                data.pop(key, None)
            else:
                data[key] = old
            return False
    for spid, prefix, fn in list(_fw_db_subs):
        if key.startswith(prefix):
            entry = _plugins.get(spid)
            if entry is not None and entry.get('plugin') is not None:
                _fw_pool.submit(_fw_db_notify, entry, fn, key, None if delete else value, old, pid)
    return True


def _fw_db_notify(entry, fn, key, value, old, pid):
    try:
        fn(key, value, old, pid)
    except BaseException as e:
        _hook_error(entry, 'db.on_change', e)


# ---- секреты: хранилище ОС (secret-tool) или файл с ключом 0600 ----

def _fw_secret_file():
    return os.path.join(_data_root(), 'plugin_data', '_shared', 'secrets.enc')


def _fw_secret_key():
    import secrets as _s
    path = os.path.join(os.path.expanduser('~'), '.config', 'mokagram', 'secrets.key')
    try:
        with open(path, 'rb') as f:
            key = f.read()
        if len(key) == 32:
            return key
    except OSError:
        pass
    os.makedirs(os.path.dirname(path), exist_ok=True)
    key = _s.token_bytes(32)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'wb') as f:
        f.write(key)
    return key


def _fw_crypt(data, key, nonce):
    import hashlib
    import hmac
    out = bytearray()
    counter = 0
    while len(out) < len(data):
        out += hmac.new(key, nonce + counter.to_bytes(8, 'big'), hashlib.sha256).digest()
        counter += 1
    return bytes(a ^ b for a, b in zip(data, out))


def _fw_secret_file_io(write=None):
    import hashlib
    import hmac
    import secrets as _s
    path = _fw_secret_file()
    key = _fw_secret_key()
    if write is None:
        try:
            with open(path, 'rb') as f:
                blob = f.read()
            nonce, tag, body = blob[:16], blob[16:48], blob[48:]
            if not hmac.compare_digest(tag, hmac.new(key, nonce + body, hashlib.sha256).digest()):
                return {}
            return json.loads(_fw_crypt(body, key, nonce).decode('utf8'))
        except Exception:
            return {}
    nonce = _s.token_bytes(16)
    body = _fw_crypt(json.dumps(write, ensure_ascii=False).encode('utf8'), key, nonce)
    tag = hmac.new(key, nonce + body, hashlib.sha256).digest()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd = os.open(path + '.tmp', os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'wb') as f:
        f.write(nonce + tag + body)
    os.replace(path + '.tmp', path)
    return write


def _fw_secret_get(pid, name):
    import subprocess
    try:
        r = subprocess.run(['secret-tool', 'lookup', 'service', 'mokagram', 'plugin', pid, 'name', name],
                           capture_output=True, timeout=5)
        if r.returncode == 0 and r.stdout:
            return r.stdout.decode('utf8')
    except Exception:
        pass
    return _fw_secret_file_io().get('%s/%s' % (pid, name))


def _fw_secret_set(pid, name, value):
    import subprocess
    if value is None:
        try:
            subprocess.run(['secret-tool', 'clear', 'service', 'mokagram', 'plugin', pid, 'name', name], timeout=5)
        except Exception:
            pass
        data = _fw_secret_file_io()
        data.pop('%s/%s' % (pid, name), None)
        _fw_secret_file_io(data)
        return True
    try:
        r = subprocess.run(['secret-tool', 'store', '--label=Mokagram %s/%s' % (pid, name),
                            'service', 'mokagram', 'plugin', pid, 'name', name],
                           input=str(value).encode('utf8'), capture_output=True, timeout=5)
        if r.returncode == 0:
            return True
    except Exception:
        pass
    data = _fw_secret_file_io()
    data['%s/%s' % (pid, name)] = str(value)
    _fw_secret_file_io(data)
    return True


# ---- язык, зависимости ----

_fw_lang_cache = [None]


def _fw_lang():
    if _fw_lang_cache[0] is None:
        try:
            _fw_lang_cache[0] = json.loads(_moka_native('client_lang', '{}')).get('lang', 'ru')
        except Exception:
            _fw_lang_cache[0] = 'ru'
    return _fw_lang_cache[0]


def _fw_missing_deps(meta):
    deps = meta.get('__dependencies__') or []
    if not isinstance(deps, (list, tuple)):
        return []
    return [d for d in deps if isinstance(d, str) and (d not in _plugins or _plugins[d].get('state') != 'loaded')]


def _fw_load_all(folder):
    """Загрузка файлов в порядке зависимостей (__dependencies__ = [id, ...])."""
    items = {}
    for name in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
        if not name.endswith('.plugin'):
            continue
        try:
            with open(os.path.join(folder, name), encoding='utf8') as f:
                meta = json.loads(_moka_parse(f.read())).get('meta', {})
        except Exception:
            meta = {}
        deps = [d for d in (meta.get('__dependencies__') or []) if isinstance(d, str)] if isinstance(meta.get('__dependencies__'), (list, tuple)) else []
        items[name] = (meta.get('__id__') or name, deps)
    by_id = {pid: name for name, (pid, _d) in items.items()}
    order, seen = [], set()

    def visit(name, stack=()):
        if name in seen or name in stack:
            return
        for dep in items[name][1]:
            if dep in by_id:
                visit(by_id[dep], stack + (name,))
        seen.add(name)
        order.append(name)
    for name in items:
        visit(name)
    for name in order:
        _moka_load_file(name)


# =====================================================================================
# Этап 5: события чатов из сырых обновлений (chat_action, read, story, album)
# =====================================================================================

_FW_UPDATE_NAMES = ('updateNewMessage', 'updateNewChannelMessage', 'updateReadHistoryInbox', 'updateReadHistoryOutbox',
                    'updateReadChannelInbox', 'updateReadChannelOutbox', 'updateStory')
_FW_UPDATE_EVENTS = ('chat_action', 'read', 'story')
_fw_albums = {}
_fw_album_lock = threading.Lock()


def _fw_sync_updates():
    """Подписывает каркас на нужные обновления, если хоть один плагин слушает chat_action/read/story."""
    base = sys.modules['mokagram_sdk.base_plugin'].BasePlugin
    want = False
    for entry in _plugins.values():
        inst = entry.get('plugin')
        if inst is None:
            continue
        for ev in _FW_UPDATE_EVENTS:
            if getattr(type(inst), 'on_' + ev, None) is not getattr(base, 'on_' + ev, None):
                want = True
    ids = {_MOKA_TL[n] for n in _FW_UPDATE_NAMES if n in _MOKA_TL} if want else set()
    pseudo = {'id': '_fw', 'filters': {}}
    if ids != _fw_updates_on[0]:
        _fw_updates_on[0] = ids
        _push_filters(pseudo, {0: ids} if ids else {})


_fw_updates_on = [set()]


def _fw_peer_str(p):
    if not isinstance(p, dict):
        return None
    if p.get('_') == 'peerUser':
        return str(p['user_id'])
    if p.get('_') == 'peerChat':
        return str(p['chat_id'] + (1 << 48))
    if p.get('_') == 'peerChannel':
        return str(p['channel_id'] + (1 << 49))
    return None


def _fw_emit(name, payload, perm='messages.read'):
    hook = 'on_' + name
    base = sys.modules['mokagram_sdk.base_plugin'].BasePlugin
    for entry in list(_plugins.values()):
        inst = entry.get('plugin')
        if inst is None or hook in entry.get('hooks_off', ()):
            continue
        fn = getattr(inst, hook, None)
        if fn is None or getattr(type(inst), hook, None) is getattr(base, hook, None):
            continue
        if not _granted(entry['id'], perm):
            continue
        try:
            _fw_call(entry, fn, dict(payload), 5.0)
            _hook_ok(entry, hook)
        except BaseException as e:
            _hook_error(entry, hook, e)


def _fw_observe(kind, ctor, raw):
    """Из _moka_hook_observe: разбираем обновления для chat_action / read / story."""
    if kind != 0 or not _fw_updates_on[0] or ctor not in _fw_updates_on[0]:
        return
    try:
        u = sys.modules['mokagram_sdk.tl'].decode_object(raw)
    except Exception:
        return
    name = u.get('_')
    if name in ('updateNewMessage', 'updateNewChannelMessage'):
        m = u.get('message') or {}
        if m.get('_') != 'messageService':
            return
        act = m.get('action') or {}
        a = act.get('_', '')
        who = _fw_peer_str(m.get('from_id'))
        users = [str(x) for x in (act.get('users') or [])] + ([str(act['user_id'])] if act.get('user_id') else [])
        kind_name = {'messageActionChatJoinedByLink': 'join', 'messageActionChatJoinedByRequest': 'join',
                     'messageActionPinMessage': 'pin', 'messageActionChatEditTitle': 'title',
                     'messageActionChatEditPhoto': 'photo', 'messageActionChatCreate': 'create',
                     'messageActionChannelCreate': 'create', 'messageActionChatDeletePhoto': 'photo_deleted'}.get(a)
        if a == 'messageActionChatAddUser':
            kind_name = 'join' if (who and users and who == users[0]) else 'add'
        elif a == 'messageActionChatDeleteUser':
            kind_name = 'leave' if (who and users and who == users[0]) else 'remove'
        _fw_emit('chat_action', {
            'peer': _fw_peer_str(m.get('peer_id')), 'action': kind_name or 'other', 'raw_action': a, 'from': who,
            'user_ids': users, 'message_id': m.get('id'), 'date': m.get('date'),
            'title': act.get('title'), 'reply_to': (m.get('reply_to') or {}).get('reply_to_msg_id')})
    elif name in ('updateReadHistoryInbox', 'updateReadHistoryOutbox'):
        _fw_emit('read', {'peer': _fw_peer_str(u.get('peer')), 'max_id': u.get('max_id'),
                          'direction': 'inbox' if name.endswith('Inbox') else 'outbox'})
    elif name in ('updateReadChannelInbox', 'updateReadChannelOutbox'):
        _fw_emit('read', {'peer': str(u.get('channel_id', 0) + (1 << 49)), 'max_id': u.get('max_id'),
                          'direction': 'inbox' if name.endswith('Inbox') else 'outbox'})
    elif name == 'updateStory':
        st = u.get('story') or {}
        media = (st.get('media') or {}).get('_', '')
        _fw_emit('story', {'peer': _fw_peer_str(u.get('peer')), 'id': st.get('id'), 'date': st.get('date'),
                           'caption': st.get('caption', ''),
                           'media': 'photo' if 'Photo' in media else 'video' if 'Document' in media else media})


def _fw_album_collect(payload):
    gid = payload.get('grouped_id')
    if not gid:
        return
    key = (payload.get('peer'), gid)
    with _fw_album_lock:
        item = _fw_albums.setdefault(key, {'msgs': [], 'timer': None})
        item['msgs'].append(payload)
        if item['timer'] is not None:
            item['timer'].cancel()
        t = threading.Timer(1.3, _fw_album_flush, (key,))
        t.daemon = True
        item['timer'] = t
        t.start()


def _fw_album_flush(key):
    with _fw_album_lock:
        item = _fw_albums.pop(key, None)
    if not item:
        return
    msgs = sorted(item['msgs'], key=lambda m: int(m.get('id', 0)))
    _fw_emit('album', {'peer': key[0], 'grouped_id': key[1], 'messages': msgs,
                       'text': next((m['text'] for m in msgs if m.get('text')), ''), 'out': bool(msgs[0].get('out'))})


_FW_EXPORT = {}


def _fw_export():
    _FW_EXPORT.update({
        'job': _fw_job, 'http': _fw_http_request, 'pool': _fw_pool,
        'db_load': lambda: _fw_db_load(), 'db_set': _fw_db_set, 'db_subs': _fw_db_subs,
        'secret_get': _fw_secret_get, 'secret_set': _fw_secret_set,
        'lang': _fw_lang, 'plugins': _plugins, 'run_async': _fw_run_async,
    })


_fw_export()


# =====================================================================================
# Этап 6: бэкап и восстановление
# =====================================================================================

import zipfile

_FW_BACKUP_CLIENT = ('moka_settings.json', 'moka_notes.json', 'moka_snippets.json', 'mokagram_filters.json')
_FW_BACKUP_LIMIT = 30 << 20


def _fw_client_dir():
    try:
        return json.loads(_moka_native('client_data_dir', '{}')).get('dir', '')
    except Exception:
        return ''


@_fw_builtin('backup', 'бэкап плагинов, их данных и настроек одним файлом в Избранное')
def _fw_cmd_backup(ctx):
    def work():
        try:
            import tempfile
            stamp = time.strftime('%Y%m%d-%H%M')
            path = os.path.join(tempfile.mkdtemp(prefix='moka_backup_'), 'moka-backup-%s.zip' % stamp)
            total = 0
            with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
                z.writestr('manifest.json', json.dumps({'format': 1, 'created': stamp}))
                for name in sorted(os.listdir(_folder)) if os.path.isdir(_folder) else []:
                    if name.endswith('.plugin'):
                        z.write(os.path.join(_folder, name), 'plugins/' + name)
                root = os.path.join(_data_root(), 'plugin_data')
                for base, _dirs, files in os.walk(root):
                    for fn in files:
                        if fn in ('secrets.enc', 'secrets.key') or fn.endswith('.tmp'):
                            continue
                        full = os.path.join(base, fn)
                        total += os.path.getsize(full)
                        if total > _FW_BACKUP_LIMIT:
                            raise RuntimeError('данные плагинов больше 30 МБ')
                        z.write(full, 'plugin_data/' + os.path.relpath(full, root).replace(os.sep, '/'))
                cdir = _fw_client_dir()
                for fn in _FW_BACKUP_CLIENT:
                    full = os.path.join(cdir, fn)
                    if cdir and os.path.isfile(full):
                        z.write(full, 'client/' + fn)
            me = json.loads(_moka_native('get_me', '{}'))
            if not me.get('ok'):
                raise RuntimeError('нет открытой сессии')
            res = json.loads(_moka_native('send_file', json.dumps({
                'peer': os.environ.get('MOKAGRAM_BACKUP_PEER') or me['peer'], 'path': path, 'caption': 'Бэкап mokaGram %s. Секреты плагинов в него не входят.' % stamp,
                'kind': 'file', 'silent': True})))
            _fw_private('Бэкап', 'Отправлено в Избранное. Чтобы восстановить, нажмите на файл правой кнопкой и выберите «Восстановить из бэкапа».'
                        if res.get('ok') else 'Не вышло: %s' % res.get('error', ''))
        except BaseException as e:
            _fw_private('Бэкап', 'Не вышло: %s' % e)
    _fw_pool.submit(work)
    _fw_private('Бэкап', 'Собираю...')


def _moka_restore(path):
    """Вызывается из меню файла в Избранном. Проверяет архив и спрашивает подтверждение."""
    try:
        z = zipfile.ZipFile(path)
        names = z.namelist()
        total = sum(i.file_size for i in z.infolist())
        if total > 100 << 20 or 'manifest.json' not in names:
            raise ValueError('это не бэкап mokaGram')
        if json.loads(z.read('manifest.json')).get('format') != 1:
            raise ValueError('неизвестный формат бэкапа')
        for n in names:
            if n.startswith('/') or '..' in n.split('/') or '\\' in n:
                raise ValueError('в архиве небезопасные пути')
        plugins = [n for n in names if n.startswith('plugins/') and n.endswith('.plugin')]
    except Exception as e:
        _fw_private('Восстановление', 'Файл не подходит: %s' % e)
        return

    def go(button):
        if button != 'yes':
            return
        try:
            stage = os.path.join(_fw_client_dir(), 'restore-pending')
            import shutil
            shutil.rmtree(stage, ignore_errors=True)
            os.makedirs(stage, exist_ok=True)
            for n in names:
                if n.endswith('/'):
                    continue
                if n.startswith('client/') and n[7:] in _FW_BACKUP_CLIENT:
                    target = os.path.join(stage, n[7:])
                elif n.startswith('plugin_data/') and not n.endswith(('secrets.enc', 'secrets.key')):
                    target = os.path.join(stage, n)
                else:
                    continue
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with z.open(n) as src, open(target, 'wb') as dst:
                    dst.write(src.read())
            import tempfile
            tmp = tempfile.mkdtemp(prefix='moka_restore_')
            for n in plugins:
                p = os.path.join(tmp, os.path.basename(n))
                with z.open(n) as src, open(p, 'wb') as dst:
                    dst.write(src.read(1 << 20))
                _moka_native('install_plugin_file', json.dumps({'path': p}))
            _fw_private('Восстановление', 'Данные и настройки применятся при следующем запуске mokaGram. '
                        'Плагины (%d) устанавливаются через обычное окно с правами, подтвердите каждый.' % len(plugins))
        except BaseException as e:
            _fw_private('Восстановление', 'Не вышло: %s' % e)
    _fw_private('Восстановить из бэкапа?',
                'В архиве плагинов: %d. Данные плагинов, настройки, заметки, сниппеты и фильтры заменят текущие '
                '(старые файлы останутся рядом с суффиксом .bak). Плагины пройдут обычную установку с подтверждением прав.' % len(plugins),
                [{'id': 'yes', 'text': 'Восстановить', 'style': 'red'}, {'id': 'no', 'text': 'Отмена', 'style': 'cancel'}], go)
