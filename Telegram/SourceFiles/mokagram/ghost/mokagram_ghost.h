// Mokagram: режим призрака и скрытие сообщений заблокированных.
// Флаги лежат в реестре настроек (ключи "ghost.*" и "filters.hide_blocked"),
// здесь они кэшируются в обычных bool, горячий путь не ищет по реестру.
// Settings::Load() должен быть вызван до первого запроса к модулю.
#pragma once

#include "base/basic_types.h"

class HistoryItem;

namespace Mokagram::Ghost {

// Регистрирует записи разделов «Призрак» и «Фильтры» (hide_blocked)
// и подписывается на их изменение (одноразовый offline при включении).
void RegisterSettings();

// Слать ли «прочитано» (чаты, обсуждения, содержимое голосовых и кружков).
[[nodiscard]] bool ShouldSendRead();
// Слать ли «в сети» (account.updateStatus offline=false).
[[nodiscard]] bool ShouldSendOnline();
// Слать ли «печатает» и прочие send-action (кроме Speaking).
[[nodiscard]] bool ShouldSendTyping();
// Слать ли просмотры историй.
[[nodiscard]] bool ShouldSendStoryView();

// Включено ли скрытие сообщений заблокированных.
[[nodiscard]] bool HideBlockedEnabled();
// Скрывать ли сообщение: O(1), только по уже загруженным данным.
[[nodiscard]] bool HidesBlocked(not_null<const HistoryItem*> item);

} // namespace Mokagram::Ghost
