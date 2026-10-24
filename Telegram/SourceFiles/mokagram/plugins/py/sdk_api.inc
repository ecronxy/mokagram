constexpr auto kApiScript = R"PY(
"""Mokagram SDK: вызовы плагина к клиенту (mokagram_sdk.api, он же moka).

Каждый вызов проверяет право плагина (host.py знает, кто вызвал, по модулю в стеке),
пишет решение в журнал действий и только потом просит клиент выполнить действие.
Нет права: запись в журнал и None, плагин не падает.
"""
import json, os, time, collections

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
)PY";
