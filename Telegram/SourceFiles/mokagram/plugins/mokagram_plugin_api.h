// Mokagram: SDK плагинов на стороне клиента (мост к Qt) и входы хуков обновлений.
#pragma once

#include "mtproto/sender.h"
#include "ui/text/text_entity.h"

class HistoryItem;

namespace Mokagram::Plugins {

// Наблюдающие хуки. Если ни один плагин не подписан на этот constructor id,
// возвращаются сразу, не трогая Python.
void RunUpdateHook(const MTPUpdate &update);
void RunUpdatesHook(const MTPUpdates &updates);

// Скрывает ли сообщение какой-то плагин (message_filter_hook). Для горячего пути:
// без подписанных плагинов это одна проверка атомарной переменной, результат кэшируется.
[[nodiscard]] bool HiddenByPlugin(not_null<const HistoryItem*> item);
[[nodiscard]] bool HasMessageFilter();

// Плагины могут заменить текст сообщения при показе (message_text_hook).
// Зовётся из HistoryItem::setText, только для входящих сообщений.
void TransformMessageText(
	not_null<const HistoryItem*> item,
	TextWithEntities &text);

} // namespace Mokagram::Plugins
