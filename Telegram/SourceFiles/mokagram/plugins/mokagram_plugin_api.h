// Mokagram: SDK плагинов на стороне клиента (мост к Qt) и входы хуков обновлений.
#pragma once

#include "mtproto/sender.h"

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

} // namespace Mokagram::Plugins
