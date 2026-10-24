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

    # события клиента (нужно право messages.read): message это dict
    # {id, peer, peer_title, from, text, out, date, has_media, reply_to}
    def on_new_message(self, message: dict) -> None: ...
    def on_message_edited(self, message: dict) -> None: ...
    def on_message_deleted(self, message: dict) -> None: ...

    # интерфейс
    def add_menu_item(self, item: MenuItem) -> None:
        # плагин мог не вызвать super().__init__()
        if not hasattr(self, "_menu_items"):
            self._menu_items = []
        self._menu_items.append(item)


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
