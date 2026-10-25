constexpr auto kSdkScript = R"PY(
"""Mokagram SDK: базовые классы плагина (скелет, 22.10.2026).

Имена хуков и стратегий повторяют контракт exteraless (GPL-3.0). Реализация своя.
Хост на C++ вызывает методы этого класса; сейчас это заглушки, чтобы спецификация была исполняемой.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable, Optional


class HookStrategy(str, Enum):
    DEFAULT = "DEFAULT"          # ничего не менять
    CANCEL = "CANCEL"            # отменить действие
    MODIFY = "MODIFY"            # заменить значение, следующие плагины ещё видят его
    MODIFY_FINAL = "MODIFY_FINAL"  # заменить и остановить цепочку


@dataclass
class HookResult:
    strategy: HookStrategy = HookStrategy.DEFAULT
    value: Any = None


@dataclass
class MenuItem:
    location: str                      # "message" | "sticker" | "chat" | "attach"
    text: str
    icon: Optional[str] = None
    condition: Optional[Callable[[Any], bool]] = None
    action: Optional[Callable[[Any], None]] = None
    priority: int = 0                  # больше значит выше в меню
    item_id: Optional[str] = None


class _Logger:
    """self.logger.info(...) и т. д.: строки попадают в журнал плагинов (команда .logs)."""

    def __init__(self, pid):
        self._pid = pid

    def _put(self, level, msg, args):
        import sys
        try:
            text = msg % args if args else str(msg)
        except Exception:
            text = str(msg)
        sys.modules['mokagram_sdk.api']._h['say']('[%s] %s: %s' % (self._pid, level, text))

    def debug(self, msg, *args): self._put('DEBUG', msg, args)
    def info(self, msg, *args): self._put('INFO', msg, args)
    def warning(self, msg, *args): self._put('WARN', msg, args)
    def error(self, msg, *args): self._put('ERROR', msg, args)

    def exception(self, msg, *args):
        import traceback
        self._put('ERROR', msg, args)
        self._put('ERROR', traceback.format_exc().rstrip(), ())


def command(name=None, description='', aliases=()):
    """Метод плагина становится командой: @command('hello', 'приветствие'). Вызывается handler(ctx)."""
    def deco(fn):
        fn._moka_cmd = (name or fn.__name__, description, tuple(aliases))
        return fn
    return deco


def watcher(**filters):
    """Метод зовётся на подходящие сообщения: @watcher(chats='group', incoming=True, regex='привет').
    Фильтры: chats ('private'|'group'|'channel'|'bot' или список), out (True/False), incoming, regex,
    chat_ids, from_ids, media (True/False/'photo'/'video'/'voice'/'audio'/'sticker'/'document' или список)."""
    def deco(fn):
        fn._moka_watcher = dict(filters)
        return fn
    return deco


class BasePlugin:
    """Наследуйте и переопределяйте нужные методы. Метаданные задаются переменными модуля:
    __id__, __name__, __description__, __author__, __version__, __icon__, __min_version__, __permissions__.
    """

    strings: dict = {}   # {'ru': {'hello': 'Привет'}, 'en': {...}}, берётся язык клиента

    def __init__(self) -> None:
        self._menu_items: list[MenuItem] = []

    def s(self, key: str, *args, **kwargs) -> str:
        """Строка из self.strings на языке клиента (ru или en), иначе английская, русская, иначе сам ключ."""
        import sys
        lang = sys.modules['mokagram_sdk.api']._h['fw']['lang']()
        table = getattr(self, 'strings', None) or {}
        for code in (lang, 'en', 'ru'):
            text = (table.get(code) or {}).get(key)
            if text is not None:
                try:
                    return text.format(*args, **kwargs)
                except Exception:
                    return text
        return key

    def get_secret(self, key: str):
        """Секрет плагина (поле Password или api.secrets.set) или None."""
        import sys
        api = sys.modules['mokagram_sdk.api']
        return api.secrets.get(key)

    def set_secret(self, key: str, value) -> bool:
        import sys
        return sys.modules['mokagram_sdk.api'].secrets.set(key, value)

    @property
    def logger(self):
        return _Logger(getattr(self, '_moka_id', 'plugin'))

    # жизненный цикл
    def on_plugin_load(self) -> None: ...
    def on_plugin_unload(self) -> None: ...
    def on_app_event(self, event) -> None: ...   # AppEvent.START | STOP | PAUSE | RESUME (права не нужны)
    def create_settings(self) -> list: return []   # Header, Divider, Text, Switch, Selector, Input
    def on_settings_changed(self, key: str, value: Any) -> None: ...

    # хуки: возвращайте HookResult
    def send_message_hook(self, params: dict) -> HookResult: return HookResult()
    def on_update_hook(self, update: Any) -> HookResult: return HookResult()
    def on_updates_hook(self, updates: Any) -> HookResult: return HookResult()
    def pre_request_hook(self, request: Any) -> HookResult: return HookResult()
    def post_request_hook(self, response: Any, error: Any = None) -> HookResult: return HookResult()

    # скрыть сообщение в чате (только у вас, на сервере ничего не меняется): верните True или HookResult(CANCEL).
    # Зовётся один раз на сообщение (результат кэшируется), нужно право messages.read.
    def message_filter_hook(self, message: dict) -> Any: return False

    # заменить текст входящего сообщения при показе (только у вас): верните новую строку или None.
    # message: {id, peer, text, entities, date}. Верните строку, либо {'text': ..., 'entities': [...]}, либо (текст, сущности).
    # Копирование и поиск увидят уже новый текст. Нужно право messages.read.
    def message_text_hook(self, message: dict) -> Any: return None

    # короткая подпись (до 40 знаков) в подвале пузыря рядом со временем: верните строку или None.
    # message: {id, peer, text, entities, buttons, out, date, has_media}. Зовётся один раз на сообщение (результат кэшируется
    # до правки сообщения), бюджет 50 мс. Нужно право messages.read.
    def message_badge_hook(self, message: dict) -> Optional[str]: return None

    # события клиента (нужно право messages.read): message это dict
    # {id, peer, peer_title, from, text, entities, out, date, has_media, reply_to}
    def on_new_message(self, message: dict) -> None: ...
    def on_message_edited(self, message: dict) -> None: ...
    def on_message_deleted(self, message: dict) -> None: ...
    def on_new_reaction(self, message: dict) -> None: ...
    def on_message_sent(self, info: dict) -> None: ...
    def on_chat_action(self, info: dict) -> None: ...        # {peer, action: join|leave|add|remove|pin|title|photo|create|other, from, user_ids, message_id}
    def on_read(self, info: dict) -> None: ...               # {peer, max_id, direction: inbox|outbox}
    def on_story(self, info: dict) -> None: ...              # {peer, id, date, caption, media}
    def on_album(self, info: dict) -> None: ...              # {peer, grouped_id, messages: [...], text, out}      # {id, old_id, peer, text}: ваше сообщение ушло, id настоящий   # на ваше сообщение поставили реакцию (поля как у on_new_message)

    # люди и чаты (нужно право messages.read)
    def on_chat_opened(self, peer: dict) -> None: ...        # {peer, title, username, kind}
    def on_peer_updated(self, peer: dict) -> None: ...       # + changed: name|username|photo|about|blocked|emoji_status
    def on_user_status(self, peer: dict) -> None: ...        # + online: bool
    def on_typing(self, info: dict) -> None: ...             # {peer, from, from_title, action: typing|record_voice|upload_photo|...|cancel}

    # интерфейс
    def add_menu_item(self, item: MenuItem) -> None:
        # плагин мог не вызвать super().__init__()
        if not hasattr(self, "_menu_items"):
            self._menu_items = []
        self._menu_items.append(item)
        if not getattr(item, 'item_id', None):
            try:
                item.item_id = 'item%d' % len(self._menu_items)
            except Exception:
                pass
        return item.item_id

    def remove_menu_item(self, item_id) -> None:
        for item in getattr(self, "_menu_items", []):
            if getattr(item, 'item_id', None) == item_id:
                item._removed = True


# ---- элементы настроек плагина (хост кладёт их в реестр настроек клиента, ключ plugin.<id>.<key>) ----

@dataclass
class Header:
    text: str = ""


@dataclass
class Divider:
    text: Optional[str] = None


@dataclass
class Text:
    text: str = ""
    subtext: Optional[str] = None
    icon: Optional[str] = None
    on_click: Optional[Callable[[Any], None]] = None   # с on_click строка становится кнопкой


@dataclass
class Button:
    text: str = ""
    subtext: Optional[str] = None
    icon: Optional[str] = None
    on_click: Optional[Callable[[Any], None]] = None


@dataclass
class Switch:
    key: str = ""
    text: str = ""
    default: bool = False
    subtext: Optional[str] = None
    icon: Optional[str] = None
    on_change: Optional[Callable[[Any], None]] = None


@dataclass
class Selector:
    key: str = ""
    text: str = ""
    items: Optional[list] = None
    default: int = 0
    icon: Optional[str] = None
    on_change: Optional[Callable[[Any], None]] = None


@dataclass
class Input:
    key: str = ""
    text: str = ""
    default: str = ""
    subtext: Optional[str] = None
    icon: Optional[str] = None
    on_change: Optional[Callable[[Any], None]] = None


@dataclass
class Password:
    """Секретное поле в настройках: значение вводится в окне с маской и хранится зашифрованно,
    в файл настроек не попадает. Читать: self.get_secret(key)."""
    key: str = ""
    text: str = ""
    subtext: Optional[str] = None
    icon: Optional[str] = None


EditText = Input


# ---- имена из exteraGram, чтобы `from base_plugin import ...` в чужих плагинах находил их ----

class MenuItemType(str, Enum):
    MESSAGE_CONTEXT_MENU = "MESSAGE_CONTEXT_MENU"
    DRAWER_MENU = "DRAWER_MENU"
    CHAT_ACTION_MENU = "CHAT_ACTION_MENU"
    PROFILE_ACTION_MENU = "PROFILE_ACTION_MENU"
    ATTACH_MENU = "ATTACH_MENU"       # меню скрепки в композере
    MAIN_MENU = "MAIN_MENU"
    COMPOSER_BUTTON = "COMPOSER_BUTTON"   # кнопка с иконкой рядом с полем ввода
    HEADER_BUTTON = "HEADER_BUTTON"   # кнопка с иконкой в шапке чата


@dataclass
class MenuItemData:
    menu_type: Any = None
    text: str = ""
    on_click: Optional[Callable[..., None]] = None
    item_id: Optional[str] = None
    icon: Optional[str] = None
    subtext: Optional[str] = None
    condition: Optional[str] = None
    priority: int = 0


class AppEvent(str, Enum):
    START = "START"
    STOP = "STOP"
    PAUSE = "PAUSE"
    RESUME = "RESUME"


class MethodHook:
    def before_hooked_method(self, param): ...
    def after_hooked_method(self, param): ...


class MethodReplacement:
    def replace_hooked_method(self, param): ...


XposedHook = MethodHook
)PY";
