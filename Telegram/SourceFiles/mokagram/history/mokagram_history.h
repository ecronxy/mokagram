// Mokagram: история сообщений, анти-удаление и фильтры (публичный интерфейс).
// Хранилище и логика здесь, в чужие файлы врезаются только одиночные вызовы.
#pragma once

#include "base/basic_types.h"
#include "mokagram/history/mokagram_history_filters.h"
#include "mokagram/history/mokagram_history_store.h"

#include "ui/click_handler.h"

#include <vector>

class History;
class HistoryItem;
class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {
class GenericBox;
} // namespace Ui

struct TextWithTags;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Mokagram::History {

// Раздел «История и фильтры» в реестре настроек и подписка фильтров на них.
void RegisterSettings();

// От сервера пришло удаление: вызывать до notifyItemsAboutToBeDestroyed.
// Снимок кладётся в фоновую очередь, поведение клиента не меняется.
void OnServerDeleted(const std::vector<not_null<HistoryItem*>> &items);

// Перед применением правки: снимает ТЕКУЩУЮ (старую) версию сообщения.
// newText и newEditDate берутся из пришедшего обновления, чтобы пропускать
// пустые «правки» (реакции, закрепы), у которых текст и дата не менялись.
void OnBeforeEdit(
	not_null<HistoryItem*> item,
	const QString &newText,
	TimeId newEditDate);

// Хранилище аккаунта (создаётся при первом обращении, лениво).
[[nodiscard]] Store &StoreFor(not_null<Main::Session*> session);

// Призраки удалённых сообщений: локальные HistoryItem (флаг Local, id вне
// серверного диапазона), вставляются в ленту по дате, на сервер не уходят.
[[nodiscard]] bool IsGhost(not_null<const HistoryItem*> item);
// Самоуничтожающиеся фото, видео и голосовые не прячутся и не пропадают
// после просмотра: ПК показывает их как обычные (history.keep_ttl).
[[nodiscard]] bool KeepSelfDestruct();
// Призрак нужно рисовать полупрозрачным (history.dim_deleted).
[[nodiscard]] bool GhostDimmed(not_null<const HistoryItem*> item);
// Чат открыт или подгрузил срез: подтянуть призраков и пометки правок.
// Дёшево и идемпотентно, можно звать на каждый скролл.
void OnChatShown(not_null<::History*> history);

// Есть ли у сообщения прежние версии (только при history.save_edited).
[[nodiscard]] bool HasRevisions(not_null<const HistoryItem*> item);
// Ссылка для пометки «изменено» (nullptr, если версий нет).
[[nodiscard]] ClickHandlerPtr RevisionsLink(not_null<HistoryItem*> item);
void ShowRevisions(
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);
// Нужен ли пункт «История удалённых и правок» в меню чата.
[[nodiscard]] bool ShowInPeerMenu();

// Экраны. kind: 1 = удалённые, 2 = правки.
void ShowHistoryBox(
	not_null<Window::SessionController*> controller,
	uint64 peerId,
	Kind kind);
// Прежние версии одного сообщения.
void ShowRevisionsBox(
	not_null<Window::SessionController*> controller,
	uint64 peerId,
	int64 messageId);
void ShowChatsBox(not_null<Window::SessionController*> controller);

// «Удалить все мои сообщения» в чате: подтверждение, поиск на сервере, удаление пачками.
// Для групп, супергрупп, личных чатов и «Избранного». Каналы не поддерживаются.
[[nodiscard]] bool CanDeleteMyMessages(not_null<PeerData*> peer);
void ShowDeleteMyMessagesForPeers(
	not_null<Window::SessionController*> controller,
	const std::vector<PeerId> &peers);

void ShowDeleteMyMessages(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);
// peerId = 0: фильтры создаются «везде», иначе только для этого чата.
void ShowFiltersBox(
	not_null<Window::SessionController*> controller,
	uint64 peerId = 0);

// Сериализованное медиа сообщения запоминаем, чтобы показать после удаления.
[[nodiscard]] bool WantsMediaTl();
void RememberMedia(FullMsgId id, QByteArray tl);

// Убирает призрак из ленты и из хранилища.
void RemoveGhost(not_null<HistoryItem*> item);

// Меню удалённого сообщения: ответить, переслать, копировать.
void FillGhostMenu(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	std::vector<not_null<HistoryItem*>> items,
	Fn<void(const TextWithTags &)> insertText);

} // namespace Mokagram::History
