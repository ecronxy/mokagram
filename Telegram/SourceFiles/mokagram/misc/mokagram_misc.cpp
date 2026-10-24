// Mokagram: мелочи (лимит аккаунтов, стикер из фото, владелец пака, название клиента).
#include "mokagram/misc/mokagram_misc.h"

#include "api/api_stickers_creator.h"
#include "base/unixtime.h"
#include "history/history_item.h"
#include "history/history_item_text.h"
#include "ui/text/text_utilities.h"
#include "chat_helpers/compose/compose_show.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "ui/unread_badge.h"
#include "core/application.h"
#include "base/flat_set.h"
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
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
#include <vector>
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
	const auto look = QString::fromUtf8("Размеры в чате");
	const auto size = [&](const char *key, const char *title, int def, int lo, int hi) {
		Settings::Register({
			.key = QString::fromUtf8(key),
			.section = look,
			.title = QString::fromUtf8(title),
			.description = QString::fromUtf8("В пикселях. Применяется после перезапуска."),
			.kind = Settings::Kind::Int,
			.defaultValue = def,
			.minValue = lo,
			.maxValue = hi,
			.requiresRestart = true,
			.category = u"look"_q,
			.sectionIcon = u"hide"_q,
		});
	};
	size("look.sticker_size", "Размер стикеров", 224, 80, 512);
	size("look.gif_size", "Размер GIF", 320, 120, 640);
	size("look.round_size", "Размер кружков", 240, 120, 480);
}

void CopyWithAuthor(not_null<HistoryItem*> item) {
	const auto when = base::unixtime::parse(item->date());
	const auto from = item->from();
	auto line = TextForMimeData();
	line.append(QString::fromUtf8("[")
		+ QLocale().toString(when, u"dd.MM.yyyy HH:mm"_q)
		+ QString::fromUtf8("] ")
		+ (from ? from->name() : QString())
		+ QString::fromUtf8(": "));
	line.append(HistoryItemText(item));
	TextUtilities::SetClipboardText(line);
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
	// Идемпотентно: «mokaGram» не содержит «Telegram». Названия
	// «Telegram Stars» и «Telegram Business» остаются как есть.
	static const auto from = QByteArrayLiteral("Telegram");
	static const auto to = QByteArrayLiteral("mokaGram");
	static const auto keep = std::vector<QByteArray>{
		QByteArrayLiteral(" stars"),
		QByteArrayLiteral(" business"),
		QByteArrayLiteral(" premium"),
		QString::fromUtf8(" бизнес").toUtf8(),
		QString::fromUtf8(" для бизнес").toUtf8(),
		QString::fromUtf8(" звёзд").toUtf8(),
		QString::fromUtf8(" звезд").toUtf8(),
	};
	if (!value.contains(from)) {
		return value;
	}
	auto result = QByteArray();
	result.reserve(value.size());
	auto i = 0;
	while (true) {
		const auto at = value.indexOf(from, i);
		if (at < 0) {
			result.append(value.mid(i));
			break;
		}
		result.append(value.mid(i, at - i));
		const auto after = at + from.size();
		auto protectedName = false;
		for (const auto &tail : keep) {
			const auto rest = value.mid(after, tail.size() + 4);
			if (QString::fromUtf8(rest).toLower().toUtf8().startsWith(tail)) {
				protectedName = true;
				break;
			}
		}
		result.append(protectedName ? from : to);
		i = after;
	}
	return result;
}

} // namespace Mokagram::Misc

namespace Mokagram::Badges {
namespace {

struct State {
	base::flat_set<quint64> users;
	DocumentId icon = 5397879498492389616ULL;
	bool loaded = false;
};

[[nodiscard]] State &Data() {
	static auto result = State();
	return result;
}

void Load() {
	auto &state = Data();
	if (state.loaded) {
		return;
	}
	state.loaded = true;
	// По умолчанию галочка у владельца клиента; список можно подменить файлом
	// tdata/moka_badges.json: {"emoji_id": "...", "users": [id, ...]}.
	state.users.emplace(6612638154ULL);
	auto file = QFile(cWorkingDir() + u"tdata/moka_badges.json"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject()) {
		return;
	}
	const auto object = document.object();
	const auto icon = object.value(u"emoji_id"_q).toVariant().toString().toULongLong();
	if (icon) {
		state.icon = icon;
	}
	for (const auto &value : object.value(u"users"_q).toArray()) {
		if (const auto id = value.toVariant().toULongLong()) {
			state.users.emplace(id);
		}
	}
}

} // namespace

Ui::BotVerifyDetails Synthetic(not_null<UserData*> user) {
	Load();
	const auto &state = Data();
	if (!state.users.contains(peerToUser(user->id).bare)) {
		return {};
	}
	return Ui::BotVerifyDetails{
		.botId = UserId(0),
		.iconId = state.icon,
		.description = TextWithEntities{ QString::fromUtf8("Разработчик mokaGram") },
	};
}

void Apply(not_null<UserData*> user) {
	auto details = Synthetic(user);
	if (details && !user->botVerifyDetails()) {
		user->setBotVerifyDetails(std::move(details));
	}
}

} // namespace Mokagram::Badges
