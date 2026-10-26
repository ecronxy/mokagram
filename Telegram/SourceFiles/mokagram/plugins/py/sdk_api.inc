constexpr auto kApiScript = R"PY(
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
    if not _limit(entry, '__all', 300, 60):
        _h['say']('[%s] больше 300 действий в минуту, %s отклонён' % (entry['id'], op))
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
                'EditText': 'input', 'Password': 'password', 'Divider': 'divider', 'Progress': 'progress'}


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


def set_interval(fn, seconds, persist=None):
    """Повторять каждые seconds секунд (минимум 0.5), пока плагин не выгружен или не отменён.
    persist='имя': расписание помнится на диске, пропущенный при выключенном клиенте запуск выполняется при старте
    (минимум 5 секунд, шаг проверки 15 секунд)."""
    entry = _gate('set_interval', 'background')
    if entry is None or not callable(fn):
        return None
    if persist:
        try:
            _h['fw']['job'](entry, 'interval', max(5, int(seconds)), fn, persist)
        except Exception as e:
            _h['say']('[%s] set_interval: %s' % (entry['id'], e))
            return None
        return True
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

def _clean_entities(entities, text):
    out = []
    for e in (entities or [])[:200]:
        if not isinstance(e, dict):
            continue
        item = {k: e[k] for k in ('type', 'offset', 'length', 'url', 'language') if k in e}
        if 'document_id' in e:
            item['document_id'] = str(int(e['document_id']))
        out.append(item)
    return out


def send_message(peer, text, reply_to=None, silent=False, entities=None):
    """Отправка от имени пользователя. peer это id чата, как в send_message_hook (params['peer']).
    entities: [{'type': 'bold|italic|underline|strike|code|pre|spoiler|quote|link|custom_emoji', 'offset': 0,
    'length': 3, 'url': ..., 'document_id': ...}], смещения в UTF-16 как в Telegram. custom_emoji это премиум-эмодзи."""
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
        'reply_to': int(reply_to) if reply_to else 0, 'silent': bool(silent),
        'entities': _clean_entities(entities, text)})
    if res.get('ok'):
        _h['audit'](entry, 'send_message', target, 'allowed', len(text))
        return True
    _h['say']('[%s] send_message: %s' % (entry['id'], res.get('error', 'отказ')))
    _h['audit'](entry, 'send_message', target, 'denied')
    return None


def send_file(peer, path, caption='', as_photo=False, silent=False):
    """Отправка файла или фото от имени пользователя (право messages.send, те же лимиты, что у send_message)."""
    target = str(peer)
    entry = _gate('send_file', 'messages.send', target)
    if entry is None:
        return None
    path = os.path.abspath(str(path))
    if not target.isdigit() or not os.path.isfile(path) or len(str(caption)) > 1024:
        _h['say']('[%s] send_file: неверные параметры' % entry['id'])
        return None
    if not _limit(entry, 'send_file', 5, 60):
        _h['say']('[%s] send_file: больше 5 файлов в минуту, отказ' % entry['id'])
        _h['audit'](entry, 'send_file', target, 'rate_limit')
        return None
    peers = entry.setdefault('peers', {})
    now = time.monotonic()
    if target not in peers and len(peers) >= 5:
        _h['audit'](entry, 'send_file', target, 'rate_limit')
        return None
    peers[target] = now
    res = _native('send_file', {'peer': target, 'path': path, 'caption': str(caption),
                                'kind': 'photo' if as_photo else 'file', 'silent': bool(silent)})
    _h['audit'](entry, 'send_file', target, 'allowed' if res.get('ok') else 'failed', os.path.getsize(path))
    if not res.get('ok'):
        _h['say']('[%s] send_file: %s' % (entry['id'], res.get('error', 'отказ')))
    return True if res.get('ok') else None


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


def edit_message(peer, msg_id, text, entities=None):
    """Правка своего сообщения (entities как у send_message)."""
    peer, msg, ok = _ids(peer, msg_id)
    entry = _gate('edit_message', 'messages.edit', peer)
    if entry is None or not ok or not isinstance(text, str) or not text.strip() or len(text) > 4096:
        return None
    if not _limit(entry, 'edit', 20, 60):
        _h['say']('[%s] edit_message: больше 20 правок в минуту, отказ' % entry['id'])
        return None
    res = _native('edit_message', {'peer': peer, 'id': msg, 'text': text, 'entities': _clean_entities(entities, text)})
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


def open_chat(peer, msg_id=None):
    """Открыть чат; с msg_id сразу прокрутить к этому сообщению. Нужно право ui."""
    peer, msg, ok = _ids(peer, msg_id)
    entry = _gate('open_chat', 'ui', peer)
    if entry is None or not ok or not _limit(entry, 'open', 20, 60):
        return None
    return True if _native('open_chat', {'peer': peer, 'msg_id': msg}).get('ok') else None


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


def get_dialogs(limit=50):
    """Список чатов из загруженных: [{peer, title, username, kind, unread}]."""
    entry = _gate('get_dialogs', 'messages.read')
    if entry is None or not _limit(entry, 'read', 120, 60):
        return None
    res = _native('get_dialogs', {'limit': int(limit)})
    return res.get('dialogs') if res.get('ok') else None


def get_history(peer, limit=30):
    """Последние загруженные сообщения чата (старые первыми): [{id, peer, from, text, out, date, has_media}]."""
    peer, _, ok = _ids(peer)
    entry = _gate('get_history', 'messages.read', peer)
    if entry is None or not ok or not _limit(entry, 'read', 120, 60):
        return None
    res = _native('get_history', {'peer': peer, 'limit': int(limit)})
    return res.get('messages') if res.get('ok') else None


def current_chat():
    """Открытый сейчас чат: {peer, title, username, kind} или None."""
    entry = _gate('current_chat', 'messages.read')
    if entry is None or not _limit(entry, 'read', 120, 60):
        return None
    res = _native('current_chat', {})
    return {k: v for k, v in res.items() if k != 'ok'} if res.get('ok') else None


def mark_read(peer):
    peer, _, ok = _ids(peer)
    entry = _gate('mark_read', 'messages.read', peer)
    if entry is None or not ok:
        return None
    return True if _native('mark_read', {'peer': peer}).get('ok') else None


def get_user_info(peer):
    """Сведения о чате или человеке из загруженных: {peer, title, username, kind, about, premium, verified, online, ...}."""
    peer, _, ok = _ids(peer)
    entry = _gate('get_user_info', 'messages.read', peer)
    if entry is None or not ok or not _limit(entry, 'read', 120, 60):
        return None
    res = _native('get_user_info', {'peer': peer})
    return {k: v for k, v in res.items() if k != 'ok'} if res.get('ok') else None


def get_me():
    """Ваш аккаунт: {peer, title, username, premium}."""
    entry = _gate('get_me', 'messages.read')
    if entry is None:
        return None
    res = _native('get_me', {})
    return {k: v for k, v in res.items() if k != 'ok'} if res.get('ok') else None


def clipboard_get():
    """Текст из буфера обмена или None. Нужно право clipboard."""
    entry = _gate('clipboard_get', 'clipboard')
    if entry is None or not _limit(entry, 'clipboard', 30, 60):
        return None
    res = _native('clipboard_get', {})
    _h['audit'](entry, 'clipboard_get', '', 'allowed' if res.get('ok') else 'failed')
    return res.get('text') if res.get('ok') else None


def pin_message(peer, msg_id, pin=True):
    """Закрепить или открепить сообщение. Нужно право messages.edit."""
    peer, msg, ok = _ids(peer, msg_id)
    entry = _gate('pin_message', 'messages.edit', peer)
    if entry is None or not ok or not _limit(entry, 'pin', 10, 60):
        return None
    res = _native('pin_message', {'peer': peer, 'id': msg, 'pin': bool(pin)})
    _h['audit'](entry, 'pin_message', peer, 'allowed' if res.get('ok') else 'failed')
    return True if res.get('ok') else None


def delete_messages(peer, msg_ids, revoke=False):
    """Удалить сообщения (до 100 за раз). revoke=True удаляет и у других. Нужно право messages.delete."""
    peer, _, ok = _ids(peer)
    entry = _gate('delete_messages', 'messages.delete', peer)
    if entry is None or not ok or not isinstance(msg_ids, (list, tuple)) or not 0 < len(msg_ids) <= 100:
        return None
    if not _limit(entry, 'delete', 5, 60):
        _h['say']('[%s] delete_messages: больше 5 удалений в минуту, отказ' % entry['id'])
        return None
    ids = [str(int(i)) for i in msg_ids]
    res = _native('delete_messages', {'peer': peer, 'ids': ids, 'revoke': bool(revoke)})
    _h['audit'](entry, 'delete_messages', peer, 'allowed' if res.get('ok') else 'failed', len(ids))
    return True if res.get('ok') else None


# ---- универсальный вызов TL и обёртки над ним ----
# Ответы приходят в колбэк callback(result, error) в рабочем потоке плагина. result это dict/список
# с ключом '_' (имя конструктора), error это {'code', 'type', 'description'}.

_TL_DENY = {
    'channels.deleteChannel', 'channels.editCreator', 'messages.deleteChat', 'users.setSecureValueErrors',
    'contacts.resetSaved', 'messages.deletePhoneCallHistory',
}
_TL_DENY_HEAD = {'auth', 'bots', 'smsjobs', 'langpack', 'phone'}
_TL_SENSITIVE = ('Password', 'Secure', 'Authorization', 'Takeout', 'Passkey', 'Phone', 'Email', 'Passport')


def _tl_perm(method):
    """Какое право нужно методу или None, если метод закрыт совсем (захват и потеря аккаунта, платежи)."""
    if not isinstance(method, str) or not method or method in _TL_DENY:
        return None
    if method.startswith(('invoke', 'initConnection')):
        return None
    head, _, tail = method.rpartition('.')
    readonly = tail.startswith(('get', 'search', 'resolve', 'check'))
    if head in _TL_DENY_HEAD:
        return None
    if head in ('account', 'payments'):
        if not readonly or any(w in tail for w in _TL_SENSITIVE):
            return None
    return 'tl.read' if readonly else 'tl.write'


def _flood_delay(error_type):
    import re as _re
    m = _re.match(r'(?:FLOOD_WAIT|SLOWMODE_WAIT|FLOOD_PREMIUM_WAIT)_(\d+)$', error_type or '')
    return int(m.group(1)) if m else 0


def _invoke(entry, method, params, callback):
    tl = _h.get('tl')
    if tl is None or not _limit(entry, 'tl', 60, 60):
        _h['say']('[%s] invoke_tl %s: не готово или больше 60 вызовов в минуту' % (entry['id'], method))
        return None
    try:
        raw = tl.encode_request(method, params or {})
    except Exception as e:
        _h['say']('[%s] invoke_tl %s: %s' % (entry['id'], method, e))
        return None
    state = {'attempt': 0}

    def send():
        token = _h['register_cb'](entry, done, True)
        return _native('invoke_raw', {'pid': entry['id'], 'token': token, 'hex': raw.hex()})

    def done(payload):
        result = error = None
        if payload.get('ok'):
            try:
                result = tl.decode_result(method, bytes.fromhex(payload['hex']))
            except Exception as e:
                error = {'code': 0, 'type': 'DECODE', 'description': str(e)}
        else:
            error = {'code': payload.get('code', 0), 'type': payload.get('type', ''),
                     'description': payload.get('description', '')}
            wait = _flood_delay(error['type'])
            if wait and wait <= 60 and state['attempt'] < 3:
                state['attempt'] += 1
                entry['flood_until'] = time.monotonic() + wait + 1
                _h['say']('[%s] %s: %s, повтор через %d с' % (entry['id'], method, error['type'], wait + 1))
                t = threading.Timer(wait + 1, send)
                t.daemon = True
                t.start()
                return
        if callable(callback):
            callback(result, error)

    delay = entry.get('flood_until', 0) - time.monotonic()
    if delay > 0:
        t = threading.Timer(min(delay, 60), send)
        t.daemon = True
        t.start()
        res = {'ok': True}
    else:
        res = send()
    _h['audit'](entry, 'invoke_tl', method, 'allowed' if res.get('ok') else 'failed', len(raw))
    return True if res.get('ok') else None


def invoke_tl(method, params=None, callback=None):
    """Любой метод Telegram API: invoke_tl('messages.getHistory', {...}, cb). Нужно право tl.read или tl.write
    (смотря на метод). Методы входа, платежей, паролей и сеансов закрыты."""
    perm = _tl_perm(method)
    if perm is None:
        entry = _h['caller']()
        if entry is not None:
            _h['audit'](entry, 'invoke_tl', str(method), 'denied')
            _h['say']('[%s] invoke_tl %s закрыт' % (entry['id'], method))
        return None
    entry = _gate('invoke_tl', perm, method)
    if entry is None:
        return None
    return _invoke(entry, method, params, callback)


def _tl_input(entry, kind, peer):
    res = _native('tl_input', {'kind': kind, 'peer': peer})
    tl = _h.get('tl')
    if not res.get('ok') or tl is None:
        return None, res
    return tl.decode_object(bytes.fromhex(res['hex'])), res


def input_peer(peer):
    """InputPeer чата (dict TL) для своих запросов через invoke_tl."""
    peer, _, ok = _ids(peer)
    entry = _gate('input_peer', 'messages.read', peer)
    if entry is None or not ok:
        return None
    return _tl_input(entry, 'peer', peer)[0]


def _user_brief(u):
    if not isinstance(u, dict):
        return None
    return {'id': u.get('id'), 'first_name': u.get('first_name', ''), 'last_name': u.get('last_name', ''),
            'username': u.get('username', ''), 'bot': bool(u.get('bot')), 'premium': bool(u.get('premium')),
            'deleted': bool(u.get('deleted'))}


def _peer_id(p):
    if not isinstance(p, dict):
        return None
    return p.get('user_id') or p.get('chat_id') or p.get('channel_id')


def _msg_brief(m):
    if not isinstance(m, dict) or m.get('_') == 'messageEmpty':
        return None
    return {'id': m.get('id'), 'text': m.get('message', ''), 'date': m.get('date'), 'out': bool(m.get('out')),
            'from': _peer_id(m.get('from_id')), 'has_media': m.get('media') is not None,
            'reply_to': (m.get('reply_to') or {}).get('reply_to_msg_id')}


def _random_id():
    import random
    return random.getrandbits(62)


def forward_messages(from_peer, msg_ids, to_peer, callback=None, drop_author=False, silent=False):
    """Переслать сообщения (до 100). Нужно право messages.send."""
    from_peer, _, ok1 = _ids(from_peer)
    to_peer, _, ok2 = _ids(to_peer)
    entry = _gate('forward_messages', 'messages.send', to_peer)
    if entry is None or not (ok1 and ok2) or not isinstance(msg_ids, (list, tuple)) or not 0 < len(msg_ids) <= 100:
        return None
    if not _limit(entry, 'forward', 10, 60):
        _h['say']('[%s] forward_messages: больше 10 пересылок в минуту, отказ' % entry['id'])
        return None
    src = _tl_input(entry, 'peer', from_peer)[0]
    dst = _tl_input(entry, 'peer', to_peer)[0]
    if src is None or dst is None:
        return None
    ids = [int(i) for i in msg_ids]
    return _invoke(entry, 'messages.forwardMessages', {
        'from_peer': src, 'id': ids, 'random_id': [_random_id() for _ in ids], 'to_peer': dst,
        'drop_author': True if drop_author else None, 'silent': True if silent else None,
    }, callback)


def react(peer, msg_id, emoji, callback=None):
    """Поставить реакцию: emoji строкой, а для премиум-эмодзи число document_id. Нужно право messages.send."""
    peer, msg, ok = _ids(peer, msg_id)
    entry = _gate('react', 'messages.send', peer)
    if entry is None or not ok:
        return None
    if not _limit(entry, 'react', 30, 60):
        _h['say']('[%s] react: больше 30 реакций в минуту, отказ' % entry['id'])
        return None
    ip = _tl_input(entry, 'peer', peer)[0]
    if ip is None:
        return None
    reaction = ({'_': 'reactionCustomEmoji', 'document_id': int(emoji)} if isinstance(emoji, int)
                else {'_': 'reactionEmoji', 'emoticon': str(emoji)})
    return _invoke(entry, 'messages.sendReaction', {
        'peer': ip, 'msg_id': int(msg), 'reaction': [reaction],
    }, callback)


def get_participants(peer, callback, limit=200, offset=0):
    """Участники группы или канала: callback([{user: {...}, role}], error). Нужно право messages.read."""
    peer, _, ok = _ids(peer)
    entry = _gate('get_participants', 'messages.read', peer)
    if entry is None or not ok or not callable(callback) or not _limit(entry, 'read', 120, 60):
        return None
    kind = _native('tl_input', {'kind': 'peer', 'peer': peer})
    if not kind.get('ok'):
        return None

    def simplify(result, error):
        if error or result is None:
            return callback(None, error)
        users = {u.get('id'): _user_brief(u) for u in result.get('users', [])}
        out = []
        if result.get('_') == 'channels.channelParticipants':
            for p in result.get('participants', []):
                uid = _peer_id(p.get('peer')) or p.get('user_id')
                out.append({'user': users.get(uid) or {'id': uid},
                            'role': p['_'].replace('channelParticipant', '').lower() or 'member'})
        elif result.get('_') == 'messages.chatFull':
            full = (result.get('full_chat') or {}).get('participants') or {}
            for p in full.get('participants', []) or []:
                uid = p.get('user_id')
                out.append({'user': users.get(uid) or {'id': uid},
                            'role': p['_'].replace('chatParticipant', '').lower() or 'member'})
        callback(out, None)

    if kind.get('kind') == 'channel':
        ip = _tl_input(entry, 'channel', peer)[0]
        if ip is None:
            return None
        return _invoke(entry, 'channels.getParticipants', {
            'channel': ip, 'filter': {'_': 'channelParticipantsRecent'},
            'offset': int(offset), 'limit': min(int(limit), 200), 'hash': 0,
        }, simplify)
    if kind.get('kind') == 'chat':
        return _invoke(entry, 'messages.getFullChat', {'chat_id': int(kind['bare'])}, simplify)
    return None


def _messages_call(entry, method, params, callback):
    def simplify(result, error):
        if error or result is None:
            return callback(None, error)
        callback([m for m in (_msg_brief(x) for x in result.get('messages', [])) if m], None)
    return _invoke(entry, method, params, simplify)


def search_messages(peer, query, callback, limit=20, offset_id=0):
    """Поиск по чату на сервере: callback([{id, text, date, out, from}], error). Нужно право messages.read."""
    peer, _, ok = _ids(peer)
    entry = _gate('search_messages', 'messages.read', peer)
    if entry is None or not ok or not callable(callback) or not isinstance(query, str) or len(query) > 256:
        return None
    if not _limit(entry, 'read', 120, 60):
        return None
    ip = _tl_input(entry, 'peer', peer)[0]
    if ip is None:
        return None
    return _messages_call(entry, 'messages.search', {
        'peer': ip, 'q': query, 'filter': {'_': 'inputMessagesFilterEmpty'}, 'min_date': 0, 'max_date': 0,
        'offset_id': int(offset_id), 'add_offset': 0, 'limit': min(int(limit), 100), 'max_id': 0, 'min_id': 0,
        'hash': 0,
    }, callback)


def get_history_remote(peer, callback, limit=30, offset_id=0):
    """История чата с сервера, не только загруженное: callback([{id, text, date, out, from}], error)."""
    peer, _, ok = _ids(peer)
    entry = _gate('get_history_remote', 'messages.read', peer)
    if entry is None or not ok or not callable(callback) or not _limit(entry, 'read', 120, 60):
        return None
    ip = _tl_input(entry, 'peer', peer)[0]
    if ip is None:
        return None
    return _messages_call(entry, 'messages.getHistory', {
        'peer': ip, 'offset_id': int(offset_id), 'offset_date': 0, 'add_offset': 0,
        'limit': min(int(limit), 100), 'max_id': 0, 'min_id': 0, 'hash': 0,
    }, callback)


# ---- файлы и медиа ----

def _path_ok(entry, path):
    """Путь можно трогать плагину: своя папка данных, временные файлы или выданное право files."""
    import tempfile
    real = os.path.realpath(path)
    roots = [os.path.realpath(_h['data_dir'](entry['id'])), os.path.realpath(tempfile.gettempdir())]
    if any(real == r or real.startswith(r + os.sep) for r in roots):
        return True
    return bool(_h['granted'](entry['id'], 'files'))


def send_album(peer, paths, caption='', as_photo=True, silent=False):
    """Альбом из 2-10 файлов (фото или документы). Право messages.send; пути как у download_media."""
    target = str(peer)
    entry = _gate('send_album', 'messages.send', target)
    if entry is None:
        return None
    if (not target.isdigit() or not isinstance(paths, (list, tuple)) or not 2 <= len(paths) <= 10
            or len(str(caption)) > 1024):
        _h['say']('[%s] send_album: неверные параметры' % entry['id'])
        return None
    paths = [os.path.abspath(str(p)) for p in paths]
    if not all(os.path.isfile(p) and _path_ok(entry, p) for p in paths):
        _h['say']('[%s] send_album: файла нет или путь вне папки плагина (нужно право files)' % entry['id'])
        _h['audit'](entry, 'send_album', target, 'denied')
        return None
    if not _limit(entry, 'send_file', 5, 60):
        _h['audit'](entry, 'send_album', target, 'rate_limit')
        return None
    res = _native('send_album', {'peer': target, 'paths': paths, 'caption': str(caption),
                                 'kind': 'photo' if as_photo else 'file', 'silent': bool(silent)})
    _h['audit'](entry, 'send_album', target, 'allowed' if res.get('ok') else 'failed', len(paths))
    if not res.get('ok'):
        _h['say']('[%s] send_album: %s' % (entry['id'], res.get('error', 'отказ')))
    return True if res.get('ok') else None


def download_media(peer, msg_id, path, callback=None):
    """Сохранить фото или файл из сообщения: callback({'ok': True, 'path': ...} или {'ok': False, 'error': ...}).
    Путь внутри папки плагина или во временной, иначе нужно право files. Право messages.read."""
    peer, msg, ok = _ids(peer, msg_id)
    entry = _gate('download_media', 'messages.read', peer)
    if entry is None or not ok or not isinstance(path, str) or not path:
        return None
    path = os.path.abspath(path)
    if not _path_ok(entry, path):
        _h['say']('[%s] download_media: путь вне папки плагина (нужно право files)' % entry['id'])
        _h['audit'](entry, 'download_media', peer, 'denied')
        return None
    if not _limit(entry, 'download', 10, 60):
        _h['say']('[%s] download_media: больше 10 загрузок в минуту, отказ' % entry['id'])
        return None
    token = _h['register_cb'](entry, lambda p: callback(p) if callable(callback) else None, True)
    res = _native('download_media', {'pid': entry['id'], 'token': token, 'peer': peer, 'id': msg, 'path': path})
    _h['audit'](entry, 'download_media', peer, 'allowed' if res.get('ok') else 'failed')
    if not res.get('ok') and callable(callback):
        callback({'ok': False, 'error': res.get('error', 'отказ')})
    return True if res.get('ok') else None


def press_button(peer, msg_id, button, callback=None):
    """Нажать инлайн-кнопку бота из message['buttons']: callback(ответ, ошибка). Право tl.write (это действие от вашего имени)."""
    peer, msg, ok = _ids(peer, msg_id)
    entry = _gate('press_button', 'tl.write', peer)
    if entry is None or not ok or not isinstance(button, dict):
        return None
    if button.get('kind') == 'url' and button.get('url'):
        return open_url(button['url'])
    if button.get('kind') != 'callback':
        _h['say']('[%s] press_button: этот вид кнопки не поддерживается' % entry['id'])
        return None
    import base64
    ip = _tl_input(entry, 'peer', peer)[0]
    if ip is None:
        return None
    return _invoke(entry, 'messages.getBotCallbackAnswer', {
        'peer': ip, 'msg_id': int(msg), 'data': base64.b64decode(button.get('data', ''))}, callback)


def get_user(user_id):
    """Сведения о человеке из загруженных (то же, что get_user_info)."""
    return get_user_info(user_id)


def delete_message(peer, msg_id, revoke=False):
    """Одно сообщение (см. delete_messages)."""
    return delete_messages(peer, [msg_id], revoke)


# =====================================================================================
# Платформа: cron, общая база, секреты, http, зависимости
# =====================================================================================

def cron(expr, fn, name=None):
    """Запуск по расписанию cron из 5 полей: api.cron('0 9 * * *', fn). Запуски, пропущенные при выключенном клиенте
    (до двух недель), выполняются один раз при старте. Права не нужны."""
    entry = _gate('cron', 'background')
    if entry is None or not callable(fn):
        return None
    try:
        _h['fw']['job'](entry, 'cron', expr, fn, name)
    except Exception as e:
        _h['say']('[%s] cron: %s' % (entry['id'], e))
        return None
    return True


class _Db:
    """Общая база всех плагинов (ключи вида 'тема.имя', значения JSON, всего до 4 МБ). Право storage."""

    def _entry(self, op):
        return _gate('db.' + op, 'storage')

    def get(self, key, default=None):
        if self._entry('get') is None:
            return default
        return _h['fw']['db_load']().get(str(key), default)

    def set(self, key, value):
        entry = self._entry('set')
        key = str(key)
        if entry is None or not key or len(key) > 128:
            return False
        try:
            json.dumps(value)
        except TypeError:
            return False
        return _h['fw']['db_set'](entry['id'], key, value)

    def delete(self, key):
        entry = self._entry('delete')
        if entry is None:
            return False
        return _h['fw']['db_set'](entry['id'], str(key), None, True)

    def keys(self, prefix=''):
        if self._entry('keys') is None:
            return []
        return [k for k in _h['fw']['db_load']() if k.startswith(prefix)]

    def on_change(self, prefix, fn):
        """fn(key, value, old, plugin_id) при изменении любого ключа с этим началом (value None при удалении)."""
        entry = self._entry('on_change')
        if entry is None or not callable(fn):
            return False
        _h['fw']['db_subs'].append((entry['id'], str(prefix), fn))
        return True


db = _Db()


class _Secrets:
    """Секреты плагина: хранилище ОС (secret-tool), иначе зашифрованный файл. Право storage."""

    def get(self, name):
        entry = _gate('secrets.get', 'storage')
        return _h['fw']['secret_get'](entry['id'], str(name)) if entry is not None else None

    def set(self, name, value):
        entry = _gate('secrets.set', 'storage')
        return bool(entry is not None and _h['fw']['secret_set'](entry['id'], str(name), None if value is None else str(value)))

    def delete(self, name):
        return self.set(name, None)


secrets = _Secrets()


def get_plugin(plugin_id):
    """Экземпляр другого загруженного плагина (для __dependencies__) или None."""
    entry = _h['caller']()
    if entry is None:
        return None
    other = _h['fw']['plugins'].get(str(plugin_id))
    return other.get('plugin') if other and other.get('state') == 'loaded' else None


class HttpResponse:
    def __init__(self, status, headers, content):
        self.status = status
        self.headers = headers
        self.content = content

    @property
    def ok(self):
        return 200 <= self.status < 300

    @property
    def text(self):
        return self.content.decode('utf8', 'replace')

    def json(self):
        return json.loads(self.content.decode('utf8'))


class _HttpFuture:
    def __init__(self, fut):
        self._fut = fut

    def result(self, timeout=None):
        return self._fut.result(timeout)

    def __await__(self):
        import asyncio
        return asyncio.wrap_future(self._fut).__await__()


class _Http:
    """api.http.get/post/request/download. Право network. Только http(s), внутренние адреса закрыты,
    ответ до 20 МБ, до 30 запросов в минуту. С callback(response, error) ответ придёт в колбэк,
    без него вернётся объект, который можно await в async-команде или взять .result()."""

    def request(self, method, url, headers=None, params=None, data=None, json_body=None, files=None, timeout=15, callback=None):
        entry = _gate('http', 'network', str(url)[:120])
        if entry is None or not _limit(entry, 'http', 30, 60):
            return None
        url = str(url)
        if params:
            import urllib.parse
            url += ('&' if '?' in url else '?') + urllib.parse.urlencode(params)
        hdrs = dict(headers or {})
        body = None
        if json_body is not None:
            body = json.dumps(json_body).encode('utf8')
            hdrs.setdefault('Content-Type', 'application/json')
        elif files:
            import uuid
            boundary = uuid.uuid4().hex
            parts = []
            for k, v in (data or {}).items():
                parts.append(('--%s\r\nContent-Disposition: form-data; name="%s"\r\n\r\n%s\r\n' % (boundary, k, v)).encode('utf8'))
            for k, v in files.items():
                if isinstance(v, str):
                    path = os.path.abspath(v)
                    if not os.path.isfile(path) or not _path_ok(entry, path):
                        _h['say']('[%s] http: файл вне папки плагина (нужно право files)' % entry['id'])
                        return None
                    fname, content = os.path.basename(path), open(path, 'rb').read()
                else:
                    fname, content = k, bytes(v)
                parts.append(('--%s\r\nContent-Disposition: form-data; name="%s"; filename="%s"\r\n\r\n' % (boundary, k, fname)).encode('utf8') + content + b'\r\n')
            parts.append(('--%s--\r\n' % boundary).encode('utf8'))
            body = b''.join(parts)
            hdrs['Content-Type'] = 'multipart/form-data; boundary=' + boundary
        elif data is not None:
            import urllib.parse
            body = data if isinstance(data, bytes) else urllib.parse.urlencode(data).encode('utf8')
            hdrs.setdefault('Content-Type', 'application/x-www-form-urlencoded')
        if body is not None and len(body) > (20 << 20):
            return None

        def work():
            r = _h['fw']['http'](method, url, hdrs, body, timeout, 20 << 20)
            return HttpResponse(r['status'], r['headers'], r['content'])
        _h['audit'](entry, 'http', '%s %s' % (method, url[:150]), 'allowed', len(body or b''))
        fut = _h['fw']['pool'].submit(work)
        if callback is None:
            return _HttpFuture(fut)

        def finish(f):
            try:
                resp, err = f.result(), None
            except BaseException as e:
                resp, err = None, str(e)
            try:
                callback(resp, err)
            except BaseException as e:
                _h['hook_error'](entry, 'http.callback', e)
        fut.add_done_callback(finish)
        return True

    def get(self, url, **kw):
        return self.request('GET', url, **kw)

    def post(self, url, **kw):
        return self.request('POST', url, **kw)

    def download(self, url, path, callback=None, timeout=60):
        """Скачать в файл (до 100 МБ). Путь в папке плагина или во временной, иначе нужно право files."""
        entry = _gate('http', 'network', str(url)[:120])
        path = os.path.abspath(str(path))
        if entry is None or not _limit(entry, 'http', 30, 60) or not _path_ok(entry, path):
            return None

        def work():
            os.makedirs(os.path.dirname(path), exist_ok=True)
            r = _h['fw']['http']('GET', str(url), None, None, timeout, 100 << 20, path)
            return HttpResponse(r['status'], r['headers'], b'')
        _h['audit'](entry, 'http.download', str(url)[:150], 'allowed')
        fut = _h['fw']['pool'].submit(work)
        if callback is None:
            return _HttpFuture(fut)
        fut.add_done_callback(lambda f: callback(None if f.exception() else f.result(), str(f.exception()) if f.exception() else None))
        return True


http = _Http()


# =====================================================================================
# Обёртки над invoke_tl: короткие имена. Каждое действие под своим правом и в журнале.
# chats.manage: join/leave/invite/kick/ban/restrict/edit_admin; chats.settings: mute/archive/folders/set_typing;
# profile.write: профиль и emoji-статус; privacy.write: приватность. Ответ приходит в callback(result, error).
# =====================================================================================

def peer_id_for(kind, bare_id):
    """Строковый id чата в клиенте из вида ('user'|'chat'|'channel') и id из TL."""
    bare_id = int(bare_id)
    return str(bare_id + (1 << 48 if kind == 'chat' else 1 << 49 if kind == 'channel' else 0))


def _entities_to_tl(entities):
    out = []
    names = {'bold': 'messageEntityBold', 'italic': 'messageEntityItalic', 'underline': 'messageEntityUnderline',
             'strike': 'messageEntityStrike', 'code': 'messageEntityCode', 'spoiler': 'messageEntitySpoiler',
             'quote': 'messageEntityBlockquote'}
    for e in (entities or [])[:200]:
        try:
            t, o, l = e['type'], int(e['offset']), int(e['length'])
        except Exception:
            continue
        if t in names:
            out.append({'_': names[t], 'offset': o, 'length': l})
        elif t == 'pre':
            out.append({'_': 'messageEntityPre', 'offset': o, 'length': l, 'language': str(e.get('language', ''))})
        elif t == 'link' and e.get('url'):
            out.append({'_': 'messageEntityTextUrl', 'offset': o, 'length': l, 'url': str(e['url'])})
        elif t == 'custom_emoji' and e.get('document_id'):
            out.append({'_': 'messageEntityCustomEmoji', 'offset': o, 'length': l, 'document_id': int(e['document_id'])})
    return out


def _wrap(perm, op, peer=None, limit=(30, 60)):
    """Общая проверка обёртки: возвращает (entry, peer_str) или (None, None)."""
    entry = _gate(op, perm, str(peer) if peer is not None else '')
    if entry is None or not _limit(entry, op, limit[0], limit[1]):
        return None
    return entry


def get_reply_message(peer, msg_id):
    """Сообщение, на которое ответили (из загруженных) или None."""
    m = get_message(peer, msg_id)
    if not m or not m.get('reply_to') or m['reply_to'] == '0':
        return None
    return get_message(peer, m['reply_to'])



def get_entity(query, callback):
    """Найти человека, канал или группу по @username или ссылке t.me/...: callback({id, peer, kind, title, username}, error).
    Право tl.read."""
    entry = _wrap('tl.read', 'get_entity', query)
    if entry is None or not callable(callback):
        return None
    q = str(query).strip()
    for pre in ('https://t.me/', 'http://t.me/', 't.me/', '@'):
        if q.startswith(pre):
            q = q[len(pre):]
    q = q.split('/')[0].split('?')[0]
    if not q:
        return None

    def done(res, err):
        if err or res is None:
            return callback(None, err)
        found = None
        for u in res.get('users', []):
            found = {'kind': 'user', 'id': u.get('id'), 'peer': peer_id_for('user', u['id']), 'title': ' '.join(x for x in (u.get('first_name'), u.get('last_name')) if x), 'username': u.get('username', ''), 'access_hash': u.get('access_hash')}
        for c in res.get('chats', []):
            kind = 'channel' if c.get('_') in ('channel', 'channelForbidden') else 'chat'
            found = {'kind': kind, 'id': c.get('id'), 'peer': peer_id_for(kind, c['id']), 'title': c.get('title', ''), 'username': c.get('username', ''), 'access_hash': c.get('access_hash')}
        callback(found, None if found else {'type': 'NOT_FOUND', 'description': 'не найдено'})
    return _invoke(entry, 'contacts.resolveUsername', {'username': q}, done)


def schedule_message(peer, text, when, callback=None, entities=None, silent=False):
    """Отложенное сообщение: when это время (unix) в будущем. Право messages.send."""
    peer, _, ok = _ids(peer)
    entry = _wrap('messages.send', 'schedule_message', peer, (10, 60))
    if entry is None or not ok or not isinstance(text, str) or not text.strip() or len(text) > 4096:
        return None
    when = int(when)
    if when < time.time() + 10:
        _h['say']('[%s] schedule_message: время должно быть в будущем' % entry['id'])
        return None
    ip = _tl_input(entry, 'peer', peer)[0]
    if ip is None:
        return None
    ents = _entities_to_tl(entities)
    params = {'peer': ip, 'message': text, 'random_id': _random_id(), 'schedule_date': when,
              'silent': True if silent else None}
    if ents:
        params['entities'] = ents
    return _invoke(entry, 'messages.sendMessage', params, callback)


def save_draft(peer, text, entities=None, callback=None):
    """Черновик в чате (пустой текст убирает). Право messages.send."""
    peer, _, ok = _ids(peer)
    entry = _wrap('messages.send', 'save_draft', peer)
    if entry is None or not ok or not isinstance(text, str) or len(text) > 4096:
        return None
    ip = _tl_input(entry, 'peer', peer)[0]
    if ip is None:
        return None
    params = {'peer': ip, 'message': text}
    ents = _entities_to_tl(entities)
    if ents:
        params['entities'] = ents
    return _invoke(entry, 'messages.saveDraft', params, callback)


def get_drafts(callback):
    """Все черновики: callback([{peer, text, date}], error). Право messages.read."""
    entry = _wrap('messages.read', 'get_drafts', None, (10, 60))
    if entry is None or not callable(callback):
        return None

    def done(res, err):
        if err or res is None:
            return callback(None, err)
        out = []
        for u in res.get('updates', []) if isinstance(res, dict) else []:
            d = u.get('draft') or {}
            if u.get('_') == 'updateDraftMessage' and d.get('_') == 'draftMessage':
                out.append({'peer': _fw_peer(u.get('peer')), 'text': d.get('message', ''), 'date': d.get('date')})
        callback(out, None)
    return _invoke(entry, 'messages.getAllDrafts', {}, done)


def _fw_peer(p):
    if not isinstance(p, dict):
        return None
    t = p.get('_')
    if t == 'peerUser':
        return peer_id_for('user', p['user_id'])
    if t == 'peerChat':
        return peer_id_for('chat', p['chat_id'])
    if t == 'peerChannel':
        return peer_id_for('channel', p['channel_id'])
    return None


def _kind_of(peer):
    res = _native('tl_input', {'kind': 'peer', 'peer': peer})
    return res if res.get('ok') else None


def join(target, callback=None):
    """Вступить по @username, ссылке t.me/name или приглашению t.me/+hash. Право chats.manage."""
    entry = _wrap('chats.manage', 'join', target, (5, 60))
    if entry is None:
        return None
    t = str(target).strip()
    for pre in ('https://t.me/', 'http://t.me/', 't.me/'):
        if t.startswith(pre):
            t = t[len(pre):]
    t = t.lstrip('@').split('?')[0]
    if t.startswith('+') or t.startswith('joinchat/'):
        h = t.split('/')[-1].lstrip('+')
        return _invoke(entry, 'messages.importChatInvite', {'hash': h}, callback)

    def resolved(res, err):
        chans = [c for c in (res or {}).get('chats', []) if c.get('_') == 'channel']
        if err or not chans:
            return callback(None, err or {'type': 'NOT_FOUND', 'description': 'канал не найден'}) if callable(callback) else None
        c = chans[0]
        _invoke(entry, 'channels.joinChannel', {'channel': {'_': 'inputChannel', 'channel_id': c['id'], 'access_hash': c['access_hash']}}, callback)
    return _invoke(entry, 'contacts.resolveUsername', {'username': t}, resolved)


def leave(peer, callback=None):
    """Выйти из группы или канала. Право chats.manage."""
    peer, _, ok = _ids(peer)
    entry = _wrap('chats.manage', 'leave', peer, (5, 60))
    k = _kind_of(peer) if entry is not None and ok else None
    if k is None:
        return None
    if k['kind'] == 'channel':
        ip = _tl_input(entry, 'channel', peer)[0]
        return _invoke(entry, 'channels.leaveChannel', {'channel': ip}, callback) if ip else None
    if k['kind'] == 'chat':
        return _invoke(entry, 'messages.deleteChatUser', {'chat_id': int(k['bare']), 'user_id': {'_': 'inputUserSelf'}}, callback)
    return None


def _user_input(entry, user):
    return _tl_input(entry, 'user', str(user))[0]


def invite(peer, user_ids, callback=None):
    """Пригласить людей (их id из загруженных). Право chats.manage."""
    peer, _, ok = _ids(peer)
    entry = _wrap('chats.manage', 'invite', peer, (5, 60))
    k = _kind_of(peer) if entry is not None and ok else None
    users = [_user_input(entry, u) for u in (user_ids if isinstance(user_ids, (list, tuple)) else [user_ids])][:50] if k else []
    users = [u for u in users if u]
    if k is None or not users:
        return None
    if k['kind'] == 'channel':
        ip = _tl_input(entry, 'channel', peer)[0]
        return _invoke(entry, 'channels.inviteToChannel', {'channel': ip, 'users': users}, callback) if ip else None
    if k['kind'] == 'chat':
        return _invoke(entry, 'messages.addChatUser', {'chat_id': int(k['bare']), 'user_id': users[0], 'fwd_limit': 50}, callback)
    return None


_RIGHTS = ('view_messages', 'send_messages', 'send_media', 'send_stickers', 'send_gifs', 'send_games', 'send_inline',
           'embed_links', 'send_polls', 'change_info', 'invite_users', 'pin_topics', 'pin_messages')


def _banned_rights(rights, until):
    r = {'_': 'chatBannedRights', 'until_date': int(until)}
    for k in _RIGHTS:
        if rights.get(k):
            r[k] = True
    return r


def _edit_banned(entry, peer, user, rights, until, callback):
    ip = _tl_input(entry, 'channel', peer)[0]
    up = _tl_input(entry, 'peer', str(user))[0]
    if ip is None or up is None:
        return None
    return _invoke(entry, 'channels.editBanned', {'channel': ip, 'participant': up,
                                                   'banned_rights': _banned_rights(rights, until)}, callback)


def ban(peer, user, until=0, callback=None):
    """Заблокировать в супергруппе или канале (until: unix, 0 навсегда). Право chats.manage."""
    peer, _, ok = _ids(peer)
    entry = _wrap('chats.manage', 'ban', peer, (10, 60))
    if entry is None or not ok:
        return None
    return _edit_banned(entry, peer, user, {'view_messages': True}, until, callback)


def unban(peer, user, callback=None):
    peer, _, ok = _ids(peer)
    entry = _wrap('chats.manage', 'unban', peer, (10, 60))
    if entry is None or not ok:
        return None
    return _edit_banned(entry, peer, user, {}, 0, callback)


def kick(peer, user, callback=None):
    """Убрать из группы (бан и сразу разбан, человек может вернуться по ссылке). Право chats.manage."""
    peer, _, ok = _ids(peer)
    entry = _wrap('chats.manage', 'kick', peer, (10, 60))
    k = _kind_of(peer) if entry is not None and ok else None
    if k is None:
        return None
    if k['kind'] == 'chat':
        u = _user_input(entry, user)
        return _invoke(entry, 'messages.deleteChatUser', {'chat_id': int(k['bare']), 'user_id': u}, callback) if u else None
    return _edit_banned(entry, peer, user, {'view_messages': True}, 0,
                        lambda r, e: unban(peer, user, callback) if not e else (callback(r, e) if callable(callback) else None))


def restrict(peer, user, rights, until=0, callback=None):
    """Ограничить участника: rights это dict запретов, например {'send_messages': True, 'send_media': True}. Право chats.manage."""
    peer, _, ok = _ids(peer)
    entry = _wrap('chats.manage', 'restrict', peer, (10, 60))
    if entry is None or not ok or not isinstance(rights, dict):
        return None
    return _edit_banned(entry, peer, user, rights, until, callback)


_ADMIN = ('change_info', 'post_messages', 'edit_messages', 'delete_messages', 'ban_users', 'invite_users',
          'pin_messages', 'add_admins', 'anonymous', 'manage_call', 'other', 'manage_topics')


def edit_admin(peer, user, rights, rank='', callback=None):
    """Назначить или снять админа: rights dict из {'delete_messages': True, 'ban_users': True, ...}, пустой снимает.
    Право chats.manage."""
    peer, _, ok = _ids(peer)
    entry = _wrap('chats.manage', 'edit_admin', peer, (5, 60))
    if entry is None or not ok or not isinstance(rights, dict):
        return None
    ip = _tl_input(entry, 'channel', peer)[0]
    u = _user_input(entry, user)
    if ip is None or u is None:
        return None
    ar = {'_': 'chatAdminRights'}
    for k in _ADMIN:
        if rights.get(k):
            ar[k] = True
    return _invoke(entry, 'channels.editAdminRights', {'channel': ip, 'user_id': u, 'admin_rights': ar,
                                                        'rank': str(rank)[:16]}, callback)


def mute(peer, seconds=True, callback=None):
    """Выключить уведомления чата: seconds число (на сколько), True навсегда, False включить. Право chats.settings."""
    peer, _, ok = _ids(peer)
    entry = _wrap('chats.settings', 'mute', peer)
    if entry is None or not ok:
        return None
    ip = _tl_input(entry, 'peer', peer)[0]
    if ip is None:
        return None
    if seconds is False:
        until = 0
    elif seconds is True:
        until = 2147483647
    else:
        until = int(time.time()) + int(seconds)
    return _invoke(entry, 'account.updateNotifySettings', {
        'peer': {'_': 'inputNotifyPeer', 'peer': ip},
        'settings': {'_': 'inputPeerNotifySettings', 'mute_until': until}}, callback)


def archive(peer, archived=True, callback=None):
    """Убрать чат в архив или вернуть. Право chats.settings."""
    peer, _, ok = _ids(peer)
    entry = _wrap('chats.settings', 'archive', peer)
    if entry is None or not ok:
        return None
    ip = _tl_input(entry, 'peer', peer)[0]
    if ip is None:
        return None
    return _invoke(entry, 'folders.editPeerFolders', {'folder_peers': [
        {'_': 'inputFolderPeer', 'peer': ip, 'folder_id': 1 if archived else 0}]}, callback)


def get_folders(callback):
    """Папки чатов: callback([{id, title}], error). Право chats.settings."""
    entry = _wrap('chats.settings', 'get_folders', None, (10, 60))
    if entry is None or not callable(callback):
        return None

    def done(res, err):
        if err or res is None:
            return callback(None, err)
        out = []
        for f in res.get('filters', []):
            if f.get('_') == 'dialogFilter':
                t = f.get('title')
                out.append({'id': f['id'], 'title': t.get('text') if isinstance(t, dict) else str(t)})
        callback(out, None)
    return _invoke(entry, 'messages.getDialogFilters', {}, done)


def folder_add(folder_id, peer, callback=None):
    """Добавить чат в папку. Право chats.settings."""
    peer, _, ok = _ids(peer)
    entry = _wrap('chats.settings', 'folder_add', peer, (10, 60))
    if entry is None or not ok:
        return None
    ip = _tl_input(entry, 'peer', peer)[0]
    if ip is None:
        return None

    def got(res, err):
        flt = next((f for f in (res or {}).get('filters', []) if f.get('_') == 'dialogFilter' and f.get('id') == int(folder_id)), None)
        if err or flt is None:
            return callback(None, err or {'type': 'NOT_FOUND', 'description': 'папки нет'}) if callable(callback) else None
        flt = dict(flt)
        flt['include_peers'] = list(flt.get('include_peers', [])) + [ip]
        _invoke(entry, 'messages.updateDialogFilter', {'id': int(folder_id), 'filter': flt}, callback)
    return _invoke(entry, 'messages.getDialogFilters', {}, got)


_ACTIONS = {'typing': 'sendMessageTypingAction', 'record_voice': 'sendMessageRecordAudioAction',
            'record_video': 'sendMessageRecordVideoAction', 'upload_photo': 'sendMessageUploadPhotoAction',
            'choose_sticker': 'sendMessageChooseStickerAction', 'cancel': 'sendMessageCancelAction'}


def set_typing(peer, action='typing', callback=None):
    """Показать собеседникам «печатает...» и т. п. (typing, record_voice, record_video, upload_photo, choose_sticker, cancel).
    Право chats.settings."""
    peer, _, ok = _ids(peer)
    entry = _wrap('chats.settings', 'set_typing', peer, (60, 60))
    if entry is None or not ok or action not in _ACTIONS:
        return None
    ip = _tl_input(entry, 'peer', peer)[0]
    if ip is None:
        return None
    act = {'_': _ACTIONS[action]}
    if action == 'upload_photo':
        act['progress'] = 0
    return _invoke(entry, 'messages.setTyping', {'peer': ip, 'action': act}, callback)


def set_profile(first_name=None, last_name=None, about=None, callback=None):
    """Имя, фамилия, «О себе» (что не указано, то не меняется). Право profile.write."""
    entry = _wrap('profile.write', 'set_profile', None, (5, 60))
    if entry is None:
        return None
    params = {}
    if first_name is not None:
        params['first_name'] = str(first_name)[:64]
    if last_name is not None:
        params['last_name'] = str(last_name)[:64]
    if about is not None:
        params['about'] = str(about)[:70]
    if not params:
        return None
    return _invoke(entry, 'account.updateProfile', params, callback)


def set_emoji_status(document_id=None, until=None, callback=None):
    """Эмодзи-статус по document_id премиум-эмодзи (None снимает). Право profile.write, нужен Premium."""
    entry = _wrap('profile.write', 'set_emoji_status', None, (5, 60))
    if entry is None:
        return None
    if document_id is None:
        status = {'_': 'emojiStatusEmpty'}
    else:
        status = {'_': 'emojiStatus', 'document_id': int(document_id)}
        if until:
            status['until'] = int(until)
    return _invoke(entry, 'account.updateEmojiStatus', {'emoji_status': status}, callback)


_PRIVACY_KEYS = {'status': 'inputPrivacyKeyStatusTimestamp', 'phone': 'inputPrivacyKeyPhoneNumber',
                 'photo': 'inputPrivacyKeyProfilePhoto', 'forwards': 'inputPrivacyKeyForwards',
                 'calls': 'inputPrivacyKeyPhoneCall', 'invites': 'inputPrivacyKeyChatInvite',
                 'about': 'inputPrivacyKeyAbout', 'birthday': 'inputPrivacyKeyBirthday'}
_PRIVACY_RULES = {'everyone': 'inputPrivacyValueAllowAll', 'contacts': 'inputPrivacyValueAllowContacts',
                  'nobody': 'inputPrivacyValueDisallowAll'}


def get_privacy(key, callback):
    """Кому видно: callback(['everyone'|'contacts'|'nobody', ...], error). key: status, phone, photo, forwards, calls, invites, about, birthday."""
    entry = _wrap('privacy.write', 'get_privacy', key, (10, 60))
    if entry is None or key not in _PRIVACY_KEYS or not callable(callback):
        return None

    def done(res, err):
        if err or res is None:
            return callback(None, err)
        names = {'privacyValueAllowAll': 'everyone', 'privacyValueAllowContacts': 'contacts',
                 'privacyValueDisallowAll': 'nobody', 'privacyValueDisallowContacts': 'not_contacts',
                 'privacyValueAllowUsers': 'users', 'privacyValueDisallowUsers': 'except_users'}
        callback([names.get(r.get('_'), r.get('_')) for r in res.get('rules', [])], None)
    return _invoke(entry, 'account.getPrivacy', {'key': {'_': _PRIVACY_KEYS[key]}}, done)


def set_privacy(key, rule, callback=None):
    """Поставить приватность: rule 'everyone' | 'contacts' | 'nobody'. Право privacy.write."""
    entry = _wrap('privacy.write', 'set_privacy', key, (5, 60))
    if entry is None or key not in _PRIVACY_KEYS or rule not in _PRIVACY_RULES:
        return None
    return _invoke(entry, 'account.setPrivacy', {'key': {'_': _PRIVACY_KEYS[key]},
                                                  'rules': [{'_': _PRIVACY_RULES[rule]}]}, callback)


def register_command(name, handler, description=''):
    """Команда в композере: пользователь пишет /name аргументы, сообщение не уходит, зовётся
    handler(ctx) с ctx = {'args': 'аргументы', 'peer': id чата, 'text': весь текст}."""
    entry = _gate('register_command', 'ui')
    if entry is None or not callable(handler):
        return None
    name = str(name).strip().lower().lstrip('/')
    if not name or not name.replace('_', '').isalnum() or len(name) > 32:
        return None
    entry.setdefault('commands', {})[name] = (handler, str(description)[:100])
    return True


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
)PY";
