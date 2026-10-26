// Mokagram: мелочи (лимит аккаунтов, стикер из фото, владелец пака, название клиента).
#include "mokagram/misc/mokagram_misc.h"
#include "mokagram/lang/mokagram_lang.h"
#include "lang/lang_keys.h"
#include "mokagram/misc/mokagram_datereg.h"
#include "mokagram/misc/mokagram_ping.h"
#include "mokagram/stars/mokagram_stars_stats.h"

#include "api/api_stickers_creator.h"
#include "base/unixtime.h"
#include "history/history_item.h"
#include "history/history_item_text.h"
#include "ui/text/text_utilities.h"
#include "chat_helpers/compose/compose_show.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "history/history.h"
#include "api/api_blocked_peers.h"
#include "api/api_user_privacy.h"
#include "apiwrap.h"
#include "main/main_domain.h"
#include "ui/power_saving.h"
#include "storage/cache/storage_cache_database.h"
#include "base/call_delayed.h"
#include <deque>
#include <QtGui/QGuiApplication>
#include "ui/unread_badge.h"
#include "core/application.h"
#include "base/flat_set.h"
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_stickers_set.h"
#include "main/main_session.h"
#include "main/main_account.h"
#include "mokagram/settings/mokagram_settings.h"
#include "mokagram/settings/mokagram_settings_box.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/menu/menu_common.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_menu_icons.h"

#include <QtGui/QGuiApplication>
#include <vector>
#include <QtGui/QClipboard>


namespace Mokagram::Guard {
void RegisterSettings();
} // namespace Mokagram::Guard

namespace Mokagram::Misc {
namespace {

constexpr auto kMaxAccountsKey = "misc.max_accounts";

} // namespace

void RegisterSettings() {
	Settings::Register({
		.key = u"misc.welcome_shown"_q,
		.title = u"welcome"_q,
		.kind = Settings::Kind::Bool,
		.defaultValue = false,
		.hidden = true,
	});
	Settings::Register({
		.key = QString::fromUtf8(kMaxAccountsKey),
		.section = QString::fromUtf8(Mokagram::Lang::Tr("Аккаунты")),
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Сколько аккаунтов можно добавить")),
		.description = QString::fromUtf8(
			Mokagram::Lang::Tr("Предел аккаунтов в одном клиенте. Если поставить больше 6, официальный Telegram не прочтёт эти данные.")),
		.kind = Settings::Kind::Int,
		.defaultValue = 10,
		.minValue = 3,
		.maxValue = 50,
		.category = u"accounts"_q,
		.sectionIcon = u"accounts"_q,
		.sectionNote = QString::fromUtf8(
			Mokagram::Lang::Tr("Переключаться между аккаунтами можно в меню слева вверху. Здесь настраивается только их предельное число.")),
	});
	Settings::Register({
		.key = u"chats.font"_q,
		.section = QString::fromUtf8(Mokagram::Lang::Tr("Шрифт сообщений")),
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Каким шрифтом отправлять")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr(
			"Всё, что вы пишете, уходит выбранным начертанием: напишете «привет», уйдёт жирным.")),
		.kind = Settings::Kind::Choice,
		.defaultValue = 0,
		.choices = {
			QString::fromUtf8(Mokagram::Lang::Tr("Обычный")),
			QString::fromUtf8(Mokagram::Lang::Tr("Жирный")),
			QString::fromUtf8(Mokagram::Lang::Tr("Курсив")),
			QString::fromUtf8(Mokagram::Lang::Tr("Жирный курсив")),
			QString::fromUtf8(Mokagram::Lang::Tr("Подчёркнутый")),
			QString::fromUtf8(Mokagram::Lang::Tr("Зачёркнутый")),
			QString::fromUtf8(Mokagram::Lang::Tr("Моноширинный")),
			QString::fromUtf8(Mokagram::Lang::Tr("Скрытый (спойлер)")),
			QString::fromUtf8(Mokagram::Lang::Tr("Цитата")),
			QString::fromUtf8("𝐁𝐨𝐥𝐝 (жирный)"),
			QString::fromUtf8("𝓢𝓬𝓻𝓲𝓹𝓽 (рукописный)"),
			QString::fromUtf8("𝔊𝔬𝔱𝔥𝔦𝔠 (готический)"),
			QString::fromUtf8("Ⓒⓘⓡⓒⓛⓔⓢ (в кружках)"),
			QString::fromUtf8("Ｗｉｄｅ (широкий)"),
			QString::fromUtf8("ꜱᴍᴀʟʟ ᴄᴀᴘꜱ (малые заглавные)"),
		},
		.category = u"chats"_q,
		.sectionIcon = u"edit"_q,
		.sectionNote = QString::fromUtf8(Mokagram::Lang::Tr(
			"Начертания с 𝐁𝐨𝐥𝐝 и ниже меняют только латиницу, кириллица остаётся обычной. Команды плагинов, ссылки, @имена и текст, где вы сами выделили жирный или курсив, не меняются.")),
	});
	const auto look = QString::fromUtf8(Mokagram::Lang::Tr("Размеры в чате"));
	const auto size = [&](const char *key, const char *title, int def, int lo, int hi) {
		Settings::Register({
			.key = QString::fromUtf8(key),
			.section = look,
			.title = QString::fromUtf8(title),
			.description = QString::fromUtf8(Mokagram::Lang::Tr("В пикселях. Применяется после перезапуска.")),
			.kind = Settings::Kind::Int,
			.defaultValue = def,
			.minValue = lo,
			.maxValue = hi,
			.requiresRestart = true,
			.category = u"look"_q,
			.sectionIcon = u"hide"_q,
		});
	};
	size("look.sticker_size", Mokagram::Lang::Tr("Размер стикеров"), 224, 80, 512);
	size("look.gif_size", Mokagram::Lang::Tr("Размер GIF"), 320, 120, 640);
	size("look.round_size", Mokagram::Lang::Tr("Размер кружков"), 240, 120, 480);
	Settings::Register({
		.key = u"misc.ping_dc"_q,
		.section = QString::fromUtf8(Mokagram::Lang::Tr("Сеть")),
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Пинг дата-центров")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Показывает, за сколько отвечает каждый дата-центр Telegram.")),
		.kind = Settings::Kind::String,
		.defaultValue = QString::fromUtf8(Mokagram::Lang::Tr("Проверить")),
		.category = u"other"_q,
		.sectionIcon = u"storage"_q,
		.action = [] { ShowPingBox(); },
	});
	Settings::Register({
		.key = u"plugins.prefix"_q,
		.section = QString::fromUtf8(Mokagram::Lang::Tr("Команды плагинов")),
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Префикс команд")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("С чего начинаются команды плагинов в поле ввода: например .help или !help. Один или два знака.")),
		.kind = Settings::Kind::String,
		.defaultValue = QString::fromUtf8("."),
		.category = u"plugins"_q,
	});
	Settings::Register({
		.key = u"plugins.autoupdate"_q,
		.section = QString::fromUtf8(Mokagram::Lang::Tr("Команды плагинов")),
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Обновлять плагины из магазина")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Плагины из магазина обновляются сами раз в 12 часов, если новая версия не просит новых прав. Если просит, придёт подсказка открыть магазин.")),
		.kind = Settings::Kind::Bool,
		.defaultValue = true,
		.category = u"plugins"_q,
	});
	Settings::Register({
		.key = u"plugins.dev_reload"_q,
		.section = QString::fromUtf8(Mokagram::Lang::Tr("Команды плагинов")),
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Режим разработчика")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Плагин перезагружается сам, когда вы сохраняете его файл, без подтверждения прав. Включайте только для плагинов, которые пишете сами.")),
		.kind = Settings::Kind::Bool,
		.defaultValue = false,
		.category = u"plugins"_q,
	});
	Settings::Register({
		.key = u"misc.stars_stats"_q,
		.section = QString::fromUtf8(Mokagram::Lang::Tr("Профиль")),
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Траты звёзд")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Сколько звёзд потрачено: по месяцам, категориям и получателям.")),
		.kind = Settings::Kind::String,
		.defaultValue = QString::fromUtf8(Mokagram::Lang::Tr("Открыть")),
		.category = u"other"_q,
		.sectionIcon = u"person"_q,
		.action = [] { Stars::ShowSpendingStatsBox(); },
	});
	Guard::RegisterSettings();
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
		show->showToast(QString::fromUtf8(Mokagram::Lang::Tr("Владельца определить не удалось.")));
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
			Mokagram::Lang::Tr("Вероятный владелец пака: ID "))
			+ idText
			+ QString::fromUtf8(
				Mokagram::Lang::Tr(". Его профиля нет в кэше, открыть напрямую нельзя. Ссылка tg://user?id="))
			+ idText
			+ QString::fromUtf8(Mokagram::Lang::Tr(" может сработать, если вы с ним знакомы.")),
		.confirmed = [=](Fn<void()> close) {
			QGuiApplication::clipboard()->setText(idText);
			show->showToast(QString::fromUtf8(Mokagram::Lang::Tr("ID скопирован")));
			close();
		},
		.confirmText = QString::fromUtf8(Mokagram::Lang::Tr("Копировать ID")),
		.cancelText = QString::fromUtf8(Mokagram::Lang::Tr("Закрыть")),
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
		QString::fromUtf8(Mokagram::Lang::Tr(" бизнес")).toUtf8(),
		QString::fromUtf8(Mokagram::Lang::Tr(" для бизнес")).toUtf8(),
		QString::fromUtf8(Mokagram::Lang::Tr(" звёзд")).toUtf8(),
		QString::fromUtf8(Mokagram::Lang::Tr(" звезд")).toUtf8(),
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
	base::flat_set<quint64> remote;
	DocumentId icon = 5402452264273094522ULL;
	bool loaded = false;
};

// Список выданных галочек лежит в репозитории, ветка badges, файл badges.json;
// его обновляет бот командой /publish.
constexpr const char *kBadgesUrls[] = {
	"http://93.123.84.219:8088/badges.json",
	"https://raw.githubusercontent.com/ecronxy/mokagram/badges/badges.json",
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

namespace {

// Команда клиента по юзернейму (в нижнем регистре) и подпись под профилем.
// Подпись на русском и на английском: язык берётся из языка интерфейса.
struct TeamEntry {
	const char *name;
	const char *ru;
	const char *en;
	bool blue;
};
constexpr TeamEntry kTeam[] = {
	{ "litteraliy", "Разработчик mokaGram", "Maintainer mokaGram", false },
	{ "xdshka", "Разработчик mokaGram", "Maintainer mokaGram", true },
};
constexpr auto kOwnerId = 6612638154ULL;

[[nodiscard]] bool Russian() {
	return ::Lang::Id().startsWith(u"ru"_q);
}

constexpr auto kBlueIcon = 5397879498492389616ULL;

[[nodiscard]] bool BlueIcon(not_null<UserData*> user) {
	if (peerToUser(user->id).bare == kOwnerId) {
		return true;
	}
	for (const auto &username : user->usernames()) {
		const auto lower = username.toLower().toUtf8();
		for (const auto &entry : kTeam) {
			if (entry.blue && lower == entry.name) {
				return true;
			}
		}
	}
	return false;
}

[[nodiscard]] const char *TeamRole(not_null<UserData*> user) {
	if (peerToUser(user->id).bare == kOwnerId) {
		return Russian()
			? Mokagram::Lang::Tr("Разработчик и владелец mokaGram")
			: "Developer and owner of mokaGram";
	}
	for (const auto &username : user->usernames()) {
		const auto lower = username.toLower().toUtf8();
		for (const auto &entry : kTeam) {
			if (lower == entry.name) {
				return Russian() ? entry.ru : entry.en;
			}
		}
	}
	return nullptr;
}

} // namespace

bool IsOwner(UserData *user) {
	return user && (peerToUser(user->id).bare == kOwnerId);
}

Ui::BotVerifyDetails Synthetic(not_null<UserData*> user) {
	Load();
	const auto &state = Data();
	const auto id = peerToUser(user->id).bare;
	const auto role = TeamRole(user);
	if (!role && !state.users.contains(id) && !state.remote.contains(id)) {
		return {};
	}
	return Ui::BotVerifyDetails{
		.botId = UserId(0),
		.iconId = BlueIcon(user) ? kBlueIcon : state.icon,
		.description = TextWithEntities{
			QString::fromUtf8(role ? role : (state.remote.contains(id) ? Mokagram::Lang::Tr("Поддерживает mokaGram") : Mokagram::Lang::Tr("Разработчик mokaGram"))),
		},
	};
}

// Список выданных галочек пришёл (из бота или из файла): обновляем значки у уже загруженных людей.
void ApplyRemote(const std::vector<quint64> &ids, quint64 icon) {
	auto &state = Data();
	const auto old = base::take(state.remote);
	if (icon) {
		state.icon = icon;
	}
	for (const auto id : ids) {
		state.remote.emplace(id);
	}
	auto all = old;
	for (const auto id : state.remote) {
		all.emplace(id);
	}
	const auto account = Core::App().domain().maybeLastOrSomeAuthedAccount();
	if (const auto session = account ? account->maybeSession() : nullptr) {
		for (const auto id : all) {
			if (const auto user = session->data().userLoaded(UserId(id))) {
				user->setBotVerifyDetails(Synthetic(user));
			}
		}
	}
}

// Основной путь: inline-запрос «list» к @mokagent (без сообщений в чатах, нужна только сессия).
void RefreshInline(Fn<void()> fail) {
	constexpr auto kBotId = 8687512401LL;
	const auto account = Core::App().domain().maybeLastOrSomeAuthedAccount();
	const auto session = account ? account->maybeSession() : nullptr;
	if (!session) {
		fail();
		return;
	}
	const auto weak = base::make_weak(session);
	const auto ask = [=](not_null<UserData*> bot) {
		session->api().request(MTPmessages_GetInlineBotResults(
			MTP_flags(0),
			bot->inputUser(),
			MTP_inputPeerSelf(),
			MTPInputGeoPoint(),
			MTP_string(u"list"_q),
			MTP_string()
		)).done([=](const MTPmessages_BotResults &result) {
			auto ids = std::vector<quint64>();
			auto icon = quint64();
			result.match([&](const MTPDmessages_botResults &data) {
				for (const auto &item : data.vresults().v) {
					item.match([&](const MTPDbotInlineResult &r) {
						const auto id = qs(r.vid());
						const auto title = r.vtitle() ? qs(*r.vtitle()) : QString();
						if (id == u"e"_q) {
							icon = title.toULongLong();
						} else if (id.startsWith('l')) {
							for (const auto &part : title.split(',', Qt::SkipEmptyParts)) {
								if (const auto value = part.toULongLong()) {
									ids.push_back(value);
								}
							}
						}
					}, [](const auto &) {});
				}
			});
			ApplyRemote(ids, icon);
		}).fail([=] {
			fail();
		}).send();
	};
	if (const auto bot = session->data().userLoaded(UserId(kBotId))) {
		ask(bot);
		return;
	}
	session->api().request(MTPcontacts_Search(
		MTP_flags(0),
		MTP_string(u"mokagent"_q),
		MTP_int(5)
	)).done([=](const MTPcontacts_Found &result) {
		result.match([&](const MTPDcontacts_found &data) {
			session->data().processUsers(data.vusers());
		});
		if (const auto bot = session->data().userLoaded(UserId(kBotId))) {
			ask(bot);
		} else {
			fail();
		}
	}).fail([=] {
		fail();
	}).send();
}

void Refresh(int index = 0) {
	Load();
	if (index == 0) {
		RefreshInline([] { Refresh(1); });
		return;
	}
	index -= 1;
	if (index >= int(std::size(kBadgesUrls))) {
		return;
	}
	static auto manager = new QNetworkAccessManager();
	auto request = QNetworkRequest(QUrl(QString::fromLatin1(kBadgesUrls[index])));
	request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
	request.setTransferTimeout(8000);
	const auto reply = manager->get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto document = (reply->error() == QNetworkReply::NoError)
			? QJsonDocument::fromJson(reply->readAll())
			: QJsonDocument();
		if (!document.isObject()) {
			// Сервер недоступен: пробуем следующий источник.
			Refresh(index + 2);
			return;
		}
		const auto object = document.object();
		auto ids = std::vector<quint64>();
		for (const auto &value : object.value(u"users"_q).toArray()) {
			if (const auto id = value.toVariant().toULongLong()) {
				ids.push_back(id);
			}
		}
		ApplyRemote(ids, object.value(u"emoji_id"_q).toVariant().toString().toULongLong());
	});
}

void Start() {
	Refresh(0);
	// При старте клиент ещё может быть не авторизован: пробуем ещё раз позже.
	for (const auto delay : { 20000, 90000 }) {
		QTimer::singleShot(delay, [] { Refresh(0); });
	}
	static auto timer = new QTimer();
	timer->setInterval(10 * 60 * 1000);
	QObject::connect(timer, &QTimer::timeout, [] { Refresh(0); });
	timer->start();
}

void Apply(not_null<UserData*> user) {
	auto details = Synthetic(user);
	if (details && !user->botVerifyDetails()) {
		user->setBotVerifyDetails(std::move(details));
	}
}

} // namespace Mokagram::Badges

namespace Mokagram::Guard {
namespace {

namespace S = ::Mokagram::Settings;

[[nodiscard]] QString SpamSection() {
	return QString::fromUtf8(Mokagram::Lang::Tr("Защита от спама"));
}
[[nodiscard]] QString LagSection() {
	return QString::fromUtf8(Mokagram::Lang::Tr("Анти-лаг"));
}

constexpr auto kDefaultThreats =
	"убью, прибью, найду тебя, найду и убью, сдохни, зарежу, взорву, "
	"выебу тебя, i will kill you, i'll kill you, kill yourself, kys";

struct StickerLog {
	std::deque<crl::time> times;
};

struct MassAddLog {
	std::deque<crl::time> times;
	base::flat_set<quint64> seen;
};

[[nodiscard]] bool IsStranger(not_null<UserData*> user) {
	return !user->isSelf()
		&& !user->isBot()
		&& !user->isContact()
		&& !user->isVerified()
		&& peerToUser(user->id).bare != 777000;
}

void Toast(const QString &text) {
	if (const auto window = Core::App().activePrimaryWindow()) {
		if (const auto controller = window->sessionController()) {
			controller->showToast(text);
		}
	}
}

void Block(not_null<UserData*> user, const QString &why) {
	user->session().api().blockedPeers().block(user);
	Toast(QString::fromUtf8(Mokagram::Lang::Tr("Заблокировал ")) + user->name() + QString::fromUtf8(": ") + why);
}

[[nodiscard]] QStringList ExemptList() {
	auto result = QStringList();
	for (auto part : S::String(u"guard.threat_exempt"_q).split(
			QRegularExpression(u"[,\\n;\\s]+"_q), Qt::SkipEmptyParts)) {
		result.push_back(part.trimmed().toLower().remove(u'@'));
	}
	return result;
}

[[nodiscard]] QStringList Threats() {
	auto result = QStringList();
	const auto raw = S::String(u"guard.threat_words"_q);
	for (auto part : raw.split(QRegularExpression(u"[,\\n;]"_q), Qt::SkipEmptyParts)) {
		part = part.trimmed().toLower();
		if (!part.isEmpty()) {
			result.push_back(part);
		}
	}
	return result;
}

void OnNewMessage(not_null<HistoryItem*> item) {
	if (item->out() || item->isService() || item->isEmpty()) {
		return;
	}
	const auto user = item->history()->peer->asUser();
	if (!user || user->isSelf() || user->isBot() || user->isVerified()) {
		return;
	}
	const auto strangersOnly = S::Bool(u"guard.only_strangers"_q);
	if (strangersOnly && !IsStranger(user)) {
		return;
	}
	if (S::Bool(u"guard.stickers"_q)) {
		const auto media = item->media();
		const auto document = media ? media->document() : nullptr;
		if (document && document->sticker()) {
			static auto logs = base::flat_map<quint64, StickerLog>();
			auto &log = logs[peerToUser(user->id).bare];
			const auto now = crl::now();
			const auto window = crl::time(S::Int(u"guard.sticker_seconds"_q)) * 1000;
			log.times.push_back(now);
			while (!log.times.empty() && now - log.times.front() > window) {
				log.times.pop_front();
			}
			if (int(log.times.size()) >= S::Int(u"guard.sticker_count"_q)) {
				logs.erase(peerToUser(user->id).bare);
				Block(user, QString::fromUtf8(Mokagram::Lang::Tr("спам стикерами")));
				return;
			}
		}
	}
	if (S::Bool(u"guard.threats"_q) && !ThreatExempt(user)) {
		const auto text = item->originalText().text.toLower();
		if (!text.isEmpty()) {
			for (const auto &word : Threats()) {
				if (text.contains(word)) {
					Block(user, QString::fromUtf8(Mokagram::Lang::Tr("угрозы")));
					return;
				}
			}
		}
	}
}

void WatchMassAdd(not_null<Main::Session*> session) {
	static const auto started = crl::now();
	static auto log = MassAddLog();
	session->changes().peerUpdates(
		Data::PeerUpdate::Flag::ChannelAmIn
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		const auto channel = update.peer->asChannel();
		if (!channel || !channel->amIn() || channel->amCreator()) {
			return;
		}
		const auto id = peerToChannel(channel->id).bare;
		if (!log.seen.emplace(id).second) {
			return;
		}
		// Первые минуты после запуска чаты просто подгружаются.
		if (crl::now() - started < 90 * 1000 || !S::Bool(u"guard.mass_add"_q)) {
			return;
		}
		const auto channelId = channel->id;
		base::call_delayed(5000, session, [=] {
			const auto strong = session->data().channelLoaded(peerToChannel(channelId));
			if (!strong || !strong->amIn() || strong->inviter.bare <= 0) {
				return;
			}
			const auto inviter = session->data().userLoaded(strong->inviter);
			if (!inviter || !IsStranger(inviter)) {
				return;
			}
			const auto now = crl::now();
			const auto window = crl::time(S::Int(u"guard.mass_minutes"_q)) * 60 * 1000;
			log.times.push_back(now);
			while (!log.times.empty() && now - log.times.front() > window) {
				log.times.pop_front();
			}
			if (S::Bool(u"guard.mass_leave"_q)) {
				session->api().leaveChannel(strong);
			}
			if (int(log.times.size()) >= S::Int(u"guard.mass_count"_q)) {
				log.times.clear();
				auto rule = Api::UserPrivacy::Rule();
				rule.option = Api::UserPrivacy::Option::Contacts;
				session->api().userPrivacy().save(
					Api::UserPrivacy::Key::Invites,
					rule);
				Toast(QString::fromUtf8(Mokagram::Lang::Tr("Много добавлений в чаты подряд: теперь добавлять вас могут только контакты")));
			}
		});
	}, session->lifetime());
}

} // namespace

bool ThreatExempt(not_null<PeerData*> peer) {
	const auto list = ExemptList();
	if (list.contains(QString::number(peerToUser(peer->id).bare))) {
		return true;
	}
	if (const auto user = peer->asUser()) {
		const auto name = user->username().toLower();
		return !name.isEmpty() && list.contains(name);
	}
	return false;
}

void ToggleThreatExempt(not_null<PeerData*> peer) {
	auto list = ExemptList();
	const auto id = QString::number(peerToUser(peer->id).bare);
	if (ThreatExempt(peer)) {
		list.removeAll(id);
		if (const auto user = peer->asUser()) {
			list.removeAll(user->username().toLower());
		}
	} else {
		list.push_back(id);
	}
	S::SetString(u"guard.threat_exempt"_q, list.join(u", "_q));
}

void RegisterSettings() {
	const auto add = [&](S::Entry entry) {
		entry.section = SpamSection();
		entry.category = u"privacy"_q;
		entry.sectionIcon = u"block"_q;
		entry.sectionNote = QString::fromUtf8(
			Mokagram::Lang::Tr("Работает в личных сообщениях. Заблокированных можно вернуть в настройках Telegram, раздел «Заблокированные»."));
		S::Register(std::move(entry));
	};
	add({
		.key = u"guard.only_strangers"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Только для тех, кого нет в контактах")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Знакомых и ботов защита не трогает.")),
		.kind = S::Kind::Bool,
		.defaultValue = true,
	});
	add({
		.key = u"guard.stickers"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Блокировать за спам стикерами")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Если человек присылает слишком много стикеров подряд.")),
		.kind = S::Kind::Bool,
		.defaultValue = false,
	});
	add({
		.key = u"guard.sticker_count"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Сколько стикеров ведёт к блокировке")),
		.kind = S::Kind::Int,
		.defaultValue = 5,
		.minValue = 2,
		.maxValue = 50,
		.dependsOn = u"guard.stickers"_q,
	});
	add({
		.key = u"guard.sticker_seconds"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("За сколько секунд")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Окно, в котором считаются стикеры.")),
		.kind = S::Kind::Int,
		.defaultValue = 10,
		.minValue = 1,
		.maxValue = 120,
		.dependsOn = u"guard.stickers"_q,
	});
	add({
		.key = u"guard.threats"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Блокировать за угрозы")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Если в сообщении есть слово или фраза из списка ниже.")),
		.kind = S::Kind::Bool,
		.defaultValue = false,
	});
	add({
		.key = u"guard.threat_words"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Список угроз")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Через запятую, регистр не важен.")),
		.kind = S::Kind::String,
		.defaultValue = QString::fromUtf8(kDefaultThreats),
		.dependsOn = u"guard.threats"_q,
	});
	add({
		.key = u"guard.threat_exempt"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("На кого список угроз не действует")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Им можно писать что угодно. Добавляется в меню чата («Угрозы: не проверять»), здесь можно править: айди или @юзернеймы через запятую.")),
		.kind = S::Kind::String,
		.defaultValue = QString(),
		.dependsOn = u"guard.threats"_q,
	});
	add({
		.key = u"guard.mass_add"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Защита от массового добавления в чаты")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Если вас подряд добавляют в чаты незнакомые, добавлять смогут только контакты.")),
		.kind = S::Kind::Bool,
		.defaultValue = false,
	});
	add({
		.key = u"guard.mass_count"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Сколько добавлений считать массовыми")),
		.kind = S::Kind::Int,
		.defaultValue = 3,
		.minValue = 2,
		.maxValue = 20,
		.dependsOn = u"guard.mass_add"_q,
	});
	add({
		.key = u"guard.mass_minutes"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("За сколько минут")),
		.kind = S::Kind::Int,
		.defaultValue = 10,
		.minValue = 1,
		.maxValue = 180,
		.dependsOn = u"guard.mass_add"_q,
	});
	add({
		.key = u"guard.mass_leave"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Сразу выходить из таких чатов")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Только там, куда вас добавил не контакт.")),
		.kind = S::Kind::Bool,
		.defaultValue = false,
		.dependsOn = u"guard.mass_add"_q,
	});

	const auto lag = [&](S::Entry entry) {
		entry.section = LagSection();
		entry.category = u"look"_q;
		entry.sectionIcon = u"hide"_q;
		entry.sectionNote = QString::fromUtf8(
			Mokagram::Lang::Tr("Клиент следит за подвисаниями интерфейса. Если их много, он на время отключает тяжёлые анимации, а когда всё спокойно, возвращает как было."));
		S::Register(std::move(entry));
	};
	lag({
		.key = u"lag.enabled"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Анти-лаг")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Автоматически включает экономный режим при подвисаниях.")),
		.kind = S::Kind::Bool,
		.defaultValue = true,
	});
	lag({
		.key = u"lag.idle"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Экономия в фоне")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Когда окно не в фокусе, анимации, эмодзи и эффекты приостанавливаются. Сообщения, плагины и уведомления работают как обычно.")),
		.kind = S::Kind::Bool,
		.defaultValue = true,
	});
	lag({
		.key = u"lag.emoji_cap"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Сколько эмодзи играет одновременно")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Остальные премиум-эмодзи замирают на кадре. 0 значит без ограничения.")),
		.kind = S::Kind::Int,
		.defaultValue = 24,
		.minValue = 0,
		.maxValue = 200,
	});
	lag({
		.key = u"lag.sticker_cap"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Сколько стикеров играет одновременно")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Остальные анимированные стикеры замирают на кадре. 0 значит без ограничения.")),
		.kind = S::Kind::Int,
		.defaultValue = 6,
		.minValue = 0,
		.maxValue = 50,
	});
	S::Register({
		.key = u"lag.autostart"_q,
		.section = QString::fromUtf8(Mokagram::Lang::Tr("Запуск и фон")),
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Запускать вместе с системой")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Нужно, чтобы расписания и команды плагинов работали сразу после включения компьютера.")),
		.kind = S::Kind::Bool,
		.defaultValue = false,
		.category = u"look"_q,
		.sectionIcon = u"storage"_q,
	});
	S::Register({
		.key = u"lag.background"_q,
		.section = QString::fromUtf8(Mokagram::Lang::Tr("Запуск и фон")),
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Крестик убирает окно в фон")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Клиент продолжает работать со свёрнутым окном: плагины, расписания и уведомления не останавливаются. Если выключить, крестик закрывает клиент.")),
		.kind = S::Kind::Bool,
		.defaultValue = true,
		.category = u"look"_q,
		.sectionIcon = u"storage"_q,
	});
	lag({
		.key = u"lag.toaster"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Режим тостера")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Максимальная экономия всегда: без анимаций и эффектов. Для слабых компьютеров и ноутбуков на батарее.")),
		.kind = S::Kind::Bool,
		.defaultValue = false,
	});
	lag({
		.key = u"lag.level"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Что отключать при лагах")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Мягко: фон чата, эффекты, спойлеры. Средне: ещё стикеры и эмодзи. Жёстко: всё.")),
		.kind = S::Kind::Choice,
		.defaultValue = 1,
		.choices = {
			QString::fromUtf8(Mokagram::Lang::Tr("Мягко")),
			QString::fromUtf8(Mokagram::Lang::Tr("Средне")),
			QString::fromUtf8(Mokagram::Lang::Tr("Жёстко")),
		},
		.dependsOn = u"lag.enabled"_q,
	});
	lag({
		.key = u"lag.sensitivity"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Сколько подвисаний за 10 секунд считать лагом")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Меньше число: реагирует раньше.")),
		.kind = S::Kind::Int,
		.defaultValue = 8,
		.minValue = 3,
		.maxValue = 40,
		.dependsOn = u"lag.enabled"_q,
	});
	lag({
		.key = u"lag.calm"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Через сколько секунд тишины вернуть всё")),
		.kind = S::Kind::Int,
		.defaultValue = 45,
		.minValue = 10,
		.maxValue = 600,
		.dependsOn = u"lag.enabled"_q,
	});
	lag({
		.key = u"lag.cache"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Чистить кэш при лагах")),
		.description = QString::fromUtf8(Mokagram::Lang::Tr("Только если он больше предела ниже. Не чаще раза в полчаса.")),
		.kind = S::Kind::Bool,
		.defaultValue = false,
		.dependsOn = u"lag.enabled"_q,
	});
	lag({
		.key = u"lag.cache_mb"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Предел кэша, МБ")),
		.kind = S::Kind::Int,
		.defaultValue = 2048,
		.minValue = 256,
		.maxValue = 20480,
		.dependsOn = u"lag.cache"_q,
	});
	lag({
		.key = u"lag.notify"_q,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Говорить, когда режим включается")),
		.kind = S::Kind::Bool,
		.defaultValue = true,
		.dependsOn = u"lag.enabled"_q,
	});
}

namespace {

struct LagState {
	crl::time last = 0;
	std::deque<crl::time> stalls;
	bool active = false;
	PowerSaving::Flags saved;
	crl::time lastCacheClear = 0;
};

void LagTick(LagState &state) {
	const auto now = crl::now();
	const auto delta = (state.last > 0) ? (now - state.last - 200) : 0;
	state.last = now;
	if (!S::Bool(u"lag.enabled"_q)) {
		if (state.active) {
			PowerSaving::Set(state.saved);
			state.active = false;
		}
		state.stalls.clear();
		return;
	}
	// Сон, свёрнутое окно и скачки времени подвисаниями не считаем.
	const auto counted = QGuiApplication::applicationState() == Qt::ApplicationActive
		&& delta > 120 && delta < 3000;
	if (counted) {
		state.stalls.push_back(now);
	}
	while (!state.stalls.empty() && now - state.stalls.front() > 10000) {
		state.stalls.pop_front();
	}
	if (!state.active && int(state.stalls.size()) >= S::Int(u"lag.sensitivity"_q)) {
		const auto level = S::Int(u"lag.level"_q);
		auto flags = PowerSaving::Flags(0);
		using F = PowerSaving::Flag;
		flags |= F::kChatBackground | F::kChatSpoiler | F::kChatEffects | F::kEmojiStatus;
		if (level >= 1) {
			flags |= F::kStickersChat | F::kEmojiChat | F::kEmojiReactions
				| F::kStickersPanel | F::kEmojiPanel;
		}
		if (level >= 2) {
			flags = PowerSaving::Flags(PowerSaving::kAll);
		}
		state.saved = PowerSaving::Current();
		state.active = true;
		PowerSaving::Set(state.saved | flags);
		if (S::Bool(u"lag.notify"_q)) {
			Toast(QString::fromUtf8(Mokagram::Lang::Tr("Анти-лаг: включил экономный режим")));
		}
		if (S::Bool(u"lag.cache"_q)
			&& (now - state.lastCacheClear > 30 * 60 * 1000)) {
			if (const auto window = Core::App().activePrimaryWindow()) {
				if (const auto controller = window->sessionController()) {
					const auto session = &controller->session();
					const auto limit = int64(S::Int(u"lag.cache_mb"_q)) * 1024 * 1024;
					rpl::combine(
						session->data().cache().statsOnMain(),
						session->data().cacheBigFile().statsOnMain()
					) | rpl::take(1) | rpl::on_next([=](
							Storage::Cache::Database::Stats &&a,
							Storage::Cache::Database::Stats &&b) {
						if (a.full.totalSize + b.full.totalSize > limit) {
							session->data().cacheBigFile().clear();
							session->data().cache().clear();
						}
					}, session->lifetime());
					state.lastCacheClear = now;
				}
			}
		}
	} else if (state.active
		&& (state.stalls.empty()
			|| now - state.stalls.back() > crl::time(S::Int(u"lag.calm"_q)) * 1000)) {
		PowerSaving::Set(state.saved);
		state.active = false;
		state.stalls.clear();
	}
}

} // namespace

void Start() {
	// Лаг-монитор: опоздание таймера на главном потоке = подвисание интерфейса.
	static auto state = LagState();
	static auto timer = new QTimer();
	// Грубый таймер: Qt склеивает пробуждения, процессор спит дольше.
	timer->setTimerType(Qt::CoarseTimer);
	timer->setInterval(200);
	QObject::connect(timer, &QTimer::timeout, [] { LagTick(state); });
	timer->start();
	// Пока окно в фоне, подвисания никто не видит: таймер спит и не будит процессор.
	QObject::connect(qApp, &QGuiApplication::applicationStateChanged, [](
			Qt::ApplicationState application) {
		if (application == Qt::ApplicationActive) {
			if (!timer->isActive()) {
				// После сна не считаем паузу подвисанием.
				state.last = 0;
				timer->start();
			}
		} else {
			timer->stop();
		}
	});

	static auto lifetime = rpl::lifetime();
	Core::App().domain().activeSessionValue(
	) | rpl::filter([](Main::Session *session) {
		return session != nullptr;
	}) | rpl::on_next([](Main::Session *session) {
		session->changes().realtimeMessageUpdates(
			Data::MessageUpdate::Flag::NewAdded
		) | rpl::on_next([](const Data::MessageUpdate &update) {
			OnNewMessage(update.item);
		}, session->lifetime());
		WatchMassAdd(session);
		::Mokagram::Settings::PreloadRowIcons(session);
	}, lifetime);
}

} // namespace Mokagram::Guard
