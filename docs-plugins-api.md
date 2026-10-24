# Mokagram: API плагинов (Python)

Файл `.plugin` с классом, наследующим `BasePlugin`. Метаданные: `__id__`, `__name__`, `__description__`, `__author__`, `__version__`, `__permissions__`.
`from base_plugin import ...` и `from mokagram_sdk import api` (или `import moka`).

## Права
`messages.read` (чтение, события, chat_info), `messages.send` (send_message, send_file), `messages.edit`, `network`, `files`.
Без права вызов возвращает `None`, плагин не падает. Всё пишется в журнал действий. Выдаются при установке.

## Пункты интерфейса
`self.add_menu_item(MenuItemData(menu_type=..., text, on_click(ctx), icon, priority, condition))` возвращает id, `remove_menu_item(id)`.
Места: `MESSAGE_CONTEXT_MENU` (ctx: id, peer, text, selected, out, has_media, date), `CHAT_ACTION_MENU` (меню чата и строки списка),
`PROFILE_ACTION_MENU`, `ATTACH_MENU` (скрепка), `DRAWER_MENU`/`MAIN_MENU` (боковое меню). Для чатов ctx: peer, title, kind, username.
Иконки по имени: copy, delete, edit, info, send, settings, search, translate, share, link, pin, forward, reply, bot, lock, palette, premium, timer и ещё ~70 (см. `MenuIconByName`).

## Окна и тосты
`api.show_dialog(title, message, fields, buttons, on_result(button_id, values), on_close)` -> id.
Поля: `Header, Text, Switch, Selector, Input` из base_plugin или dict `{'type': 'progress', 'default': 40}`.
`api.update_dialog(id, message=, progress=, title=)`, `api.close_dialog(id)`, `api.confirm(...)`, `api.prompt(...)`.
`api.show_toast(text)`, `api.show_bulletin(text, kind, subtitle, button, on_click, duration)`.

## События (право messages.read)
`on_new_message(m)`, `on_message_edited(m)`, `on_message_deleted(m)`; m: id, peer, peer_title, from, text, out, date, has_media, reply_to.

## Действия
`send_message`, `send_file(peer, path, caption, as_photo)`, `edit_message(peer, id, text)`, `get_message(peer, id)`, `chat_info(peer)`,
`open_chat(peer)`, `open_url(url)`, `copy_text(text)`.
`get_dialogs(limit)`, `get_history(peer, limit)` (только загруженные), `current_chat()`, `mark_read(peer)`.
Команды в композере: `api.register_command('name', handler, description)`: пишешь `/name аргументы`, сообщение не уходит, зовётся `handler({'args', 'peer', 'text'})`.

## Потоки и таймеры
`set_timeout(fn, sec)`, `set_interval(fn, sec)`, `cancel_timer(id)`, `run_on_ui_thread(fn, delay_ms)`, `run_on_queue(fn, delay_ms)`.

## Хранилище и настройки
`api.storage` (get/set/delete/keys), `get_setting/set_setting`, `create_settings()` со списком Header/Switch/Selector/Input/Text/Divider.

## Хуки
`send_message_hook(params)`, `on_update_hook`, `on_updates_hook`, `pre_request_hook`, `post_request_hook` с `HookResult(strategy, value)`.
