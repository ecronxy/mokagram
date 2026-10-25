# Mokagram: API плагинов (Python)

Файл `.plugin` с классом, наследующим `BasePlugin`. Метаданные: `__id__`, `__name__`, `__description__`, `__author__`, `__version__`, `__permissions__`.
`from base_plugin import ...` и `from mokagram_sdk import api` (или `import moka`).

## Права
`messages.read` (чтение, события, watchers, chat_info, get_user_info), `messages.send` (отправка, пересылка, реакции, альбомы),
`messages.edit` (правка, закреп), `messages.delete` (удаление), `tl.read` / `tl.write` (invoke_tl), `clipboard`, `network`, `files`.
Без права вызов возвращает `None`, плагин не падает. Каждое действие пишется в журнал. Права выдаются при установке.
У плагина общий лимит 300 действий в минуту, у отдельных действий свои (отправка 20, пересылка 10, удаление 5 и т. д.).
Пути к файлам для `send_album` и `download_media` допустимы только в папке данных плагина или во временной, иначе нужно право `files`.

## Пункты интерфейса
`self.add_menu_item(MenuItemData(menu_type=..., text, on_click(ctx), icon, priority, condition))` возвращает id, `remove_menu_item(id)`.
Кнопки с иконкой: `HEADER_BUTTON` (шапка чата), `COMPOSER_BUTTON` (рядом с полем ввода), ctx: peer, title, kind, username.
Места меню: `MESSAGE_CONTEXT_MENU` (ctx: id, peer, text, selected, out, has_media, date), `CHAT_ACTION_MENU` (меню чата и строки списка),
`PROFILE_ACTION_MENU`, `ATTACH_MENU` (скрепка), `DRAWER_MENU`/`MAIN_MENU` (боковое меню). Для чатов ctx: peer, title, kind, username.
Иконки по имени: copy, delete, edit, info, send, settings, search, translate, share, link, pin, forward, reply, bot, lock, palette, premium, timer и ещё ~70 (см. `MenuIconByName`).

## Окна и тосты
`api.show_dialog(title, message, fields, buttons, on_result(button_id, values), on_close)` -> id.
Поля: `Header, Text, Switch, Selector, Input` из base_plugin или dict `{'type': 'progress', 'default': 40}`.
`api.update_dialog(id, message=, progress=, title=)`, `api.close_dialog(id)`, `api.confirm(...)`, `api.prompt(...)`.
`api.show_toast(text)`, `api.show_bulletin(text, kind, subtitle, button, on_click, duration)`.

## События (право messages.read)
`on_new_message(m)`, `on_message_edited(m)`, `on_message_deleted(m)`, `on_new_reaction(m)`;
m: id, peer, peer_title, peer_kind (user|bot|group|channel), from, text, entities, out, date, has_media, media (photo|video|voice|round|audio|sticker|gif|document), reply_to, buttons.
`on_message_sent(info)`: ваше сообщение ушло, info: id (настоящий), old_id (локальный), peer, text. Только с настоящим id работают реакция, закреп, удаление.
`on_chat_opened(peer)`, `on_peer_updated(peer)` (+changed: name, username, photo, about, blocked, emoji_status), `on_user_status(peer)` (+online), `on_typing(info)` (peer, from, from_title, action).
`on_app_event(event)`: `AppEvent.START | STOP | PAUSE | RESUME`, права не нужны. Колбэки событий выполняются в отдельном потоке с таймаутом 5 с.

## Действия
`send_message`, `send_file(peer, path, caption, as_photo)`, `edit_message(peer, id, text)`, `get_message(peer, id)`, `chat_info(peer)`,
`open_chat(peer)`, `open_url(url)`, `copy_text(text)`.
`get_dialogs(limit)`, `get_history(peer, limit)` (только загруженные), `current_chat()`, `mark_read(peer)`.
Остальное: `forward_messages(from, ids, to, cb)`, `react(peer, id, emoji, cb)` (число вместо эмодзи это premium-эмодзи), `pin_message(peer, id, pin)`,
`delete_messages(peer, ids, revoke)` и `delete_message`, `get_me()`, `get_user_info(peer)` и `get_user`, `get_participants(peer, cb, limit)`,
`search_messages(peer, query, cb)` и `get_history_remote(peer, cb)` (с сервера, не только загруженное), `clipboard_get()`,
`download_media(peer, id, path, cb)`, `send_album(peer, paths, caption)` (2-10 файлов), `press_button(peer, id, button, cb)` (инлайн-кнопка бота из `m['buttons']`).
Методы с `cb` отвечают асинхронно: `cb(result, error)`.

## Команды (каркас)
Метод класса становится командой декоратором: `@command('hello', 'описание', aliases=['hi'])`. Зовётся `handler(ctx)`.
Префикс задаёт пользователь (Кастомизация → Плагины → Префикс команд, по умолчанию `.`), `/` тоже работает.
`ctx`: `args` (текст после команды), `args_list` (разобрано по пробелам и кавычкам), `args_entities` (форматирование аргументов), `peer`, `text`,
`reply_to` и `reply_message` (на что ответили; dict как у get_message), `prefix`, `name`.
Результат: верните строку (или `(текст, entities)`) или вызовите `ctx.edit(...)`, и команда превратится в этот текст прямо в поле ввода, лишнего сообщения нет.
`ctx.reply(текст)` отправляет отдельное сообщение. Обработчик может быть `async def`: `await ctx.edit('думаю...')`, `await asyncio.sleep(2)`, `await ctx.edit('готово')`:
сообщение отправится первым вызовом, следующие вызовы правят его. Async-команда прерывается через 60 с.
Встроенная `.help` (или `.help имя`) собирает справку из описаний всех команд. Старый `api.register_command(name, handler, description)` работает.

## Watchers
`@watcher(chats='group', incoming=True, regex='привет', media='photo')`: метод зовётся на каждое подходящее сообщение, как `on_new_message`, но с фильтрами.
Фильтры: `chats` (`'private'|'group'|'channel'|'bot'` или список), `out` (True/False), `incoming`, `regex` (без учёта регистра), `chat_ids`, `from_ids`, `media` (True/False/тип/список).
Нужно право `messages.read`.

## Форматирование
Сущности (entities) принимают и отдают `send_message(..., entities=)`, `edit_message(..., entities=)`, `message_text_hook`, события, `get_message`, `ctx`.
Вид: `{'type': 'bold|italic|underline|strike|code|pre|spoiler|quote|link|custom_emoji', 'offset': 0, 'length': 3, 'url': ..., 'language': ..., 'document_id': ...}`,
смещения в UTF-16 как в Telegram. `custom_emoji` это премиум-эмодзи по `document_id`.

## Универсальный вызов Telegram API
`api.invoke_tl('messages.getHistory', {...}, callback)` вызывает любой метод схемы. Объекты это dict с ключом `'_'` (имя конструктора), флаги считаются сами.
`api.input_peer(peer)` даёт InputPeer для своих запросов. Права: `tl.read` для методов get/search/resolve/check, `tl.write` для остальных.
Закрыты совсем: входа (`auth.*`), платежей, паролей, сеансов и данных паспорта (`account.*` кроме безопасного чтения), `phone.*`, `bots.*`,
удаление каналов и групп, `invokeWith*`. При FLOOD_WAIT до 60 с запрос повторяется сам (до 3 раз); лимит 60 вызовов в минуту.

## Скрытие сообщений
`message_filter_hook(m)` возвращает True, чтобы скрыть сообщение у себя (на сервере ничего не меняется). Вызывается один раз на сообщение, результат кэшируется. Нужно право `messages.read`.

## Подпись в пузыре
`message_badge_hook(m)` возвращает короткую строку (до 40 знаков) или None: она встаёт в подвал пузыря рядом со временем. Результат кэшируется, бюджет 50 мс.

## Замена текста
`message_text_hook(m)` (m: id, peer, text, entities, date) возвращает новую строку, `{'text', 'entities'}` или None. Работает для входящих сообщений при показе, на сервере ничего не меняется; копирование и поиск видят уже новый текст. Форматирование (жирный, ссылки-сущности) в заменённом тексте теряется, обычные ссылки определяются заново. Нужно право `messages.read`.

## Потоки и таймеры
`set_timeout(fn, sec)`, `set_interval(fn, sec)`, `cancel_timer(id)`, `run_on_ui_thread(fn, delay_ms)`, `run_on_queue(fn, delay_ms)`.

## Хранилище и настройки
`api.storage` (get/set/delete/keys), `get_setting/set_setting`, `create_settings()` со списком Header/Switch/Selector/Input/Text/Divider.

## Настройки плагина
`create_settings()` возвращает `Header(text)` (начинает новую группу), `Switch`, `Selector`, `Input`, `Text` и `Button(text, subtext, icon, on_click)`. `Button` (или `Text` с `on_click`) это строка-кнопка в Кастомизация → Плагины; из `on_click` можно открыть `show_dialog` как подстраницу.

## Разработка
Кастомизация → Плагины → Режим разработчика: плагин перезагружается сам при сохранении файла, без повторного подтверждения прав.
Если плагин не загрузился, показывается окно с причиной и строкой. Ошибки хуков и команд попадают в журнал и тост.

## Хуки
`send_message_hook(params)` (params: text, entities, peer, reply_to), `on_update_hook`, `on_updates_hook`, `pre_request_hook`, `post_request_hook` с `HookResult(strategy, value)`.

Пример со всем сразу: `examples/showcase.plugin`.
