"""Mokagram SDK: вызовы плагина к клиенту (mokagram_sdk.api, он же moka).

Каждый вызов проверяет право плагина (host.py знает, кто вызвал, по модулю в стеке),
пишет решение в журнал действий и только потом просит клиент выполнить действие.
Нет права: запись в журнал и None, плагин не падает.
"""
import json, os, time, collections, threading

_h = {}   # функции хоста, кладёт host.py: caller, granted, audit, say, native, data_dir


def _native(op, args):
    try:
        return json.loads(_h['native'](op, json.dumps(args, ensure_ascii=False)))
    except Exception as e:
        return {'ok': False, 'error': 'сбой вызова клиента: %s' % e}


def _gate(op, perm, target=''):
    entry = _h['caller']()
    if entry is None:
        _h['say']('SDK-вызов %s не из плагина, отказ' % op)
        return None
    if not _h['granted'](entry['id'], perm):
        _h['audit'](entry, op, target, 'denied')
        _h['say']('[%s] нет права %s для %s' % (entry['id'], perm, op))
        return None
    return entry


def _limit(entry, name, count, window):
    # Скользящее окно: не больше count вызовов за window секунд.
    q = entry.setdefault('rate', {}).setdefault(name, collections.deque())
    now = time.monotonic()
    while q and now - q[0] > window:
        q.popleft()
    if len(q) >= count:
        return False
    q.append(now)
    return True


# ---- интерфейс ----

def show_toast(text, icon=None, duration=2.5):
    entry = _gate('show_toast', 'ui')
    if entry is None:
        return None
    if not _limit(entry, 'toast', 10, 10):
        _h['say']('[%s] слишком много тостов, пропущен' % entry['id'])
        return None
    try:
        sec = min(max(float(duration), 1.0), 10.0)
    except Exception:
        sec = 2.5
    res = _native('toast', {'text': str(text)[:200], 'duration': sec})
    _h['audit'](entry, 'show_toast', '', 'allowed' if res.get('ok') else 'failed')
    return bool(res.get('ok'))


# ---- свои окна, формы и тосты с кнопкой ----

_FIELD_TYPES = {'Header': 'header', 'Text': 'text', 'Switch': 'switch', 'Selector': 'select', 'Input': 'input',
                'EditText': 'input', 'Divider': 'divider', 'Progress': 'progress'}


def _field_dict(f):
    if isinstance(f, dict):
        d = dict(f)
    else:
        d = {k: getattr(f, k) for k in ('key', 'text', 'default', 'items', 'subtext', 'multiline', 'hint')
             if hasattr(f, k)}
        d['type'] = _FIELD_TYPES.get(type(f).__name__, 'text')
    d['type'] = str(d.get('type', 'text'))
    d['text'] = str(d.get('text', d.get('hint', '')))[:300]
    d['key'] = str(d.get('key', ''))[:64]
    if d['type'] == 'select':
        d['items'] = [str(i)[:100] for i in (d.get('items') or [])][:50]
    default = d.get('default')
    d['default'] = default if isinstance(default, (bool, int, float, str)) else ''
    return {k: d[k] for k in ('type', 'key', 'text', 'default', 'items', 'multiline') if k in d}


def show_dialog(title, message='', fields=None, buttons=None, on_result=None, on_close=None):
    """Своё окно. fields: Header/Text/Switch/Selector/Input из base_plugin или dict
    {'type': header|text|input|switch|select|progress, 'key', 'text', 'default', 'items', 'multiline'}.
    buttons: [{'id': 'ok', 'text': 'Готово', 'style': 'primary'|'cancel'|'red', 'keep_open': False}].
    on_result(button_id, values) зовётся при нажатии; on_close() когда окно закрыли.
    Возвращает id окна для update_dialog / close_dialog."""
    entry = _gate('show_dialog', 'ui')
    if entry is None:
        return None
    if not _limit(entry, 'dialog', 10, 60):
        _h['say']('[%s] слишком много окон, пропущено' % entry['id'])
        return None
    btns = []
    for b in (buttons or [{'id': 'ok', 'text': 'Ок'}])[:4]:
        b = dict(b) if isinstance(b, dict) else {'id': str(b), 'text': str(b)}
        btns.append({'id': str(b.get('id', b.get('text', 'ok')))[:32], 'text': str(b.get('text', 'Ок'))[:40],
                     'style': str(b.get('style', 'primary')), 'keep_open': bool(b.get('keep_open', False))})

    def _result(payload):
        if on_result:
            on_result(payload.get('button'), payload.get('values') or {})

    def _closed(payload):
        if on_close:
            on_close()

    res = _native('dialog_show', {
        'pid': entry['id'], 'title': str(title)[:120], 'message': str(message or '')[:4000],
        'fields': [_field_dict(f) for f in (fields or [])[:30]], 'buttons': btns,
        'cb': _h['register_cb'](entry, _result), 'close_cb': _h['register_cb'](entry, _closed, True)})
    return res.get('id') if res.get('ok') else None


def update_dialog(dialog_id, message=None, progress=None, title=None):
    """Меняет открытое окно: текст, заголовок, полоса прогресса (0..100)."""
    entry = _gate('update_dialog', 'ui')
    if entry is None:
        return None
    args = {'id': dialog_id}
    if message is not None:
        args['message'] = str(message)[:4000]
    if title is not None:
        args['title'] = str(title)[:120]
    if progress is not None:
        args['progress'] = max(0.0, min(100.0, float(progress)))
    return True if _native('dialog_update', args).get('ok') else None


def close_dialog(dialog_id):
    entry = _gate('close_dialog', 'ui')
    if entry is None:
        return None
    return True if _native('dialog_close', {'id': dialog_id}).get('ok') else None


def confirm(title, message, on_yes, on_no=None, yes='Да', no='Отмена', danger=False):
    return show_dialog(title, message, buttons=[
        {'id': 'yes', 'text': yes, 'style': 'red' if danger else 'primary'},
        {'id': 'no', 'text': no, 'style': 'cancel'}],
        on_result=lambda b, v: (on_yes() if b == 'yes' else (on_no() if on_no else None)))


def prompt(title, label, on_ok, default='', multiline=False, ok='Готово'):
    return show_dialog(title, '', fields=[{'type': 'input', 'key': 'v', 'text': label, 'default': default,
                                          'multiline': multiline}],
                       buttons=[{'id': 'ok', 'text': ok}, {'id': 'cancel', 'text': 'Отмена', 'style': 'cancel'}],
                       on_result=lambda b, v: on_ok(v.get('v', '')) if b == 'ok' else None)


def show_bulletin(text, kind='info', subtitle=None, button=None, on_click=None, duration=3.0):
    """Тост с заголовком и кнопкой (undo и т.п.). kind: info|error|success. on_click() по кнопке."""
    entry = _gate('show_bulletin', 'ui')
    if entry is None:
        return None
    if not _limit(entry, 'toast', 10, 10):
        return None
    try:
        sec = min(max(float(duration), 1.0), 15.0)
    except Exception:
        sec = 3.0
    cb = _h['register_cb'](entry, lambda p: on_click(), True) if (button and on_click) else ''
    res = _native('bulletin', {'text': str(text)[:300], 'subtitle': str(subtitle or '')[:120],
                               'button': str(button or '')[:30], 'cb': cb, 'pid': entry['id'],
                               'kind': str(kind), 'duration': sec})
    return bool(res.get('ok'))


# ---- потоки и таймеры ----

_timer_ids = [0]


def run_on_ui_thread(fn, delay=0):
    """Выполнить fn в главном потоке клиента (для работы с интерфейсом), delay в миллисекундах."""
    entry = _gate('run_on_ui_thread', 'background')
    if entry is None or not callable(fn):
        return None
    if not _limit(entry, 'ui_thread', 200, 10):
        return None
    cb = _h['register_cb'](entry, lambda p: fn(), True)
    return True if _native('run_main', {'pid': entry['id'], 'cb': cb, 'delay': int(max(0, delay))}).get('ok') else None


def _start_timer(entry, fn, seconds, repeat):
    timers = entry.setdefault('timers', {})
    if len(timers) >= 20:
        _h['say']('[%s] больше 20 таймеров, отказ' % entry['id'])
        return None
    _timer_ids[0] += 1
    tid = _timer_ids[0]

    def fire():
        if tid not in timers:
            return
        if repeat:
            arm()
        else:
            timers.pop(tid, None)
        try:
            fn()
        except BaseException as e:
            _h['hook_error'](entry, 'timer', e)

    def arm():
        t = threading.Timer(seconds, fire)
        t.daemon = True
        timers[tid] = t
        t.start()

    arm()
    return tid


def set_timeout(fn, seconds):
    """Один раз через seconds секунд (в фоновом потоке). Возвращает id для cancel_timer."""
    entry = _gate('set_timeout', 'background')
    if entry is None or not callable(fn):
        return None
    return _start_timer(entry, fn, max(0.05, float(seconds)), False)


def set_interval(fn, seconds):
    """Повторять каждые seconds секунд (минимум 0.5), пока плагин не выгружен или не отменён."""
    entry = _gate('set_interval', 'background')
    if entry is None or not callable(fn):
        return None
    return _start_timer(entry, fn, max(0.5, float(seconds)), True)


def cancel_timer(timer_id):
    entry = _gate('cancel_timer', 'background')
    if entry is None:
        return None
    t = entry.get('timers', {}).pop(timer_id, None)
    if t is not None:
        t.cancel()
    return True


def run_on_queue(fn, delay_ms=0):
    """Совместимость с exteraGram: выполнить fn в фоне после delay_ms."""
    return set_timeout(fn, max(0, delay_ms) / 1000.0)


# ---- сообщения ----

def send_message(peer, text, reply_to=None, silent=False):
    """Отправка от имени пользователя. peer это id чата, как в send_message_hook (params['peer'])."""
    target = str(peer)
    entry = _gate('send_message', 'messages.send', target)
    if entry is None:
        return None
    if not target.isdigit() or not isinstance(text, str) or not text.strip() or len(text) > 4096:
        _h['say']('[%s] send_message: неверные параметры' % entry['id'])
        _h['audit'](entry, 'send_message', target, 'invalid')
        return None
    # Лимиты 3.3: 20 в минуту и не больше 5 разных получателей за 10 минут.
    if not _limit(entry, 'send', 20, 60):
        _h['say']('[%s] send_message: больше 20 сообщений в минуту, отказ' % entry['id'])
        _h['audit'](entry, 'send_message', target, 'rate_limit')
        return None
    peers = entry.setdefault('peers', {})
    now = time.monotonic()
    for p in [p for p, t in peers.items() if now - t > 600]:
        del peers[p]
    if target not in peers and len(peers) >= 5:
        _h['say']('[%s] send_message: больше 5 получателей за 10 минут, отказ' % entry['id'])
        _h['audit'](entry, 'send_message', target, 'rate_limit')
        return None
    peers[target] = now
    res = _native('send_message', {
        'peer': target, 'text': text,
        'reply_to': int(reply_to) if reply_to else 0, 'silent': bool(silent)})
    if res.get('ok'):
        _h['audit'](entry, 'send_message', target, 'allowed', len(text))
        return True
    _h['say']('[%s] send_message: %s' % (entry['id'], res.get('error', 'отказ')))
    _h['audit'](entry, 'send_message', target, 'denied')
    return None


# ---- чтение и действия в чатах ----

def _ids(peer, msg_id=None):
    peer, msg = str(peer), str(msg_id) if msg_id is not None else '0'
    ok = peer.lstrip('-').isdigit() and msg.isdigit()
    return peer, msg, ok


def get_message(peer, msg_id):
    """Сообщение из уже загруженных: dict(text, out, date, from, has_media, can_edit) или None."""
    peer, msg, ok = _ids(peer, msg_id)
    entry = _gate('get_message', 'messages.read', peer)
    if entry is None or not ok:
        return None
    if not _limit(entry, 'read', 120, 60):
        return None
    res = _native('get_message', {'peer': peer, 'id': msg})
    _h['audit'](entry, 'get_message', peer, 'allowed' if res.get('ok') else 'failed')
    return {k: v for k, v in res.items() if k != 'ok'} if res.get('ok') else None


def edit_message(peer, msg_id, text):
    """Правка своего сообщения."""
    peer, msg, ok = _ids(peer, msg_id)
    entry = _gate('edit_message', 'messages.edit', peer)
    if entry is None or not ok or not isinstance(text, str) or not text.strip() or len(text) > 4096:
        return None
    if not _limit(entry, 'edit', 20, 60):
        _h['say']('[%s] edit_message: больше 20 правок в минуту, отказ' % entry['id'])
        return None
    res = _native('edit_message', {'peer': peer, 'id': msg, 'text': text})
    _h['audit'](entry, 'edit_message', peer, 'allowed' if res.get('ok') else 'failed', len(text))
    return True if res.get('ok') else None


def chat_info(peer):
    """dict(title, username, kind: user|bot|group|channel, members) для загруженного чата или None."""
    peer, _, ok = _ids(peer)
    entry = _gate('chat_info', 'messages.read', peer)
    if entry is None or not ok or not _limit(entry, 'read', 120, 60):
        return None
    res = _native('chat_info', {'peer': peer})
    return {k: v for k, v in res.items() if k != 'ok'} if res.get('ok') else None


def open_chat(peer):
    peer, _, ok = _ids(peer)
    entry = _gate('open_chat', 'ui', peer)
    if entry is None or not ok or not _limit(entry, 'open', 20, 60):
        return None
    return True if _native('open_chat', {'peer': peer}).get('ok') else None


def open_url(url):
    """Открывает http(s)-ссылку так же, как клик по ссылке в чате (с подтверждением клиента)."""
    entry = _gate('open_url', 'ui', str(url)[:200])
    if entry is None or not isinstance(url, str) or not _limit(entry, 'open', 20, 60):
        return None
    return True if _native('open_url', {'url': url}).get('ok') else None


def copy_text(text):
    entry = _gate('copy_text', 'ui')
    if entry is None or not isinstance(text, str) or not _limit(entry, 'copy', 20, 60):
        return None
    return True if _native('copy_text', {'text': text[:10000]}).get('ok') else None


# ---- хранилище плагина: plugin_data/<id>/store.json, квота 1 МБ, ключ до 128 знаков ----

_QUOTA = 1024 * 1024
_stores = {}


def _store_path(entry):
    return os.path.join(_h['data_dir'](entry['id']), 'store.json')


def _store_load(entry):
    data = _stores.get(entry['id'])
    if data is None:
        try:
            with open(_store_path(entry), encoding='utf8') as f:
                data = json.load(f)
            if not isinstance(data, dict):
                data = {}
        except Exception:
            data = {}
        _stores[entry['id']] = data
    return data


def _store_save(entry, data):
    text = json.dumps(data, ensure_ascii=False)
    if len(text.encode('utf8')) > _QUOTA:
        return False
    path = _store_path(entry)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf8') as f:
        f.write(text)
    os.replace(tmp, path)
    return True


class _Storage:
    def get(self, key, default=None):
        entry = _gate('storage.get', 'storage')
        if entry is None:
            return default
        return _store_load(entry).get(str(key), default)

    def set(self, key, value):
        entry = _gate('storage.set', 'storage')
        key = str(key)
        if entry is None or not key or len(key) > 128:
            return False
        data = _store_load(entry)
        old = data.get(key, None)
        had = key in data
        data[key] = value
        try:
            ok = _store_save(entry, data)
        except Exception as e:
            _h['say']('[%s] storage.set: %s' % (entry['id'], e))
            ok = False
        if not ok:
            if had:
                data[key] = old
            else:
                data.pop(key, None)
            _h['say']('[%s] storage.set: не сохранено (квота 1 МБ или значение не JSON)' % entry['id'])
        return ok

    def delete(self, key):
        entry = _gate('storage.delete', 'storage')
        if entry is None:
            return False
        data = _store_load(entry)
        if data.pop(str(key), None) is None:
            return False
        try:
            _store_save(entry, data)
        except Exception as e:
            _h['say']('[%s] storage.delete: %s' % (entry['id'], e))
        return True

    def keys(self):
        entry = _gate('storage.keys', 'storage')
        if entry is None:
            return []
        return sorted(_store_load(entry).keys())


storage = _Storage()


# ---- настройки плагина (реестр настроек клиента, ключи plugin.<id>.<key>) ----

def get_setting(key, default=None):
    entry = _gate('get_setting', 'settings')
    if entry is None or str(key) not in entry.get('settings', {}):
        return default
    res = _native('setting_get', {'key': 'plugin.%s.%s' % (entry['id'], key)})
    return res['v'] if res.get('ok') else default


def set_setting(key, value):
    entry = _gate('set_setting', 'settings')
    if entry is None or str(key) not in entry.get('settings', {}):
        return False
    res = _native('setting_set', {'key': 'plugin.%s.%s' % (entry['id'], key), 'value': value})
    return bool(res.get('ok'))


# ---- журнал ----

def log(text):
    entry = _h['caller']()
    _h['say']('[%s] %s' % (entry['id'] if entry else '?', text))
