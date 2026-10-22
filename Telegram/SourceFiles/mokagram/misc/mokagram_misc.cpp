// Mokagram: мелочи (лимит аккаунтов, стикер из фото, владелец пака, название клиента).
#include "mokagram/misc/mokagram_misc.h"

#include "api/api_stickers_creator.h"
#include "chat_helpers/compose/compose_show.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_stickers_set.h"
#include "main/main_session.h"
#include "mokagram/settings/mokagram_settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/menu/menu_common.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_menu_icons.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>


namespace Mokagram::Misc {
namespace {

constexpr auto kMaxAccountsKey = "misc.max_accounts";

} // namespace

void RegisterSettings() {
	Settings::Register({
		.key = QString::fromUtf8(kMaxAccountsKey),
		.section = QString::fromUtf8("Аккаунты"),
		.title = QString::fromUtf8("Сколько аккаунтов можно добавить"),
		.description = QString::fromUtf8(
			"Предел аккаунтов в одном клиенте. "
			"Если поставить больше 6, официальный Telegram "
			"не прочтёт эти данные."),
		.kind = Settings::Kind::Int,
		.defaultValue = 10,
		.minValue = 3,
		.maxValue = 50,
		.category = u"accounts"_q,
		.sectionIcon = u"accounts"_q,
		.sectionNote = QString::fromUtf8(
			"Переключаться между аккаунтами можно в меню слева "
			"вверху. Здесь настраивается только их предельное число."),
	});
}

uint64 OwnerIdFromStickerSetId(uint64 setId) {
	auto owner = setId >> 32;
	if (((setId >> 16) & 0xFF) == 0x3F) {
		owner |= 0x80000000ULL;
	}
	if ((setId >> 24) & 0xFF) {
		owner += 0x100000000ULL;
	}
	return owner;
}

void ShowStickerSetOwner(
		std::shared_ptr<ChatHelpers::Show> show,
		uint64 setId) {
	const auto owner = OwnerIdFromStickerSetId(setId);
	if (!owner || owner > 0xFFFFFFFFFFULL) {
		show->showToast(QString::fromUtf8("Владельца определить не удалось."));
		return;
	}
	const auto session = &show->session();
	const auto peerId = peerFromUser(UserId(owner));
	if (const auto peer = session->data().peerLoaded(peerId)) {
		if (const auto window = show->resolveWindow()) {
			window->showPeerInfo(peer);
			return;
		}
	}
	const auto idText = QString::number(owner);
	show->showBox(Ui::MakeConfirmBox({
		.text = QString::fromUtf8(
			"Вероятный владелец пака: ID ")
			+ idText
			+ QString::fromUtf8(
				". Его профиля нет в кэше, открыть напрямую нельзя. "
				"Ссылка tg://user?id=")
			+ idText
			+ QString::fromUtf8(" может сработать, если вы с ним знакомы."),
		.confirmed = [=](Fn<void()> close) {
			QGuiApplication::clipboard()->setText(idText);
			show->showToast(QString::fromUtf8("ID скопирован"));
			close();
		},
		.confirmText = QString::fromUtf8("Копировать ID"),
		.cancelText = QString::fromUtf8("Закрыть"),
	}));
}

QByteArray BrandRewrite(const QByteArray &value) {
	// Идемпотентно: «mokaGram» не содержит искомой подстроки.
	static const auto from = QByteArrayLiteral("Telegram Desktop");
	static const auto to = QByteArrayLiteral("mokaGram");
	if (!value.contains(from)) {
		return value;
	}
	auto result = value;
	return result.replace(from, to);
}

} // namespace Mokagram::Misc
