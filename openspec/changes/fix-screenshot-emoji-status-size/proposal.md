# Proposal

## Why

В диалоге «Скриншот сообщений» эмодзи-статус («эмблемка») рядом с именем отправителя рисуется заметно меньше, чем в самом чате. Снимок должен выглядеть как в чате, только чётче.

## What Changes

- Эмодзи-статус у имени в скриншоте рисуется того же логического размера, что и в чате, при любом качестве снимка (`quote.quality`).
- Затрагивается только путь отрисовки снимка; обычный чат не меняется.

## Capabilities

### New Capabilities
- `message-screenshot`: поведение скриншота сообщений (внешний вид содержимого относительно чата).

### Modified Capabilities

## Impact

- `Telegram/SourceFiles/mokagram/quote/mokagram_quote.cpp` (`RenderShot`, `EmojiScale`).
- `Telegram/lib_ui/ui/text/custom_emoji_instance.cpp` (отрисовка кадров/превью кастомных эмодзи при `ShotScale() > 1`).
- `Telegram/SourceFiles/data/stickers/data_custom_emoji.cpp` (раздельные экземпляры эмодзи для снимка).
