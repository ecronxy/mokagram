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


class BasePlugin:
    """Наследуйте и переопределяйте нужные методы. Метаданные задаются переменными модуля:
    __id__, __name__, __description__, __author__, __version__, __icon__, __min_version__, __permissions__.
    """

    def __init__(self) -> None:
        self._menu_items: list[MenuItem] = []

    # жизненный цикл
    def on_plugin_load(self) -> None: ...
    def on_plugin_unload(self) -> None: ...
    def on_app_event(self, event: str) -> None: ...   # app_start, app_stop, app_pause, app_resume
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

    # события клиента (нужно право messages.read): message это dict
    # {id, peer, peer_title, from, text, entities, out, date, has_media, reply_to}
    def on_new_message(self, message: dict) -> None: ...
    def on_message_edited(self, message: dict) -> None: ...
    def on_message_deleted(self, message: dict) -> None: ...
    def on_new_reaction(self, message: dict) -> None: ...   # на ваше сообщение поставили реакцию (поля как у on_new_message)

    # люди и чаты (нужно право messages.read)
    def on_chat_opened(self, peer: dict) -> None: ...        # {peer, title, username, kind}
    def on_peer_updated(self, peer: dict) -> None: ...       # + changed: name|username|photo|about|blocked|emoji_status
    def on_user_status(self, peer: dict) -> None: ...        # + online: bool

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
