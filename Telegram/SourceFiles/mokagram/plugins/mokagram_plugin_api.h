// Mokagram: SDK плагинов на стороне клиента (мост к Qt) и входы хуков обновлений.
#pragma once

#include "mtproto/sender.h"
#include "ui/text/text_entity.h"

class HistoryItem;
class PeerData;
class UserData;

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

// Открыт чат (событие chat_opened для плагинов).
void EmitChatOpened(PeerData *peer);

// Кто-то в чате набирает текст, записывает голосовое и т.п.
void EmitTyping(PeerData *peer, UserData *user, mtpTypeId type);

// Короткий бейдж плагина в подвале пузыря рядом со временем (message_badge_hook). Пусто, если нет.
[[nodiscard]] QString MessageBadge(not_null<const HistoryItem*> item);

} // namespace Mokagram::Plugins
