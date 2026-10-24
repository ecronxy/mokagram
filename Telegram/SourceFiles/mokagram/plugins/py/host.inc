constexpr auto kHostScript = R"PY(
"""Mokagram: хост плагинов (исполняется во встроенном Python клиента).

Читает метаданные без запуска кода, загружает/выгружает плагины с изоляцией ошибок,
раздаёт хуки по цепочке. С C++ общается через строки JSON.
"""
import ast, collections, hashlib, json, os, re, sys, time, types, traceback

_MOKA_CAPS = {
    'network': {'requests', 'urllib', 'urllib3', 'http', 'socket', 'aiohttp', 'httpx', 'websocket', 'ssl', 'ftplib', 'smtplib'},
    'files': {'shutil', 'pathlib', 'glob', 'tempfile'},
    'native': {'ctypes', 'subprocess', 'multiprocessing', 'cffi'},
}


def _moka_parse(source):
    out = {'meta': {}, 'detected': [], 'error': None}
    try:
        tree = ast.parse(source)
    except SyntaxError as e:
        out['error'] = 'синтаксическая ошибка: %s' % e
        return json.dumps(out, ensure_ascii=False)
    meta = {}
    for node in tree.body:
        if (isinstance(node, ast.Assign) and len(node.targets) == 1
                and isinstance(node.targets[0], ast.Name)):
            key = node.targets[0].id
            if key.startswith('__') and key.endswith('__'):
                try:
                    meta[key] = ast.literal_eval(node.value)
                except Exception:
                    pass
    detected = set()
    for node in ast.walk(tree):
        mods = []
        if isinstance(node, ast.Import):
            mods = [a.name.split('.')[0] for a in node.names]
        elif isinstance(node, ast.ImportFrom):
            mods = [(node.module or '').split('.')[0]]
        for m in mods:
            for cap, names in _MOKA_CAPS.items():
                if m in names:
                    detected.add(cap)
        if isinstance(node, ast.Call):
            f = node.func
            if isinstance(f, ast.Name):
                if f.id == 'open':
                    detected.add('files')
                if f.id in ('exec', 'eval', '__import__', 'compile'):
                    detected.add('native')
            elif isinstance(f, ast.Attribute):
                if f.attr in ('system', 'popen'):
                    detected.add('native')
                if f.attr in ('send_message', 'send_file'):
                    detected.add('messages.send')
                if f.attr in ('get_message', 'chat_info'):
                    detected.add('messages.read')
                if f.attr == 'edit_message':
                    detected.add('messages.edit')
    out['meta'] = meta
    out['detected'] = sorted(detected)
    return json.dumps(out, ensure_ascii=False)


# ---- журнал: print и ошибки плагинов собираются здесь, C++ забирает через _moka_drain ----

_log = collections.deque()


class _LogOut:
    def __init__(self):
        self._buf = ''

    def write(self, text):
        self._buf += text
        while '\n' in self._buf:
            line, self._buf = self._buf.split('\n', 1)
            _log.append(line)
        return len(text)

    def flush(self):
        pass


sys.stdout = _LogOut()
sys.stderr = _LogOut()


def _moka_drain():
    # Журнал пишут из нескольких потоков: забираем по одной записи, без гонки на списке.
    out = []
    while True:
        try:
            out.append(_log.popleft())
        except IndexError:
            break
    return '\n'.join(out)


def _say(text):
    _log.append(text)


# ---- SDK ----

def _moka_install_sdk(source):
    pkg = types.ModuleType('mokagram_sdk')
    pkg.__path__ = []
    mod = types.ModuleType('mokagram_sdk.base_plugin')
    sys.modules['mokagram_sdk'] = pkg
    sys.modules['mokagram_sdk.base_plugin'] = mod
    sys.modules['base_plugin'] = mod   # exteraGram-плагины импортируют именно так
    exec(compile(source, 'mokagram_sdk/base_plugin.py', 'exec'), mod.__dict__)
    pkg.base_plugin = mod


def _moka_install_api(source):
    mod = types.ModuleType('mokagram_sdk.api')
    exec(compile(source, 'mokagram_sdk/api.py', 'exec'), mod.__dict__)
    mod._h.update({
        'caller': _caller, 'granted': _granted, 'audit': _audit, 'say': _say,
        'native': _moka_native, 'data_dir': _data_dir, 'register_cb': _register_cb, 'hook_error': _hook_error,
    })
    sys.modules['mokagram_sdk.api'] = mod
    sys.modules['moka'] = mod
    sys.modules['mokagram_sdk'].api = mod


# ---- права, хеш плагина, журнал действий ----

# L0 доступно всегда. L4 в обычном режиме не выдаётся никогда (режим доверия не сделан).
_L0 = {'ui', 'storage', 'settings', 'background'}
_L4 = {'raw_api', 'native', 'files.any', 'network.any', 'subprocess'}
_grants = {}   # id -> {'hash': sha256 файла, 'grants': [права]}


def _data_root():
    return os.path.dirname(_folder.rstrip('/'))


def _grants_path():
    return os.path.join(_data_root(), 'plugin_grants.json')


def _data_dir(pid):
    safe = re.sub(r'[^A-Za-z0-9_.-]', '_', pid)
    return os.path.join(_data_root(), 'plugin_data', safe)


def _load_grants():
    try:
        with open(_grants_path(), encoding='utf8') as f:
            data = json.load(f)
        if isinstance(data, dict):
            _grants.update(data)
    except Exception:
        pass


def _save_grants():
    try:
        os.makedirs(_data_root(), exist_ok=True)
        tmp = _grants_path() + '.tmp'
        with open(tmp, 'w', encoding='utf8') as f:
            json.dump(_grants, f, ensure_ascii=False, indent=1, sort_keys=True)
        os.replace(tmp, _grants_path())
    except Exception as e:
        _say('не удалось сохранить права плагинов: %s' % e)


def _sha256(raw):
    return hashlib.sha256(raw).hexdigest()


def _default_grants(meta):
    # Выдаём то, что плагин запросил в __permissions__, кроме опасного уровня L4.
    declared = meta.get('__permissions__') or []
    out = []
    for item in declared if isinstance(declared, (list, tuple)) else []:
        if isinstance(item, str) and item not in _L4 and item not in out:
            out.append(item)
    return out


def _granted(pid, perm):
    if perm in _L0:
        return True
    have = _grants.get(pid, {}).get('grants', [])
    if perm in _L4:
        return False   # L4 только в режиме доверия (его пока нет)
    parts = perm.split('.')
    for i in range(1, len(parts) + 1):
        if '.'.join(parts[:i]) in have:
            return True
    return False


def _audit(entry, op, target, decision, size=0):
    # Строку JSON дописывает C++ (время, ротация 5 МБ x 3). Плагин сюда не пишет.
    try:
        rec = _grants.get(entry['id'], {})
        line = json.dumps({
            'plugin': entry['id'], 'hash': (rec.get('hash') or '')[:12], 'op': op,
            'target': str(target)[:200], 'decision': decision, 'size': size,
        }, ensure_ascii=False)
        _moka_native('audit', line)
    except Exception:
        pass


def _caller():
    # Какой плагин зовёт SDK: ищем в стеке кадр из файла плагина (имя файла задано в compile),
    # работает и из чужих потоков. По __name__ нельзя: плагины сами задают __name__ в метаданных.
    f = sys._getframe(1)
    while f is not None:
        path = f.f_code.co_filename
        for entry in list(_plugins.values()):
            if entry.get('path') == path:
                return entry
        f = f.f_back
    return None


def _toast(text):
    try:
        _moka_native('toast', json.dumps({'text': text[:200], 'duration': 4.0}, ensure_ascii=False))
    except Exception:
        pass


def _record(entry_name, raw, meta):
    pid = meta.get('__id__') or entry_name
    _grants[pid] = {'hash': _sha256(raw), 'grants': _default_grants(meta)}
    _save_grants()
    return pid


def _moka_record_install(name):
    """Установка из окна клиента: запоминаем хеш файла и выданные права."""
    try:
        with open(os.path.join(_folder, name), 'rb') as f:
            raw = f.read()
        meta = json.loads(_moka_parse(raw.decode('utf8')))['meta']
        pid = _record(name, raw, meta)
        _say('[%s] установлен, хеш %s' % (pid, _grants[pid]['hash'][:12]))
        return pid
    except Exception as e:
        _say('не удалось записать хеш %s: %s' % (name, e))
        return ''


def _moka_approve(pid):
    """Пользователь подтвердил изменённый файл: новый хеш и права, запуск."""
    entry = _plugins.get(pid)
    if entry is None:
        return 'false'
    try:
        with open(entry['path'], 'rb') as f:
            raw = f.read()
        meta = json.loads(_moka_parse(raw.decode('utf8')))['meta']
        _record(entry['file'], raw, meta)
    except Exception as e:
        _say('[%s] не удалось подтвердить: %s' % (pid, e))
        return 'false'
    _audit(entry, 'review', '', 'approved')
    _moka_load_file(entry['file'])
    return 'true' if _plugins[pid]['state'] == 'loaded' else 'false'


# ---- реестр плагинов ----

_folder = ''
_plugins = {}      # id -> запись; порядок вставки = порядок вызова хуков
_disabled = set()
_pinned = set()
_engine = True       # общий выключатель системы плагинов


def _state_path():
    return os.path.join(os.path.dirname(_folder.rstrip('/')), 'plugins_state.json')


def _load_state():
    try:
        with open(_state_path(), encoding='utf8') as f:
            data = json.load(f)
        _disabled.update(data.get('disabled', []))
        _pinned.update(data.get('pinned', []))
        global _engine
        _engine = bool(data.get('engine', True))
    except Exception:
        pass


def _save_state():
    try:
        with open(_state_path(), 'w', encoding='utf8') as f:
            json.dump({'disabled': sorted(_disabled), 'engine': _engine, 'pinned': sorted(_pinned)}, f, ensure_ascii=False)
    except Exception as e:
        _say('не удалось сохранить состояние плагинов: %s' % e)


def _fail(entry, exc):
    entry['plugin'] = None
    entry['module'] = None
    entry['state'] = 'error'
    entry['error'] = ''.join(traceback.format_exception_only(type(exc), exc)).strip()
    _say('[%s] ошибка: %s' % (entry['id'], entry['error']))
    _say(traceback.format_exc().rstrip())


def _activate(entry, source):
    try:
        base = sys.modules['mokagram_sdk.base_plugin'].BasePlugin
        mod = types.ModuleType('moka_plugin_' + entry['file'].replace('.', '_'))
        mod.__file__ = entry['path']
        exec(compile(source, entry['path'], 'exec'), mod.__dict__)
        cls = None
        for value in list(mod.__dict__.values()):
            if (isinstance(value, type) and issubclass(value, base)
                    and value is not base and value.__module__ == mod.__name__):
                cls = value
        if cls is None:
            raise RuntimeError('в файле нет класса, наследующего BasePlugin')
        inst = cls()
        inst.on_plugin_load()
        entry['plugin'] = inst
        entry['module'] = mod
        entry['state'] = 'loaded'
        entry['error'] = None
        entry['hooks_off'] = set()
        entry['streak'] = {}
        entry['errors'] = collections.deque()
        entry['slow'] = {}
        _register_settings(entry)
        _register_filters(entry)
        _register_events(entry)
    except BaseException as e:   # включая SystemExit: плагин не должен ронять клиент
        _fail(entry, e)


# ---- события клиента (новое, изменённое, удалённое сообщение) ----

_EVENTS = ('new_message', 'message_edited', 'message_deleted')


def _register_events(entry):
    inst = entry['plugin']
    base = sys.modules['mokagram_sdk.base_plugin'].BasePlugin
    names = [e for e in _EVENTS
             if getattr(type(inst), 'on_' + e, None) is not getattr(base, 'on_' + e, None)]
    if names and not _granted(entry['id'], 'messages.read'):
        _say('[%s] события сообщений: нет права messages.read, не подписан' % entry['id'])
        _audit(entry, 'events.subscribe', ','.join(names), 'denied')
        names = []
    elif names:
        _audit(entry, 'events.subscribe', ','.join(names), 'allowed')
    entry['events'] = set(names)
    _moka_native('events_subscribe', json.dumps({'pid': entry['id'], 'events': names}))


def _moka_event(name, payload_json):
    """Рабочий поток C++: доставка события плагинам, результат игнорируется."""
    try:
        payload = json.loads(payload_json)
    except Exception:
        return ''
    hook = 'on_' + name
    for entry in list(_plugins.values()):
        inst = entry.get('plugin')
        if inst is None or hook in entry.get('hooks_off', ()):
            continue
        if name not in entry.get('events', ()):
            continue
        fn = getattr(inst, hook, None)
        if not callable(fn):
            continue
        started = time.monotonic()
        try:
            fn(dict(payload))
            _hook_ok(entry, hook)
        except BaseException as e:
            _hook_error(entry, hook, e)
        spent = time.monotonic() - started
        if spent > _OBSERVE_BUDGET:
            _hook_slow(entry, hook, spent, _OBSERVE_BUDGET)
    return ''


# ---- обратные вызовы клиента в плагин: диалоги, тосты с кнопкой, кнопки интерфейса ----

_cb_counter = [0]


def _register_cb(entry, fn, once=False):
    if not callable(fn):
        return ''
    _cb_counter[0] += 1
    token = 'c%d' % _cb_counter[0]
    entry.setdefault('cbs', {})[token] = (fn, bool(once))
    return token


def _moka_invoke(pid, token, payload_json):
    entry = _plugins.get(pid)
    if entry is None or entry.get('plugin') is None:
        return 'false'
    item = entry.get('cbs', {}).get(token)
    if item is None:
        return 'false'
    fn, once = item
    if once:
        entry['cbs'].pop(token, None)
    try:
        payload = json.loads(payload_json) if payload_json else {}
        fn(payload)
    except BaseException as e:
        _hook_error(entry, 'callback', e)
        return 'false'
    return 'true'


def _deactivate(entry):
    try:
        _moka_native('plugin_unloaded', json.dumps({'pid': entry['id']}))
    except BaseException:
        pass
    entry['cbs'] = {}
    for t in list(entry.get('timers', {}).values()):
        t.cancel()
    entry['timers'] = {}
    entry['events'] = set()
    _clear_filters(entry)
    inst = entry.get('plugin')
    if inst is not None:
        try:
            inst.on_plugin_unload()
        except BaseException as e:
            _say('[%s] ошибка при выгрузке: %s' % (entry['id'], e))
    entry['plugin'] = None
    entry['module'] = None


def _moka_load_file(name):
    path = os.path.join(_folder, name)
    try:
        with open(path, 'rb') as f:
            raw = f.read()
        source = raw.decode('utf8')
    except Exception as e:
        _say('не прочитать %s: %s' % (name, e))
        return ''
    info = json.loads(_moka_parse(source))
    meta = info['meta']
    pid = meta.get('__id__') or name
    old = _plugins.get(pid)
    if old is not None:
        _deactivate(old)
    entry = {'id': pid, 'file': name, 'path': path, 'meta': meta,
             'plugin': None, 'module': None, 'state': 'error', 'error': info['error']}
    _plugins[pid] = entry
    if info['error'] or not meta.get('__id__'):
        entry['error'] = info['error'] or 'нет поля __id__'
    elif pid in _disabled:
        entry['state'] = 'disabled'
        entry['error'] = None
    elif not _engine:
        entry['state'] = 'paused'
        entry['error'] = None
    else:
        digest = _sha256(raw)
        rec = _grants.get(pid)
        if rec is None:
            # Плагин поставлен до появления защиты: верим текущему файлу и запоминаем хеш.
            _record(name, raw, meta)
            _say('[%s] хеш записан впервые: %s' % (pid, digest[:12]))
            _activate(entry, source)
        elif rec.get('hash') != digest:
            entry['state'] = 'needs_review'
            entry['error'] = None
            _say('[%s] файл изменился после установки, плагин не запущен до подтверждения' % pid)
            _audit(entry, 'load', '', 'needs_review')
        else:
            _activate(entry, source)
    return pid


def _moka_init(folder, sdk_source, api_source):
    global _folder
    _folder = folder
    _install_sdk = _moka_install_sdk
    _install_sdk(sdk_source)
    _moka_install_api(api_source)
    _load_state()
    _load_grants()
    if os.path.isdir(folder):
        for name in sorted(os.listdir(folder)):
            if name.endswith('.plugin'):
                _moka_load_file(name)
    return len(_plugins)


def _moka_list():
    out = []
    for e in _plugins.values():
        m = e['meta']
        out.append({
            'id': e['id'], 'file': e['file'], 'state': e['state'], 'error': e['error'],
            'enabled': e['id'] not in _disabled,
            'pinned': e['id'] in _pinned,
            'name': m.get('__name__', e['id']), 'description': m.get('__description__', ''),
            'author': m.get('__author__', ''), 'version': m.get('__version__', ''),
            'icon': m.get('__icon__', ''), 'permissions': m.get('__permissions__', []),
        })
    out.sort(key=lambda x: not x['pinned'])
    return json.dumps(out, ensure_ascii=False)


def _moka_set_enabled(pid, enabled):
    entry = _plugins.get(pid)
    if entry is None:
        return 'false'
    if enabled == '1':
        _disabled.discard(pid)
        _moka_load_file(entry['file'])
    else:
        _disabled.add(pid)
        _deactivate(entry)
        entry['state'] = 'disabled'
        entry['error'] = None
    _save_state()
    return 'true'


def _moka_set_pinned(pid, flag):
    if pid not in _plugins:
        return 'false'
    if flag == '1':
        _pinned.add(pid)
    else:
        _pinned.discard(pid)
    _save_state()
    return 'true'


def _moka_engine():
    return '1' if _engine else '0'


def _moka_set_engine(flag):
    global _engine
    on = (flag == '1')
    if on == _engine:
        return 'true'
    _engine = on
    for entry in list(_plugins.values()):
        if on:
            _moka_load_file(entry['file'])
        else:
            _deactivate(entry)
            if entry['id'] not in _disabled:
                entry['state'] = 'paused'
            entry['error'] = None
    _save_state()
    return 'true'


def _moka_remove(pid):
    entry = _plugins.pop(pid, None)
    if entry is None:
        return 'false'
    _deactivate(entry)
    _disabled.discard(pid)
    _save_state()
    if _grants.pop(pid, None) is not None:
        _save_grants()
    try:
        os.remove(entry['path'])
    except Exception as e:
        _say('не удалось удалить файл: %s' % e)
    return 'true'


def _moka_shutdown():
    for entry in list(_plugins.values()):
        _deactivate(entry)
    return ''


# ---- настройки плагина: create_settings() -> реестр настроек клиента ----

def _register_settings(entry):
    inst = entry['plugin']
    entry['settings'] = {}
    try:
        items = inst.create_settings() or []
    except BaseException as e:
        _say('[%s] ошибка в create_settings: %s' % (entry['id'], e))
        return
    section = str(entry['meta'].get('__name__') or entry['id'])
    table = {}
    out = []
    for it in items:
        cls = type(it).__name__
        key = getattr(it, 'key', None)
        if cls not in ('Switch', 'Selector', 'Input', 'EditText') or not isinstance(key, str) or not key or len(key) > 64:
            continue
        item = {
            'key': 'plugin.%s.%s' % (entry['id'], key), 'section': section,
            'title': str(getattr(it, 'text', '') or key)[:120],
            'description': str(getattr(it, 'subtext', '') or '')[:200],
        }
        if cls == 'Switch':
            item['kind'] = 'bool'
            item['default'] = bool(it.default)
        elif cls == 'Selector':
            choices = [str(x)[:60] for x in (it.items or [])][:50]
            if not choices:
                continue
            item['kind'] = 'choice'
            item['choices'] = choices
            item['default'] = it.default if isinstance(it.default, int) and 0 <= it.default < len(choices) else 0
        else:
            item['kind'] = 'string'
            item['default'] = str(it.default or '')[:500]
        table[key] = {'on_change': getattr(it, 'on_change', None)}
        out.append(item)
    entry['settings'] = table
    if out:
        res = json.loads(_moka_native('settings_register', json.dumps({'entries': out}, ensure_ascii=False)))
        if not res.get('ok'):
            _say('[%s] настройки не зарегистрированы: %s' % (entry['id'], res.get('error')))


def _moka_settings_changed(key, value_json):
    """Клиент сообщает, что пользователь поменял настройку plugin.<id>.<key>."""
    try:
        value = json.loads(value_json)[0]
    except Exception:
        return ''
    for entry in list(_plugins.values()):
        prefix = 'plugin.%s.' % entry['id']
        inst = entry.get('plugin')
        if inst is None or not key.startswith(prefix):
            continue
        short = key[len(prefix):]
        try:
            inst.on_settings_changed(short, value)
            cb = entry.get('settings', {}).get(short, {}).get('on_change')
            if callable(cb):
                cb(value)
        except BaseException as e:
            _hook_error(entry, 'on_settings_changed', e)
    return ''


# ---- подписка на хуки обновлений и запросов (фильтр по constructor id живёт в C++) ----

_HOOKS = ('on_update_hook', 'on_updates_hook', 'pre_request_hook', 'post_request_hook')
_KINDS = ('update', 'updates', 'pre', 'post')


def _push_filters(entry, lists):
    # Формат для C++ без JSON: первая строка id плагина, дальше «номер вида id id id».
    text = entry['id'] + '\n' + '\n'.join(
        '%d %s' % (k, ' '.join(str(i) for i in sorted(ids))) for k, ids in lists.items())
    _moka_native('filters', text)


def _clear_filters(entry):
    if entry.get('filters'):
        entry['filters'] = {}
        _push_filters(entry, {})


def _register_filters(entry):
    inst = entry['plugin']
    base = sys.modules['mokagram_sdk.base_plugin'].BasePlugin
    raw = getattr(entry['module'], '__hook_filters__', None) or getattr(inst, '__hook_filters__', None) or {}
    if not isinstance(raw, dict):
        raw = {}
    lists = {}
    for kind, hook in enumerate(_HOOKS):
        fn = getattr(type(inst), hook, None)
        if fn is None or fn is getattr(base, hook, None):
            continue   # хук не переопределён
        if not _granted(entry['id'], 'hooks.observe'):
            _say('[%s] %s: нет права hooks.observe, хук не подписан' % (entry['id'], hook))
            _audit(entry, hook, '', 'denied')
            continue
        names = raw.get(hook)
        if not names or not isinstance(names, (list, tuple)):
            _say('[%s] %s: нужен __hook_filters__ с именами обновлений или методов' % (entry['id'], hook))
            continue
        ids = set()
        for n in names:
            if n == '*':
                _say('[%s] %s: подписка на всё запрещена' % (entry['id'], hook))
            elif isinstance(n, int) and 0 < n <= 0xffffffff:
                ids.add(n)
            elif isinstance(n, str) and n in _MOKA_TL:
                ids.add(_MOKA_TL[n])
            else:
                _say('[%s] %s: неизвестное имя %r' % (entry['id'], hook, n))
        if ids:
            lists[kind] = ids
    entry['filters'] = lists
    if lists:
        _push_filters(entry, lists)
        _audit(entry, 'hooks.subscribe', ','.join(_HOOKS[k] for k in lists), 'allowed')


# ---- защита от падений: счётчики ошибок и просрочек ----

def _trip(entry, reason):
    _deactivate(entry)
    entry['state'] = 'error'
    entry['error'] = reason
    _say('[%s] отключён: %s' % (entry['id'], reason))
    _audit(entry, 'disable', '', reason)
    _toast('Плагин «%s» отключён: %s' % (entry['meta'].get('__name__') or entry['id'], reason))


def _hook_error(entry, hook, exc):
    _say('[%s] ошибка в %s: %s' % (entry['id'], hook, exc))
    _say(traceback.format_exc().rstrip())
    errors = entry.get('errors')
    streak = entry.get('streak')
    if errors is None or streak is None:
        return
    now = time.monotonic()
    errors.append(now)
    while errors and now - errors[0] > 60:
        errors.popleft()
    streak[hook] = streak.get(hook, 0) + 1
    if len(errors) >= 20:
        _trip(entry, '20 ошибок за минуту')
    elif streak[hook] >= 5:
        entry['hooks_off'].add(hook)
        _say('[%s] хук %s выключен: 5 ошибок подряд' % (entry['id'], hook))
        _toast('Хук %s плагина «%s» выключен из-за ошибок' % (hook, entry['meta'].get('__name__') or entry['id']))


def _hook_ok(entry, hook):
    streak = entry.get('streak')
    if streak is not None:
        streak[hook] = 0


def _hook_slow(entry, hook, spent, budget):
    """Просрочка бюджета: 3 раза за минуту выключают хук. Прервать Python-код в потоке нельзя, только отключить."""
    slow = entry.setdefault('slow', {}).setdefault(hook, collections.deque())
    now = time.monotonic()
    slow.append(now)
    while slow and now - slow[0] > 60:
        slow.popleft()
    _say('[%s] %s работал %d мс при бюджете %d мс' % (entry['id'], hook, spent * 1000, budget * 1000))
    if len(slow) >= 3 and hook not in entry.get('hooks_off', ()):
        entry['hooks_off'].add(hook)
        _say('[%s] хук %s выключен: 3 просрочки за минуту' % (entry['id'], hook))
        _toast('Хук %s плагина «%s» выключен: слишком медленный' % (hook, entry['meta'].get('__name__') or entry['id']))


class _TLRaw:
    """Сырой TL-объект: разбора полей пока нет, есть имя, constructor id и байты."""

    def __init__(self, constructor, raw):
        self.constructor = constructor
        self.raw = raw
        self.name = _TL_NAMES.get(constructor) or ('0x%08x' % constructor)

    def to_bytes(self):
        return self.raw

    def to_dict(self):
        return {'_': self.name, 'constructor': self.constructor, 'raw': self.raw.hex()}

    def __repr__(self):
        return '<TL %s %d байт>' % (self.name, len(self.raw))


_TL_NAMES = {v: k for k, v in _MOKA_TL.items()}
_RPC_ERROR = _MOKA_TL.get('rpc_error', 0x2144ca19)


def _tl_error(raw):
    # rpc_error#2144ca19 error_code:int error_message:string
    try:
        code = int.from_bytes(raw[4:8], 'little', signed=True)
        n = raw[8]
        start = 9
        if n == 254:
            n = int.from_bytes(raw[9:12], 'little')
            start = 12
        return {'code': code, 'message': raw[start:start + n].decode('utf8', 'replace')}
    except Exception:
        return {'code': 0, 'message': ''}


_OBSERVE_BUDGET = 0.2   # секунд на плагин в рабочем потоке


def _moka_hook_observe(kind, ctor, hexdata):
    """Вызывается рабочим потоком C++. Наблюдение: результат плагина игнорируется."""
    try:
        kind = int(kind)
        ctor = int(ctor)
        hook = _HOOKS[kind]
        raw = bytes.fromhex(hexdata)
    except Exception as e:
        _say('хук: неверные данные: %s' % e)
        return ''
    for entry in list(_plugins.values()):
        inst = entry.get('plugin')
        ids = entry.get('filters', {}).get(kind)
        if inst is None or not ids or ctor not in ids or hook in entry.get('hooks_off', ()):
            continue
        fn = getattr(inst, hook, None)
        if not callable(fn):
            continue
        started = time.monotonic()
        try:
            if kind == 3:
                # ctor это метод запроса, в raw ответ сервера
                if raw[:4] == _RPC_ERROR.to_bytes(4, 'little'):
                    fn(None, _tl_error(raw))
                else:
                    fn(_TLRaw(int.from_bytes(raw[:4], 'little'), raw), None)
            else:
                fn(_TLRaw(ctor, raw))
            _hook_ok(entry, hook)
        except BaseException as e:
            _hook_error(entry, hook, e)
        spent = time.monotonic() - started
        if spent > _OBSERVE_BUDGET:
            _hook_slow(entry, hook, spent, _OBSERVE_BUDGET)
    return ''


# ---- хуки ----

_HOOK_BUDGETS = {'send_message_hook': 0.15}   # секунд, перехват в главном потоке


def _moka_hook(name, params_json):
    cur = json.loads(params_json)
    budget = _HOOK_BUDGETS.get(name, 0.15)
    for entry in list(_plugins.values()):
        inst = entry.get('plugin')
        fn = getattr(inst, name, None) if inst is not None else None
        if fn is None or name in entry.get('hooks_off', ()):
            continue
        started = time.monotonic()
        try:
            result = fn(cur)
            _hook_ok(entry, name)
        except BaseException as e:
            _hook_error(entry, name, e)
            continue
        spent = time.monotonic() - started
        if spent > budget:
            # Не уложился в бюджет: результат отбрасываем, данные идут дальше как были.
            _hook_slow(entry, name, spent, budget)
            continue
        if result is None:
            continue
        strategy = getattr(result, 'strategy', None)
        strategy = str(getattr(strategy, 'value', strategy))
        if strategy == 'CANCEL':
            return json.dumps({'cancel': True}, ensure_ascii=False)
        if strategy in ('MODIFY', 'MODIFY_FINAL'):
            value = getattr(result, 'value', None)
            if value is not None:
                cur = value
            if strategy == 'MODIFY_FINAL':
                break
    return json.dumps({'cancel': False, 'value': cur}, ensure_ascii=False, default=str)


# ---- пункты меню ----

_LOCATIONS = {
    'MESSAGE_CONTEXT_MENU': 'message',
    'DRAWER_MENU': 'drawer',
    'CHAT_ACTION_MENU': 'chat',
    'PROFILE_ACTION_MENU': 'profile',
    'ATTACH_MENU': 'attach',
    'MAIN_MENU': 'drawer',
}


def _item_location(item):
    loc = getattr(item, 'location', None)
    if loc is None:
        loc = getattr(item, 'menu_type', None)
    loc = str(getattr(loc, 'value', loc))
    return _LOCATIONS.get(loc, loc)


def _item_action(item):
    return getattr(item, 'action', None) or getattr(item, 'on_click', None)


def _moka_menu_items(location, ctx_json):
    ctx = json.loads(ctx_json)
    out = []
    for entry in list(_plugins.values()):
        inst = entry.get('plugin')
        if inst is None:
            continue
        for index, item in enumerate(getattr(inst, '_menu_items', [])):
            if _item_location(item) != location or getattr(item, '_removed', False):
                continue
            cond = getattr(item, 'condition', None)
            if callable(cond):
                try:
                    if not cond(ctx):
                        continue
                except BaseException as e:
                    _say('[%s] ошибка в condition: %s' % (entry['id'], e))
                    continue
            out.append({'pid': entry['id'], 'index': index, 'text': str(getattr(item, 'text', '')),
                        'icon': str(getattr(item, 'icon', '') or ''),
                        'priority': int(getattr(item, 'priority', 0) or 0)})
    out.sort(key=lambda e: -e['priority'])
    return json.dumps(out, ensure_ascii=False)


def _moka_menu_click(pid, index, ctx_json):
    entry = _plugins.get(pid)
    if entry is None or entry.get('plugin') is None:
        return 'false'
    try:
        item = entry['plugin']._menu_items[int(index)]
        action = _item_action(item)
        if callable(action):
            action(json.loads(ctx_json))
    except BaseException as e:
        _hook_error(entry, 'menu_click', e)
        return 'false'
    return 'true'
)PY";
