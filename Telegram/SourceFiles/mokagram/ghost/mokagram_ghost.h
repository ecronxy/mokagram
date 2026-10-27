// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 6a1c4279
// Mokagram: режим призрака и скрытие сообщений заблокированных.
// Флаги лежат в реестре настроек (ключи "ghost.*" и "filters.hide_blocked"),
// здесь они кэшируются в обычных bool, горячий путь не ищет по реестру.
// Settings::Load() должен быть вызван до первого запроса к модулю.
#pragma once

#include "base/basic_types.h"
#include "base/basic_types.h"

class HistoryItem;
class History;
class PeerData;

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

// Слать ли «прочитано» в этом чате: false только если Призрак скрывает
// прочтение и чат не в списке исключений.
[[nodiscard]] bool ReadAllowedFor(not_null<::History*> history);
// Исключения: в этих чатах Призрак не мешает отмечать прочитанным.
[[nodiscard]] bool IsReadException(not_null<PeerData*> peer);
void ToggleReadException(not_null<PeerData*> peer);
// Явное «прочитать»: отправляет серверу прочтение чата, даже при Призраке.
void ReadNow(not_null<::History*> history);
// Скрывает ли сейчас Призрак прочтение (нужно ли показывать «прочитать»).
[[nodiscard]] bool HidesRead();

// Включено ли скрытие сообщений заблокированных.
[[nodiscard]] bool HideBlockedEnabled();
// Скрывать ли сообщение: O(1), только по уже загруженным данным.
[[nodiscard]] bool HidesBlocked(not_null<const HistoryItem*> item);

} // namespace Mokagram::Ghost
