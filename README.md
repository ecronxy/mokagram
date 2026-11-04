<p align="center">
  <img src="docs/readme/logo.png" alt="mokaGram" width="120">
</p>

<h1 align="center">mokaGram</h1>

<p align="center">
  Клиент на основе Telegram Desktop с переработанным дизайном и новыми функциями.
</p>

<p align="center">
  <a href="https://github.com/mokagram/mokagram/releases"><img src="https://img.shields.io/github/v/release/mokagram/mokagram?include_prereleases&style=for-the-badge&label=релиз&color=0a84ff&labelColor=282828" alt="Релиз"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/лицензия-GPLv3-8e8e93?style=for-the-badge&labelColor=282828" alt="GPLv3"></a>
  <a href="https://t.me/ecronx"><img src="https://img.shields.io/badge/автор-@ecronx-8e8e93?style=for-the-badge&labelColor=282828&logo=telegram&logoColor=white" alt="@ecronx"></a>
</p>

<p align="center">
  <img src="docs/readme/chat-full.webp" alt="mokaGram: окно чата" width="86%">
</p>

## О проекте

**mokaGram** — кастомный форк Telegram Desktop с полностью переработанным визуалом в md3-стиле и кучей новых полезных функций. Все секции в настройках лежат отдельными скруглёнными карточками, каждая функция включается отдельно, и включать нужно только то, что нужно.

Клиент работает с обычным аккаунтом Telegram и обычными серверами. Плагины из экстеры, но на ПК: свой API на Python и магазин прямо в клиенте.

## Что умеет

- **Приватность**: режим стримера прячет номер, юзернейм и ID, режим призрака не отмечает прочитанным и скрывает, что ты в сети
- **Удалённые и правки** сообщений
- **Безопасность**
- **Музыка** с текстом песни и подсветкой строки
- **Быстрые команды**
- **Система**: анти-лаг и экономия в фоне
- **Кастомизация** интерфейса
- **Инструменты**: скриншот сообщений карточкой
- **Плагины** на Python и магазин прямо в клиенте

Всё лежит в одном месте: **Настройки → Настройки mokaGram**. Подробно со скриншотами: [docs/FEATURES.md](docs/FEATURES.md).

## Скачать

Последняя сборка: **[v0.1.3](https://github.com/mokagram/mokagram/releases)**

| Система | Файл |
|---|---|
| **Windows** | [mokagram-win-x64.zip](https://github.com/mokagram/mokagram/releases/download/v0.1.3/mokagram-win-x64.zip) |
| **Linux** | [AppImage](https://github.com/mokagram/mokagram/releases/download/v0.1.3/mokagram-x86_64.AppImage) · [.deb](https://github.com/mokagram/mokagram/releases/download/v0.1.3/mokagram_0.1.3_amd64.deb) · [.rpm](https://github.com/mokagram/mokagram/releases/download/v0.1.3/mokagram-0.1.3-1.x86_64.rpm) · [pacman](https://github.com/mokagram/mokagram/releases/download/v0.1.3/mokagram-0.1.3-1-x86_64.pkg.tar.zst) · [tar.xz](https://github.com/mokagram/mokagram/releases/download/v0.1.3/mokagram-linux-x64.tar.xz) |

Сборка для Windows без подписи разработчика, система может предупредить при первом запуске.

## Плагины

Плагин — один файл `.plugin` с классом, наследующим `BasePlugin`. Метаданные, права и команды лежат в самом файле, лимит 300 действий в минуту, каждое действие пишется в журнал.

Полное описание: [docs-plugins-api.md](docs-plugins-api.md), пример: [examples/showcase.plugin](examples/showcase.plugin).

## Сборка

mokaGram собирается так же, как Telegram Desktop:

- [Windows](docs/building-win.md)
- [GNU/Linux (Docker)](docs/building-linux.md)

Устройство репозитория: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md). Исходники Telegram Desktop и список сторонних библиотек: [telegramdesktop/tdesktop](https://github.com/telegramdesktop/tdesktop).

## Контрибьюторы

<a href="https://github.com/ecronxy"><img src="https://github.com/ecronxy.png?size=96" width="48" height="48" alt="ecronxy" title="ecronxy"></a>
<a href="https://github.com/xdshkaaa"><img src="https://github.com/xdshkaaa.png?size=96" width="48" height="48" alt="xdshkaaa" title="xdshkaaa"></a>
<a href="https://github.com/kierradev"><img src="https://github.com/kierradev.png?size=96" width="48" height="48" alt="kierradev" title="kierradev"></a>
<a href="https://github.com/misterbazed"><img src="https://github.com/misterbazed.png?size=96" width="48" height="48" alt="misterbazed" title="misterbazed"></a>

## Авторство и лицензия

> **Основано на mokaGram, автор [@ecronx](https://t.me/ecronx) (https://t.me/ecronx). https://github.com/mokagram/mokagram**

mokaGram основан на [Telegram Desktop](https://github.com/telegramdesktop/tdesktop) (© The Telegram Desktop Authors, GPLv3 с исключением OpenSSL). Доработки © 2026 [@ecronx](https://t.me/ecronx), лицензия та же, GPLv3, плюс условие об сохранении авторства.

Кто берёт исходники или сборки mokaGram, обязан сохранить [уведомление](MOKAGRAM_NOTICE.md) и указать авторство в README и в окне «О программе». При нарушении лицензия прекращается.

*Anyone using mokaGram source code **must credit the author** ([@ecronx](https://t.me/ecronx), https://github.com/mokagram/mokagram) in the README and the About window and keep the [notice](MOKAGRAM_NOTICE.md).*

<p align="center">
  <a href="https://t.me/ecronx">@ecronx</a> · <a href="https://github.com/mokagram/mokagram/releases">релизы</a>
</p>
