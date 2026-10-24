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
	base::flat_set<quint64> remote;
	DocumentId icon = 5397879498492389616ULL;
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

Ui::BotVerifyDetails Synthetic(not_null<UserData*> user) {
	Load();
	const auto &state = Data();
	const auto id = peerToUser(user->id).bare;
	if (!state.users.contains(id) && !state.remote.contains(id)) {
		return {};
	}
	return Ui::BotVerifyDetails{
		.botId = UserId(0),
		.iconId = state.icon,
		.description = TextWithEntities{ QString::fromUtf8("Разработчик mokaGram") },
	};
}

void Refresh(int index = 0) {
	Load();
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
			Refresh(index + 1);
			return;
		}
		const auto object = document.object();
		auto &state = Data();
		const auto old = base::take(state.remote);
		const auto icon = object.value(u"emoji_id"_q).toVariant().toString().toULongLong();
		if (icon) {
			state.icon = icon;
		}
		for (const auto &value : object.value(u"users"_q).toArray()) {
			if (const auto id = value.toVariant().toULongLong()) {
				state.remote.emplace(id);
			}
		}
		// Пересчитываем значок у тех, кто уже загружен, и у тех, кого убрали.
		auto all = old;
		for (const auto id : state.remote) {
			all.emplace(id);
		}
		if (const auto session = Core::App().activeAccount().maybeSession()) {
			for (const auto id : all) {
				if (const auto user = session->data().userLoaded(UserId(id))) {
					user->setBotVerifyDetails(Synthetic(user));
				}
			}
		}
	});
}

void Start() {
	Refresh(0);
	static auto timer = new QTimer();
	timer->setInterval(30 * 60 * 1000);
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

const auto kSection = QString::fromUtf8("Защита от спама");
const auto kLagSection = QString::fromUtf8("Анти-лаг");

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
	Toast(QString::fromUtf8("Заблокировал ") + user->name() + QString::fromUtf8(": ") + why);
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
				Block(user, QString::fromUtf8("спам стикерами"));
				return;
			}
		}
	}
	if (S::Bool(u"guard.threats"_q)) {
		const auto text = item->originalText().text.toLower();
		if (!text.isEmpty()) {
			for (const auto &word : Threats()) {
				if (text.contains(word)) {
					Block(user, QString::fromUtf8("угрозы"));
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
				Toast(QString::fromUtf8("Много добавлений в чаты подряд: теперь добавлять вас могут только контакты"));
			}
		});
	}, session->lifetime());
}

} // namespace

void RegisterSettings() {
	const auto add = [&](S::Entry entry) {
		entry.section = kSection;
		entry.category = u"privacy"_q;
		entry.sectionIcon = u"block"_q;
		entry.sectionNote = QString::fromUtf8(
			"Работает в личных сообщениях. Заблокированных можно "
			"вернуть в настройках Telegram, раздел «Заблокированные».");
		S::Register(std::move(entry));
	};
	add({
		.key = u"guard.only_strangers"_q,
		.title = QString::fromUtf8("Только для тех, кого нет в контактах"),
		.description = QString::fromUtf8("Знакомых и ботов защита не трогает."),
		.kind = S::Kind::Bool,
		.defaultValue = true,
	});
	add({
		.key = u"guard.stickers"_q,
		.title = QString::fromUtf8("Блокировать за спам стикерами"),
		.description = QString::fromUtf8("Если человек присылает слишком много стикеров подряд."),
		.kind = S::Kind::Bool,
		.defaultValue = false,
	});
	add({
		.key = u"guard.sticker_count"_q,
		.title = QString::fromUtf8("Сколько стикеров ведёт к блокировке"),
		.kind = S::Kind::Int,
		.defaultValue = 5,
		.minValue = 2,
		.maxValue = 50,
		.dependsOn = u"guard.stickers"_q,
	});
	add({
		.key = u"guard.sticker_seconds"_q,
		.title = QString::fromUtf8("За сколько секунд"),
		.description = QString::fromUtf8("Окно, в котором считаются стикеры."),
		.kind = S::Kind::Int,
		.defaultValue = 10,
		.minValue = 1,
		.maxValue = 120,
		.dependsOn = u"guard.stickers"_q,
	});
	add({
		.key = u"guard.threats"_q,
		.title = QString::fromUtf8("Блокировать за угрозы"),
		.description = QString::fromUtf8("Если в сообщении есть слово или фраза из списка ниже."),
		.kind = S::Kind::Bool,
		.defaultValue = false,
	});
	add({
		.key = u"guard.threat_words"_q,
		.title = QString::fromUtf8("Список угроз"),
		.description = QString::fromUtf8("Через запятую, регистр не важен."),
		.kind = S::Kind::String,
		.defaultValue = QString::fromUtf8(kDefaultThreats),
		.dependsOn = u"guard.threats"_q,
	});
	add({
		.key = u"guard.mass_add"_q,
		.title = QString::fromUtf8("Защита от массового добавления в чаты"),
		.description = QString::fromUtf8("Если вас подряд добавляют в чаты незнакомые, добавлять смогут только контакты."),
		.kind = S::Kind::Bool,
		.defaultValue = false,
	});
	add({
		.key = u"guard.mass_count"_q,
		.title = QString::fromUtf8("Сколько добавлений считать массовыми"),
		.kind = S::Kind::Int,
		.defaultValue = 3,
		.minValue = 2,
		.maxValue = 20,
		.dependsOn = u"guard.mass_add"_q,
	});
	add({
		.key = u"guard.mass_minutes"_q,
		.title = QString::fromUtf8("За сколько минут"),
		.kind = S::Kind::Int,
		.defaultValue = 10,
		.minValue = 1,
		.maxValue = 180,
		.dependsOn = u"guard.mass_add"_q,
	});
	add({
		.key = u"guard.mass_leave"_q,
		.title = QString::fromUtf8("Сразу выходить из таких чатов"),
		.description = QString::fromUtf8("Только там, куда вас добавил не контакт."),
		.kind = S::Kind::Bool,
		.defaultValue = false,
		.dependsOn = u"guard.mass_add"_q,
	});

	const auto lag = [&](S::Entry entry) {
		entry.section = kLagSection;
		entry.category = u"look"_q;
		entry.sectionIcon = u"hide"_q;
		entry.sectionNote = QString::fromUtf8(
			"Клиент следит за подвисаниями интерфейса. Если их много, он на время "
			"отключает тяжёлые анимации, а когда всё спокойно, возвращает как было.");
		S::Register(std::move(entry));
	};
	lag({
		.key = u"lag.enabled"_q,
		.title = QString::fromUtf8("Анти-лаг"),
		.description = QString::fromUtf8("Автоматически включает экономный режим при подвисаниях."),
		.kind = S::Kind::Bool,
		.defaultValue = true,
	});
	lag({
		.key = u"lag.level"_q,
		.title = QString::fromUtf8("Что отключать при лагах"),
		.description = QString::fromUtf8("Мягко: фон чата, эффекты, спойлеры. Средне: ещё стикеры и эмодзи. Жёстко: всё."),
		.kind = S::Kind::Choice,
		.defaultValue = 1,
		.choices = {
			QString::fromUtf8("Мягко"),
			QString::fromUtf8("Средне"),
			QString::fromUtf8("Жёстко"),
		},
		.dependsOn = u"lag.enabled"_q,
	});
	lag({
		.key = u"lag.sensitivity"_q,
		.title = QString::fromUtf8("Сколько подвисаний за 10 секунд считать лагом"),
		.description = QString::fromUtf8("Меньше число: реагирует раньше."),
		.kind = S::Kind::Int,
		.defaultValue = 8,
		.minValue = 3,
		.maxValue = 40,
		.dependsOn = u"lag.enabled"_q,
	});
	lag({
		.key = u"lag.calm"_q,
		.title = QString::fromUtf8("Через сколько секунд тишины вернуть всё"),
		.kind = S::Kind::Int,
		.defaultValue = 45,
		.minValue = 10,
		.maxValue = 600,
		.dependsOn = u"lag.enabled"_q,
	});
	lag({
		.key = u"lag.cache"_q,
		.title = QString::fromUtf8("Чистить кэш при лагах"),
		.description = QString::fromUtf8("Только если он больше предела ниже. Не чаще раза в полчаса."),
		.kind = S::Kind::Bool,
		.defaultValue = false,
		.dependsOn = u"lag.enabled"_q,
	});
	lag({
		.key = u"lag.cache_mb"_q,
		.title = QString::fromUtf8("Предел кэша, МБ"),
		.kind = S::Kind::Int,
		.defaultValue = 2048,
		.minValue = 256,
		.maxValue = 20480,
		.dependsOn = u"lag.cache"_q,
	});
	lag({
		.key = u"lag.notify"_q,
		.title = QString::fromUtf8("Говорить, когда режим включается"),
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
	const auto delta = (state.last > 0) ? (now - state.last - 100) : 0;
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
			Toast(QString::fromUtf8("Анти-лаг: включил экономный режим"));
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
	timer->setTimerType(Qt::PreciseTimer);
	timer->setInterval(100);
	QObject::connect(timer, &QTimer::timeout, [] { LagTick(state); });
	timer->start();

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
	}, lifetime);
}

} // namespace Mokagram::Guard
