// Mokagram: мелочи (лимит аккаунтов, стикер из фото, владелец пака, название клиента).
#pragma once

#include "base/basic_types.h"

#include <QtCore/QByteArray>
#include <memory>
#include <vector>
#include <QtCore/QString>

class PhotoData;
class PeerData;
class HistoryItem;

namespace ChatHelpers {
class Show;
} // namespace ChatHelpers

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Mokagram::Misc {

// Регистрирует настройки раздела «Аккаунты» (misc.max_accounts).
void RegisterSettings();

// Кладёт в буфер «[дата время] автор: текст» для сообщения.
void CopyWithAuthor(not_null<HistoryItem*> item);

// Вероятный id владельца набора по id набора. Это эвристика, а не контракт Telegram.
[[nodiscard]] uint64 OwnerIdFromStickerSetId(uint64 setId);

// Открывает профиль вероятного владельца, а если его нет в кэше, показывает окно с ID.
void ShowStickerSetOwner(
	std::shared_ptr<ChatHelpers::Show> show,
	uint64 setId);

// Заменяет «Telegram Desktop» на «mokaGram» в значении строки языка.
// Повторный вызов безопасен (замена идемпотентна).
[[nodiscard]] QByteArray BrandRewrite(const QByteArray &value);

} // namespace Mokagram::Misc

namespace Mokagram::Bot {

// Ответ бота на inline-запрос: пары (id результата, заголовок).
using Results = std::vector<std::pair<QString, QString>>;

// Спрашивает @mokagent inline-запросом (без сообщений в чатах, нужна только сессия).
// fail вызывается, если нет сессии, бот не нашёлся или запрос не прошёл.
void Ask(const QString &query, Fn<void(Results)> done, Fn<void()> fail = nullptr);

} // namespace Mokagram::Bot

namespace Mokagram::Guard {

// Человек из списка исключений: проверка угроз на него не действует.
[[nodiscard]] bool ThreatExempt(not_null<PeerData*> peer);
void ToggleThreatExempt(not_null<PeerData*> peer);

} // namespace Mokagram::Guard
