#include "mokagram/ghost/mokagram_ghost.h"

#include "mokagram/settings/mokagram_settings.h"

#include "api/api_updates.h"
#include "core/application.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "apiwrap.h"
#include "mtproto/sender.h"

#include <rpl/producer.h>

namespace Mokagram::Ghost {
namespace {

struct Flags {
	bool enabled = false;
	bool read = false;
	bool online = false;
	bool typing = false;
	bool stories = false;
	bool offlineOnEnable = false;
	bool hideBlocked = false;
};

struct State {
	Flags flags;
	bool loaded = false;
	rpl::lifetime lifetime;
};

[[nodiscard]] State &Data() {
	static auto result = State();
	return result;
}

[[nodiscard]] Flags ReadFlags() {
	return {
		.enabled = Settings::Bool(u"ghost.enabled"_q),
		.read = Settings::Bool(u"ghost.read"_q),
		.online = Settings::Bool(u"ghost.online"_q),
		.typing = Settings::Bool(u"ghost.typing"_q),
		.stories = Settings::Bool(u"ghost.stories"_q),
		.offlineOnEnable = Settings::Bool(u"ghost.offline_on_enable"_q),
		.hideBlocked = Settings::Bool(u"filters.hide_blocked"_q),
	};
}

// Ленивая загрузка: первый вызов читает реестр, дальше работает кэш.
[[nodiscard]] const Flags &Current() {
	auto &data = Data();
	if (!data.loaded) {
		data.loaded = true;
		data.flags = ReadFlags();
	}
	return data.flags;
}

[[nodiscard]] bool HiddenOnline(const Flags &flags) {
	return flags.enabled && flags.online;
}

// Один раз сообщаем серверу, что нас нет в сети, и пересчитываем состояние.
void SendOfflineOnce() {
	for (const auto account : Core::App().domain().orderedAccounts()) {
		const auto session = account->maybeSession();
		if (!session) {
			continue;
		}
		session->api().request(MTPaccount_UpdateStatus(
			MTP_bool(true)
		)).send();
		session->updates().updateOnline();
	}
}

void Refresh() {
	auto &data = Data();
	const auto was = Current();
	data.flags = ReadFlags();
	const auto &now = data.flags;
	const auto hidOnline = HiddenOnline(now) && !HiddenOnline(was);
	if (hidOnline && now.offlineOnEnable) {
		SendOfflineOnce();
	} else if (HiddenOnline(was) && !HiddenOnline(now)) {
		// Вернули «в сети»: штатный пересчёт состояния.
		for (const auto account : Core::App().domain().orderedAccounts()) {
			if (const auto session = account->maybeSession()) {
				session->updates().updateOnline();
			}
		}
	}
}

} // namespace

void RegisterSettings() {
	const auto ghost = QString::fromUtf8("Призрак");
	const auto note = QString::fromUtf8(
		"Вы спокойно читаете чаты, а собеседники не видят, "
		"что вы были в сети и прочитали сообщения. "
		"Отключать пункты можно по отдельности.");
	const auto add = [&](
			const char *key,
			const char *title,
			const char *description,
			const char *icon,
			bool value,
			const char *dependsOn = "ghost.enabled") {
		Settings::Register({
			.key = QString::fromUtf8(key),
			.section = ghost,
			.title = QString::fromUtf8(title),
			.description = QString::fromUtf8(description),
			.kind = Settings::Kind::Bool,
			.defaultValue = value,
			.dependsOn = QString::fromUtf8(dependsOn),
			.category = u"privacy"_q,
			.sectionIcon = u"ghost"_q,
			.sectionNote = note,
			.icon = QString::fromUtf8(icon),
		});
	};
	add("ghost.enabled",
		"Призрак",
		"Вы читаете чаты, а собеседники не видят, что вы были в сети "
		"и прочитали.",
		"ghost",
		false,
		"");
	add("ghost.read",
		"Не отмечать прочитанным",
		"Вы открыли чат и прочитали, а у собеседника сообщение "
		"остаётся непрочитанным (без двух галочек).",
		"read",
		true);
	add("ghost.online",
		"Скрывать, что я в сети",
		"Собеседники не видят «в сети» и время вашего последнего "
		"визита.",
		"online",
		true);
	add("ghost.typing",
		"Скрывать «печатает»",
		"Не показываем, что вы пишете, записываете голосовое "
		"или загружаете файл.",
		"typing",
		true);
	add("ghost.stories",
		"Скрывать просмотр историй",
		"Автор не узнает, что вы смотрели его историю.",
		"stories",
		true);
	add("ghost.offline_on_enable",
		"Выйти из сети при включении",
		"Сразу после включения Призрака один раз сообщаем, "
		"что вас нет в сети.",
		"online",
		true);
	Settings::Register({
		.key = u"filters.hide_blocked"_q,
		.section = QString::fromUtf8("Заблокированные"),
		.title = QString::fromUtf8("Прятать заблокированных в группах"),
		.description = QString::fromUtf8(
			"Сообщения людей из вашего чёрного списка пропадают "
			"из групп и обсуждений. Личные диалоги не трогаем."),
		.kind = Settings::Kind::Bool,
		.defaultValue = false,
		.category = u"chats"_q,
		.sectionIcon = u"block"_q,
		.sectionNote = QString::fromUtf8(
			"Заблокировать человека мало: в группах его сообщения "
			"всё равно видны. Здесь можно их спрятать."),
	});

	auto &data = Data();
	data.loaded = true;
	data.flags = ReadFlags();
	data.lifetime.destroy();
	Settings::Changed(
	) | rpl::on_next([](const QString &key) {
		if (key.startsWith(u"ghost."_q) || key == u"filters.hide_blocked"_q) {
			Refresh();
		}
	}, data.lifetime);
}

bool ShouldSendRead() {
	const auto &flags = Current();
	return !(flags.enabled && flags.read);
}

bool ShouldSendOnline() {
	return !HiddenOnline(Current());
}

bool ShouldSendTyping() {
	const auto &flags = Current();
	return !(flags.enabled && flags.typing);
}

bool ShouldSendStoryView() {
	const auto &flags = Current();
	return !(flags.enabled && flags.stories);
}

bool HideBlockedEnabled() {
	return Current().hideBlocked;
}

bool HidesBlocked(not_null<const HistoryItem*> item) {
	if (!Current().hideBlocked || item->out()) {
		return false;
	}
	const auto history = item->history();
	const auto peer = history->peer;
	const auto blocked = [&](PeerData *who) {
		return who && who->isBlocked() && who != peer;
	};
	if (blocked(item->from())) {
		return true;
	}
	if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
		if (blocked(forwarded->originalSender)) {
			return true;
		}
	}
	return blocked(item->viaBot());
}

} // namespace Mokagram::Ghost
