// Mokagram: история сообщений, реализация (снимки, реестр настроек, хранилища).
#include "mokagram/history/mokagram_history.h"

#include "mokagram/settings/mokagram_settings.h"

#include "base/unixtime.h"
#include "mtproto/sender.h"
#include <deque>
#include "core/click_handler_types.h"
#include "data/data_channel.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"
#include "settings.h"
#include "window/window_session_controller.h"

#include <crl/crl_on_main.h>

#include <QtCore/QDir>

#include <algorithm>

namespace Mokagram::History {
namespace {

[[nodiscard]] QString U(const char *text) {
	return QString::fromUtf8(text);
}

struct Stores {
	base::flat_map<not_null<Main::Session*>, std::unique_ptr<Store>> map;
};

[[nodiscard]] Stores &AllStores() {
	static auto result = Stores();
	return result;
}

[[nodiscard]] Limits CurrentLimits() {
	return Limits{
		.days = Settings::Int(u"history.keep_days"_q),
		.megabytes = Settings::Int(u"history.keep_mb"_q),
		.perChat = Settings::Int(u"history.keep_per_chat"_q),
	};
}

[[nodiscard]] bool Eligible(not_null<HistoryItem*> item, Kind kind) {
	const auto key = (kind == Kind::Deleted)
		? u"history.save_deleted"_q
		: u"history.save_edited"_q;
	if (!Settings::Bool(key)) {
		return false;
	} else if ((kind == Kind::Deleted && item->out())
		|| item->isService()
		|| item->isLocal()
		|| item->isEmpty()
		|| !item->isRegular()) {
		return false;
	}
	const auto peer = item->history()->peer;
	if (!Settings::Bool(u"history.save_for_bots"_q)) {
		if (const auto user = peer->asUser()) {
			if (user->isBot()) {
				return false;
			}
		}
	}
	return true;
}

// Недавнее медиа сообщений: нужно, чтобы показать фото и голосовые после удаления.
struct MediaMemory {
	base::flat_map<FullMsgId, QByteArray> map;
	std::deque<FullMsgId> order;
};

[[nodiscard]] MediaMemory &MediaMem() {
	static auto result = MediaMemory();
	return result;
}

[[nodiscard]] Snapshot MakeSnapshot(not_null<HistoryItem*> item, Kind kind) {
	auto result = Snapshot();
	result.peer = item->history()->peer->id.value;
	result.topic = item->topicRootId().bare;
	result.id = item->id.bare;
	result.kind = kind;
	result.date = item->date();
	result.event = base::unixtime::now();
	result.from = item->from()->id.value;
	result.text = item->originalText().text;
	if (item->media()) {
		result.media = item->notificationText().text;
		if (kind == Kind::Deleted) {
			auto &mem = MediaMem().map;
			const auto i = mem.find(item->fullId());
			if (i != end(mem)) {
				result.tl = i->second;
			}
		}
	}
	return result;
}

void RememberMediaImpl(FullMsgId id, QByteArray tl) {
	auto &mem = MediaMem();
	if (mem.map.emplace(id, std::move(tl)).second) {
		mem.order.push_back(id);
		while (mem.order.size() > 4000) {
			mem.map.remove(mem.order.front());
			mem.order.pop_front();
		}
	}
}

// Призраки удалённых сообщений.
struct GhostHistory {
	int64 loadedFrom = 0;
	bool loaded = false;
	bool loading = false;
	base::flat_set<int64> created; // Исходные id, повторно не создаём.
};

struct GhostSession {
	base::flat_map<not_null<::History*>, GhostHistory> histories;
	base::flat_set<not_null<HistoryItem*>> items;
	base::flat_map<not_null<HistoryItem*>, int64> origin;
	rpl::lifetime lifetime;
};

[[nodiscard]] base::flat_map<
	not_null<Main::Session*>,
	std::unique_ptr<GhostSession>> &AllGhosts() {
	static auto result = base::flat_map<
		not_null<Main::Session*>,
		std::unique_ptr<GhostSession>>();
	return result;
}

[[nodiscard]] GhostSession *GhostsIfExist(not_null<Main::Session*> session) {
	auto &map = AllGhosts();
	const auto i = map.find(session);
	return (i != end(map)) ? i->second.get() : nullptr;
}

[[nodiscard]] GhostSession &GhostsFor(not_null<Main::Session*> session) {
	auto &map = AllGhosts();
	const auto i = map.find(session);
	if (i != end(map)) {
		return *i->second;
	}
	auto created = std::make_unique<GhostSession>();
	const auto raw = created.get();
	map.emplace(session, std::move(created));
	session->data().itemRemoved(
	) | rpl::on_next([=](not_null<const HistoryItem*> item) {
		raw->items.remove(const_cast<HistoryItem*>(item.get()));
		raw->origin.remove(const_cast<HistoryItem*>(item.get()));
	}, raw->lifetime);
	session->lifetime().add([=] {
		AllGhosts().erase(session);
	});
	return *raw;
}

[[nodiscard]] bool GhostsEnabled() {
	return Settings::Bool(u"history.save_deleted"_q)
		&& Settings::Bool(u"history.show_ghosts"_q);
}

[[nodiscard]] bool GhostPeerAllowed(not_null<PeerData*> peer) {
	return !peer->isForum() && !peer->isMonoforum();
}

[[nodiscard]] QString GhostText(const Snapshot &row, bool hasMedia) {
	auto body = row.text;
	if (!hasMedia && !row.media.isEmpty() && row.media != row.text) {
		body = body.isEmpty()
			? (u"["_q + row.media + u"]"_q)
			: (u"["_q + row.media + u"]\n"_q + body);
	}
	if (body.isEmpty() && !hasMedia) {
		body = u"[пусто]"_q;
	}
	return body;
}

// Создаёт призраков и один раз вставляет их в блоки по дате.
void CreateGhosts(
		not_null<Main::Session*> session,
		std::vector<Snapshot> rows) {
	if (!GhostsEnabled()) {
		return;
	}
	auto &ghosts = GhostsFor(session);
	auto touched = base::flat_set<not_null<::History*>>();
	auto made = std::vector<not_null<HistoryItem*>>();
	for (const auto &row : rows) {
		if (row.kind != Kind::Deleted || row.topic > 1) {
			continue;
		}
		const auto history = session->data().historyLoaded(PeerId(row.peer));
		if (!qEnvironmentVariableIsEmpty("MOKAGRAM_DEBUG_CMD")) {
			fprintf(stderr, "ROW peer=%llu id=%lld hist=%d topic=%lld\n", (unsigned long long)row.peer, (long long)row.id, history != nullptr, (long long)row.topic);
		}
		if (!history || !GhostPeerAllowed(history->peer)) {
			continue;
		}
		auto &state = ghosts.histories[history];
		if (!state.created.emplace(row.id).second) {
			continue;
		}
		auto fields = HistoryItemCommonFields{
			.id = session->data().nextLocalMessageId(),
			.flags = (MessageFlag::Local | MessageFlag::HasFromId),
			.from = PeerId(row.from),
			.date = row.date,
		};
		if (row.from == session->userPeerId().value) {
			fields.flags |= MessageFlag::Outgoing;
		}
		const auto channel = history->peer->asChannel();
		if (channel && channel->isBroadcast()) {
			fields.flags |= MessageFlag::Post;
		}
		auto media = MTPMessageMedia(MTP_messageMediaEmpty());
		auto hasMedia = false;
		if (!row.tl.isEmpty()) {
			auto from = reinterpret_cast<const mtpPrime*>(
				row.tl.constData());
			const auto till = from + row.tl.size() / int(sizeof(mtpPrime));
			auto parsed = MTPMessageMedia();
			if (parsed.read(from, till)) {
				media = std::move(parsed);
				hasMedia = true;
			}
		}
		const auto item = history->makeMessage(
			std::move(fields),
			TextWithEntities{ GhostText(row, hasMedia) },
			media);
		ghosts.items.emplace(item);
		ghosts.origin.emplace(item, row.id);
		made.push_back(item);
		touched.emplace(history);
	}
	for (const auto &history : touched) {
		history->checkLocalMessages();
	}
	if (!qEnvironmentVariableIsEmpty("MOKAGRAM_DEBUG_CMD")) {
		for (const auto &item : made) {
			const auto h = item->history();
			fprintf(stderr, "GHOST id=%lld date=%d view=%d top=%d bottom=%d empty=%d\n", (long long)item->id.bare, item->date(), item->mainView() != nullptr, h->loadedAtTop(), h->loadedAtBottom(), h->isEmpty());
		}
		fprintf(stderr, "CREATE rows=%d made=%d\n", int(rows.size()), int(made.size()));
	}
	for (const auto &item : made) {
		if (item->mainView()) {
			session->data().requestItemResize(item);
		}
	}
}

void DestroyAllGhosts() {
	for (const auto &[session, ghosts] : AllGhosts()) {
		auto list = std::vector<not_null<HistoryItem*>>(
			ghosts->items.begin(),
			ghosts->items.end());
		ghosts->items.clear();
		for (auto &[history, state] : ghosts->histories) {
			state.loaded = false;
			state.created.clear();
		}
		if (list.empty()) {
			continue;
		}
		session->data().notifyItemsAboutToBeDestroyed(list);
		for (const auto &item : list) {
			item->destroy();
		}
	}
}

void RepaintAllGhosts() {
	for (const auto &[session, ghosts] : AllGhosts()) {
		for (const auto &item : ghosts->items) {
			session->data().requestItemRepaint(item);
		}
	}
}

void StartGhostSettings() {
	static auto lifetime = rpl::lifetime();
	rpl::combine(
		Settings::BoolValue(u"history.save_deleted"_q),
		Settings::BoolValue(u"history.show_ghosts"_q)
	) | rpl::on_next([](bool save, bool show) {
		if (!save || !show) {
			DestroyAllGhosts();
		}
	}, lifetime);
	Settings::BoolValue(
		u"history.dim_deleted"_q
	) | rpl::skip(1) | rpl::on_next([] {
		RepaintAllGhosts();
	}, lifetime);
}

} // namespace

bool KeepSelfDestruct() {
	return Settings::Bool(u"history.keep_ttl"_q);
}

bool WantsMediaTl() {
	return Settings::Bool(u"history.save_deleted"_q);
}

void RememberMedia(FullMsgId id, QByteArray tl) {
	RememberMediaImpl(id, std::move(tl));
}

void RegisterSettings() {
	const auto section = U("История и фильтры");
	const auto note = U("Когда собеседник удаляет или правит сообщение, "
		"у вас остаётся копия. Копии лежат только на этом компьютере "
		"и никуда не отправляются.");
	const auto add = [&](Settings::Entry entry) {
		entry.section = section;
		entry.category = u"chats"_q;
		entry.sectionIcon = u"history"_q;
		entry.sectionNote = note;
		Settings::Register(std::move(entry));
	};
	add({
		.key = u"history.save_deleted"_q,
		.title = U("Сохранять удалённые сообщения"),
		.description = U("Если собеседник удалил сообщение, вы всё равно "
			"сможете его увидеть. Учитывайте приватность: сохраняются "
			"и чужие сообщения."),
		.kind = Settings::Kind::Bool,
		.defaultValue = false,
		.icon = u"trash"_q,
		.example = U("Собеседник удалил → у вас осталось"),
	});
	add({
		.key = u"history.show_ghosts"_q,
		.title = U("Показывать удалённые в чате"),
		.description = U("Удалённое сообщение остаётся в переписке с меткой."),
		.kind = Settings::Kind::Bool,
		.defaultValue = true,
		.dependsOn = u"history.save_deleted"_q,
		.icon = u"trash"_q,
	});
	add({
		.key = u"history.dim_deleted"_q,
		.title = U("Делать удалённые полупрозрачными"),
		.description = U("Так их легко отличить от обычных."),
		.kind = Settings::Kind::Bool,
		.defaultValue = true,
		.dependsOn = u"history.show_ghosts"_q,
		.icon = u"trash"_q,
	});
	add({
		.key = u"history.keep_ttl"_q,
		.title = U("Не терять самоуничтожающиеся сообщения"),
		.description = U("Голосовые, кружки, фото и видео с таймером "
			"открываются на ПК как обычные и сохраняются у вас."),
		.kind = Settings::Kind::Bool,
		.defaultValue = true,
		.icon = u"history"_q,
	});
	add({
		.key = u"history.save_edited"_q,
		.title = U("Сохранять прошлые версии правок"),
		.description = U("Если сообщение отредактировали, старый текст "
			"останется у вас."),
		.kind = Settings::Kind::Bool,
		.defaultValue = false,
		.icon = u"edit"_q,
	});
	add({
		.key = u"history.save_for_bots"_q,
		.title = U("Сохранять и в чатах с ботами"),
		.description = U("По умолчанию переписка с ботами не сохраняется."),
		.kind = Settings::Kind::Bool,
		.defaultValue = false,
		.icon = u"bot"_q,
	});
	add({
		.key = u"history.filters_enabled"_q,
		.title = U("Фильтры сообщений"),
		.description = U("Скрывает сообщения, подходящие под ваши "
			"шаблоны (регулярные выражения)."),
		.kind = Settings::Kind::Bool,
		.defaultValue = false,
		.icon = u"filter"_q,
	});
	add({
		.key = u"history.hide_blocked"_q,
		.title = U("Прятать заблокированных везде"),
		.description = U("Сообщения людей из чёрного списка не "
			"показываются в чатах."),
		.kind = Settings::Kind::Bool,
		.defaultValue = false,
		.icon = u"block"_q,
	});
	add({
		.key = u"history.mark"_q,
		.title = U("Значок у удалённого"),
		.description = U("Ставится рядом со временем удалённого сообщения, чтобы "
			"его было видно в переписке."),
		.kind = Settings::Kind::String,
		.defaultValue = QString::fromUtf8("🗑"),
		.icon = u"trash"_q,
	});
	add({
		.key = u"history.keep_days"_q,
		.title = U("Сколько дней хранить копии"),
		.description = U("Более старые копии стираются. "
			"0 значит хранить без ограничения по времени."),
		.kind = Settings::Kind::Int,
		.defaultValue = 30,
		.minValue = 0,
		.maxValue = 3650,
		.icon = u"calendar"_q,
	});
	add({
		.key = u"history.keep_mb"_q,
		.title = U("Сколько места занять (МБ)"),
		.description = U("Предел для одного аккаунта. "
			"При переполнении удаляются самые старые копии."),
		.kind = Settings::Kind::Int,
		.defaultValue = 256,
		.minValue = 16,
		.maxValue = 4096,
		.icon = u"storage"_q,
	});
	add({
		.key = u"history.keep_per_chat"_q,
		.title = U("Сколько копий на один чат"),
		.description = U("Чтобы один шумный чат не занял всё место."),
		.kind = Settings::Kind::Int,
		.defaultValue = 5000,
		.minValue = 100,
		.maxValue = 100000,
		.icon = u"chats"_q,
	});
	StartFilterSettings();
	StartGhostSettings();
}

Store &StoreFor(not_null<Main::Session*> session) {
	auto &map = AllStores().map;
	const auto i = map.find(session);
	if (i != end(map)) {
		return *i->second;
	}
	const auto dir = cWorkingDir()
		+ u"tdata/mokagram/%1/"_q.arg(session->uniqueId());
	QDir().mkpath(dir);
	auto store = std::make_unique<Store>(dir + u"history.sqlite"_q);
	store->setLimitsProvider(CurrentLimits);
	const auto raw = store.get();
	map.emplace(session, std::move(store));
	// Сессия закрывается: писатель дописывает очередь и закрывает базу.
	session->lifetime().add([=] {
		AllStores().map.erase(session);
	});
	return *raw;
}

void OnServerDeleted(const std::vector<not_null<HistoryItem*>> &items) {
	auto ghosts = std::vector<Snapshot>();
	Main::Session *ghostSession = nullptr;
	for (const auto &item : items) {
		if (!Eligible(item, Kind::Deleted)) {
			continue;
		}
		const auto session = &item->history()->session();
		auto snapshot = MakeSnapshot(item, Kind::Deleted);
		if (GhostsEnabled()) {
			ghostSession = session;
			ghosts.push_back(snapshot);
		}
		StoreFor(session).push(std::move(snapshot));
	}
	if (ghosts.empty()) {
		return;
	}
	// Оригиналы ещё не уничтожены: призраков создаём после, в главном цикле.
	std::sort(ghosts.begin(), ghosts.end(), [](const auto &a, const auto &b) {
		return a.id < b.id;
	});
	const auto weak = base::make_weak(ghostSession);
	crl::on_main(weak, [=, rows = std::move(ghosts)]() mutable {
		if (const auto strong = weak.get()) {
			CreateGhosts(strong, std::move(rows));
		}
	});
}

void OnBeforeEdit(
		not_null<HistoryItem*> item,
		const QString &newText,
		TimeId newEditDate) {
	if (!Eligible(item, Kind::Edited)) {
		return;
	}
	const auto edited = item->Get<HistoryMessageEdited>();
	const auto oldEditDate = edited ? edited->date : TimeId(0);
	if (newText == item->originalText().text
		&& newEditDate == oldEditDate) {
		return;
	}
	StoreFor(&item->history()->session()).push(
		MakeSnapshot(item, Kind::Edited));
}

} // namespace Mokagram::History

namespace Mokagram::History {

bool IsGhost(not_null<const HistoryItem*> item) {
	if (!item->isLocal()) {
		return false;
	}
	const auto ghosts = GhostsIfExist(&item->history()->session());
	return ghosts
		&& ghosts->items.contains(const_cast<HistoryItem*>(item.get()));
}

void RemoveGhost(not_null<HistoryItem*> item) {
	const auto session = &item->history()->session();
	const auto ghosts = GhostsIfExist(session);
	if (!ghosts) {
		return;
	}
	const auto i = ghosts->origin.find(item);
	if (i != end(ghosts->origin)) {
		StoreFor(session).removeOne(
			item->history()->peer->id.value,
			i->second);
		ghosts->origin.erase(i);
	}
	ghosts->items.remove(item);
	session->data().notifyItemsAboutToBeDestroyed({ item });
	item->destroy();
}

bool GhostDimmed(not_null<const HistoryItem*> item) {
	return IsGhost(item) && Settings::Bool(u"history.dim_deleted"_q);
}

void OnChatShown(not_null<::History*> history) {
	const auto peer = history->peer;
	if (!qEnvironmentVariableIsEmpty("MOKAGRAM_DEBUG_CMD")) {
		fprintf(stderr, "SHOWN peer=%llu empty=%d top=%d min=%lld ghosts=%d\n", (unsigned long long)peer->id.value, history->isEmpty(), history->loadedAtTop(), (long long)history->minMsgId().bare, GhostsEnabled());
	}
	const auto session = &history->session();
	if (Settings::Bool(u"history.save_edited"_q)) {
		StoreFor(session).ensureRevisionsLoaded(peer->id.value);
	}
	if (!GhostsEnabled()
		|| !GhostPeerAllowed(peer)
		|| history->isEmpty()) {
		return;
	}
	const auto from = history->loadedAtTop()
		? int64(0)
		: history->minMsgId().bare;
	if (!history->loadedAtTop() && !from) {
		return;
	}
	auto &state = GhostsFor(session).histories[history];
	if (state.loading || (state.loaded && from >= state.loadedFrom)) {
		return;
	}
	state.loading = true;
	const auto weak = base::make_weak(session);
	const auto peerId = peer->id;
	StoreFor(session).loadPage(PageQuery{
		.peer = peerId.value,
		.kind = Kind::Deleted,
		.minId = from ? from : 1,
		.limit = 3000,
	}, [=](std::vector<Snapshot> rows) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto owner = strong;
		if (!qEnvironmentVariableIsEmpty("MOKAGRAM_DEBUG_CMD")) {
			fprintf(stderr, "ROWS %d\n", int(rows.size()));
		}
		if (const auto h = owner->data().historyLoaded(peerId)) {
			auto &now = GhostsFor(owner).histories[h];
			now.loading = false;
			now.loaded = true;
			now.loadedFrom = from;
		}
		std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
			return a.id < b.id;
		});
		CreateGhosts(owner, std::move(rows));
	});
}

bool HasRevisions(not_null<const HistoryItem*> item) {
	return Settings::Bool(u"history.save_edited"_q)
		&& item->isRegular()
		&& StoreFor(&item->history()->session()).hasRevisions(
			item->fullId());
}

ClickHandlerPtr RevisionsLink(not_null<HistoryItem*> item) {
	if (!HasRevisions(item)) {
		return nullptr;
	}
	static auto cache = base::flat_map<FullMsgId, ClickHandlerPtr>();
	const auto id = item->fullId();
	const auto i = cache.find(id);
	if (i != end(cache)) {
		return i->second;
	} else if (cache.size() > 256) {
		cache.clear();
	}
	const auto peerId = item->history()->peer->id.value;
	const auto msgId = item->id.bare;
	auto link = std::make_shared<LambdaClickHandler>([=](ClickContext context) {
		const auto my = context.other.value<ClickHandlerContext>();
		if (const auto controller = my.sessionWindow.get()) {
			ShowRevisionsBox(controller, peerId, msgId);
		}
	});
	cache.emplace(id, link);
	return link;
}

void ShowRevisions(
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	ShowRevisionsBox(
		controller,
		item->history()->peer->id.value,
		item->id.bare);
}

bool ShowInPeerMenu() {
	return Settings::Bool(u"history.save_deleted"_q)
		|| Settings::Bool(u"history.save_edited"_q);
}

} // namespace Mokagram::History
