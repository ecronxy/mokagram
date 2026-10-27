// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 26415de3
#include "mokagram/ghost/mokagram_ghost.h"
#include "mokagram/lang/mokagram_lang.h"

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
#include "base/flat_set.h"
#include "base/timer.h"
#include "data/data_channel.h"
#include "mokagram/plugins/mokagram_plugin_install.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "ui/effects/animations.h"
#include "ui/effects/animation_value.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/time_input.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/rp_widget.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_passcode_box.h"
#include "styles/style_widgets.h"
#include <QtCore/QTime>

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

[[nodiscard]] QString U(const char *text) {
	return QString::fromUtf8(text);
}

// Карточка с закруглением, как в окне «Плагины».
[[nodiscard]] not_null<Ui::VerticalLayout*> AddScheduleCard(
		not_null<Ui::VerticalLayout*> parent) {
	const auto slot = parent->add(
		object_ptr<Ui::VerticalLayout>(parent),
		QMargins(14, 6, 14, 6),
		style::al_justify);
	const auto card = slot->add(
		object_ptr<Ui::VerticalLayout>(slot),
		QMargins(),
		style::al_justify);
	card->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		auto p = QPainter(card);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBg);
		p.drawRoundedRect(card->rect(), 22, 22);
	}, card->lifetime());
	return card;
}

// Строка-переключатель: подпись слева, пилюля справа.
void AddScheduleToggle(
		not_null<Ui::VerticalLayout*> card,
		const QString &title,
		const QString &key) {
	const auto row = card->add(
		object_ptr<Ui::AbstractButton>(card),
		QMargins(),
		style::al_justify);
	row->resize(row->width(), 52);
	row->setPointerCursor(true);
	const auto anim = row->lifetime().make_state<Ui::Animations::Simple>();
	row->setClickedCallback([=] {
		const auto now = !Settings::Bool(key);
		Settings::SetBool(key, now);
		anim->start(
			[=] { row->update(); },
			now ? 0. : 1.,
			now ? 1. : 0.,
			180,
			anim::easeOutCubic);
	});
	row->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(row);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(st::windowFg);
		p.setFont(st::normalFont);
		p.drawText(
			QRect(18, 0, row->width() - 90, row->height()),
			Qt::AlignVCenter | Qt::AlignLeft,
			title);
		const auto t = anim->value(Settings::Bool(key) ? 1. : 0.);
		const auto pill = QRectF(row->width() - 18 - 46, (row->height() - 28) / 2., 46, 28);
		p.setPen(Qt::NoPen);
		p.setBrush(anim::color(QColor(0x3A, 0x3A, 0x3C), QColor(0x0A, 0x84, 0xFF), t));
		p.drawRoundedRect(pill, 14, 14);
		p.setBrush(Qt::white);
		const auto x = pill.left() + 3 + (pill.width() - 28) * t;
		p.drawEllipse(QRectF(x, pill.top() + 3, 22, 22));
	}, row->lifetime());
}

// Строка со временем: подпись слева, поле ЧЧ:ММ справа.
void AddScheduleTime(
		not_null<Ui::VerticalLayout*> card,
		const QString &title,
		const QString &key) {
	const auto row = card->add(
		object_ptr<Ui::RpWidget>(card),
		QMargins(),
		style::al_justify);
	row->resize(row->width(), 56);
	const auto label = Ui::CreateChild<Ui::FlatLabel>(
		row,
		title,
		st::defaultFlatLabel);
	const auto input = Ui::CreateChild<Ui::TimeInput>(
		row,
		Settings::String(key),
		st::autolockTimeField,
		st::autolockDateField,
		st::scheduleTimeSeparator,
		st::scheduleTimeSeparatorPadding);
	input->resizeToWidth(st::autolockTimeWidth);
	input->value(
	) | rpl::on_next([=](const QString &value) {
		const auto parsed = QTime::fromString(value, u"HH:mm"_q);
		if (parsed.isValid()) {
			Settings::SetString(key, parsed.toString(u"HH:mm"_q));
		}
	}, input->lifetime());
	row->widthValue(
	) | rpl::on_next([=](int width) {
		label->moveToLeft(18, (row->height() - label->height()) / 2);
		input->moveToRight(18, (row->height() - input->height()) / 2);
	}, row->lifetime());
}

void AddScheduleNote(
		not_null<Ui::VerticalLayout*> content,
		const QString &text) {
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			text,
			st::boxDividerLabel),
		QMargins(32, 4, 32, 6));
}

void ScheduleBox(not_null<Ui::GenericBox*> box) {
	Plugins::RoundedWindow(box);
	box->setWidth(440);
	const auto content = box->verticalLayout();
	const auto header = content->add(
		object_ptr<Ui::RpWidget>(content),
		QMargins(10, 10, 10, 6));
	header->resize(header->width(), 40);
	const auto close = Plugins::MakeHeaderButton(
		header,
		true,
		[=] { box->closeBox(); });
	const auto title = Ui::CreateChild<Ui::FlatLabel>(
		header,
		U(Mokagram::Lang::Tr("Расписание призрака")),
		st::boxTitle);
	header->widthValue(
	) | rpl::on_next([=](int width) {
		close->move(0, (header->height() - close->height()) / 2);
		title->resizeToNaturalWidth(width - 2 * (close->width() + 8));
		title->move(
			(width - title->width()) / 2,
			(header->height() - title->height()) / 2);
	}, header->lifetime());

	const auto main = AddScheduleCard(content);
	AddScheduleToggle(main, U(Mokagram::Lang::Tr("Включать по расписанию")), u"ghost.schedule"_q);
	AddScheduleTime(main, U(Mokagram::Lang::Tr("С")), u"ghost.schedule_from"_q);
	AddScheduleTime(main, U(Mokagram::Lang::Tr("До")), u"ghost.schedule_to"_q);
	AddScheduleNote(
		content,
		U(Mokagram::Lang::Tr("Вне этих часов действуют обычные настройки Призрака.")));

	AddScheduleNote(content, U(Mokagram::Lang::Tr("ЧТО ВКЛЮЧАЕТСЯ В ЭТО ВРЕМЯ")));
	const auto what = AddScheduleCard(content);
	AddScheduleToggle(what, U(Mokagram::Lang::Tr("Не отмечать прочитанным")), u"ghost.sched_read"_q);
	AddScheduleToggle(what, U(Mokagram::Lang::Tr("Скрыть «в сети»")), u"ghost.sched_online"_q);
	AddScheduleToggle(what, U(Mokagram::Lang::Tr("Не показывать «печатает»")), u"ghost.sched_typing"_q);
	AddScheduleToggle(what, U(Mokagram::Lang::Tr("Скрыть просмотры историй")), u"ghost.sched_stories"_q);
	content->add(object_ptr<Ui::RpWidget>(content))->resize(0, 14);
}

struct State {
	Flags flags;
	bool loaded = false;
	base::flat_set<uint64> exceptions;
	rpl::lifetime lifetime;
};

void LoadExceptions(State &data) {
	data.exceptions.clear();
	const auto list = Settings::String(u"ghost.exceptions"_q).split(
		u","_q,
		Qt::SkipEmptyParts);
	for (const auto &id : list) {
		data.exceptions.emplace(id.trimmed().toULongLong());
	}
}

// Сейчас ли время, на которое назначено расписание.
[[nodiscard]] bool ScheduleInside() {
	if (!Settings::Bool(u"ghost.schedule"_q)) {
		return false;
	}
	const auto from = QTime::fromString(
		Settings::String(u"ghost.schedule_from"_q).trimmed(),
		u"HH:mm"_q);
	const auto to = QTime::fromString(
		Settings::String(u"ghost.schedule_to"_q).trimmed(),
		u"HH:mm"_q);
	if (!from.isValid() || !to.isValid() || from == to) {
		return false;
	}
	const auto now = QTime::currentTime();
	return (from < to)
		? (now >= from && now < to)
		: (now >= from || now < to);
}

void UpdateScheduleLabel() {
	const auto label = Settings::Bool(u"ghost.schedule"_q)
		? (Settings::String(u"ghost.schedule_from"_q)
			+ QString::fromUtf8(" – ")
			+ Settings::String(u"ghost.schedule_to"_q))
		: QString::fromUtf8(Mokagram::Lang::Tr("Выключено"));
	if (Settings::String(u"ghost.schedule_label"_q) != label) {
		Settings::SetString(u"ghost.schedule_label"_q, label);
	}
}

[[nodiscard]] State &Data() {
	static auto result = State();
	return result;
}

[[nodiscard]] Flags ReadFlags() {
	auto result = Flags{
		.enabled = Settings::Bool(u"ghost.enabled"_q),
		.read = Settings::Bool(u"ghost.read"_q),
		.online = Settings::Bool(u"ghost.online"_q),
		.typing = Settings::Bool(u"ghost.typing"_q),
		.stories = Settings::Bool(u"ghost.stories"_q),
		.offlineOnEnable = Settings::Bool(u"ghost.offline_on_enable"_q),
		.hideBlocked = Settings::Bool(u"filters.hide_blocked"_q),
	};
	if (ScheduleInside()) {
		// В часы расписания к обычным настройкам добавляется выбранное.
		if (!result.enabled) {
			result.read = result.online = result.typing = result.stories
				= false;
		}
		result.enabled = true;
		result.read |= Settings::Bool(u"ghost.sched_read"_q);
		result.online |= Settings::Bool(u"ghost.sched_online"_q);
		result.typing |= Settings::Bool(u"ghost.sched_typing"_q);
		result.stories |= Settings::Bool(u"ghost.sched_stories"_q);
	}
	return result;
}

void Refresh();

// Расписание: смена «внутри / снаружи» пересчитывает флаги.
void ApplySchedule() {
	static auto was = false;
	const auto now = ScheduleInside();
	if (now != was) {
		was = now;
		Refresh();
	}
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
	const auto ghost = QString::fromUtf8(Mokagram::Lang::Tr("Призрак"));
	const auto note = QString::fromUtf8(
		Mokagram::Lang::Tr("Вы спокойно читаете чаты, а собеседники не видят, что вы были в сети и прочитали сообщения. Отключать пункты можно по отдельности."));
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
		Mokagram::Lang::Tr("Призрак"),
		Mokagram::Lang::Tr("Вы читаете чаты, а собеседники не видят, что вы были в сети и прочитали."),
		"ghost",
		false,
		"");
	add("ghost.read",
		Mokagram::Lang::Tr("Не отмечать прочитанным"),
		Mokagram::Lang::Tr("Вы открыли чат и прочитали, а у собеседника сообщение остаётся непрочитанным (без двух галочек)."),
		"read",
		true);
	add("ghost.online",
		Mokagram::Lang::Tr("Скрывать, что я в сети"),
		Mokagram::Lang::Tr("Собеседники не видят «в сети» и время вашего последнего визита."),
		"online",
		true);
	add("ghost.typing",
		Mokagram::Lang::Tr("Скрывать «печатает»"),
		Mokagram::Lang::Tr("Не показываем, что вы пишете, записываете голосовое или загружаете файл."),
		"typing",
		true);
	add("ghost.stories",
		Mokagram::Lang::Tr("Скрывать просмотр историй"),
		Mokagram::Lang::Tr("Автор не узнает, что вы смотрели его историю."),
		"stories",
		true);
	add("ghost.offline_on_enable",
		Mokagram::Lang::Tr("Выйти из сети при включении"),
		Mokagram::Lang::Tr("Сразу после включения Призрака один раз сообщаем, что вас нет в сети."),
		"online",
		true);
	Settings::Register({
		.key = u"ghost.schedule_label"_q,
		.section = ghost,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Расписание")),
		.description = QString::fromUtf8(
			Mokagram::Lang::Tr("Часы и то, что включается в это время.")),
		.kind = Settings::Kind::String,
		.defaultValue = QString::fromUtf8(Mokagram::Lang::Tr("Выключено")),
		.category = u"privacy"_q,
		.sectionIcon = u"ghost"_q,
		.icon = u"online"_q,
		.action = [] {
			if (const auto window = Core::App().activePrimaryWindow()) {
				if (const auto controller = window->sessionController()) {
					controller->show(Box(ScheduleBox));
				}
			}
		},
	});
	const auto hidden = [&](const char *key, bool value) {
		Settings::Register({
			.key = QString::fromUtf8(key),
			.section = ghost,
			.title = QString::fromUtf8(key),
			.kind = Settings::Kind::Bool,
			.defaultValue = value,
			.category = u"privacy"_q,
			.hidden = true,
		});
	};
	hidden("ghost.schedule", false);
	hidden("ghost.sched_read", true);
	hidden("ghost.sched_online", true);
	hidden("ghost.sched_typing", true);
	hidden("ghost.sched_stories", false);
	for (const auto &[key, value] : {
		std::pair{ "ghost.schedule_from", "23:00" },
		std::pair{ "ghost.schedule_to", "07:00" },
	}) {
		Settings::Register({
			.key = QString::fromUtf8(key),
			.section = ghost,
			.title = QString::fromUtf8(key),
			.kind = Settings::Kind::String,
			.defaultValue = QString::fromUtf8(value),
			.category = u"privacy"_q,
			.hidden = true,
		});
	}
	Settings::Register({
		.key = u"ghost.exceptions"_q,
		.section = ghost,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Чаты-исключения (id через запятую)")),
		.description = QString::fromUtf8(
			Mokagram::Lang::Tr("В них Призрак не мешает отмечать прочитанным. Проще добавлять через меню чата.")),
		.kind = Settings::Kind::String,
		.defaultValue = QString(),
		.category = u"privacy"_q,
		.hidden = true,
	});
	Settings::Register({
		.key = u"filters.hide_blocked"_q,
		.section = QString::fromUtf8(Mokagram::Lang::Tr("Заблокированные")),
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Прятать заблокированных в группах")),
		.description = QString::fromUtf8(
			Mokagram::Lang::Tr("Сообщения людей из вашего чёрного списка пропадают из групп и обсуждений. Личные диалоги не трогаем.")),
		.kind = Settings::Kind::Bool,
		.defaultValue = false,
		.category = u"chats"_q,
		.sectionIcon = u"block"_q,
		.sectionNote = QString::fromUtf8(
			Mokagram::Lang::Tr("Заблокировать человека мало: в группах его сообщения всё равно видны. Здесь можно их спрятать.")),
	});

	auto &data = Data();
	data.loaded = true;
	data.flags = ReadFlags();
	data.lifetime.destroy();
	LoadExceptions(data);
	Settings::Changed(
	) | rpl::on_next([](const QString &key) {
		if (key == u"ghost.exceptions"_q) {
			LoadExceptions(Data());
		}
		if (key.startsWith(u"ghost.schedule"_q)) {
			UpdateScheduleLabel();
			ApplySchedule();
		}
		if (key.startsWith(u"ghost."_q) || key == u"filters.hide_blocked"_q) {
			Refresh();
		}
	}, data.lifetime);
	static auto timer = base::Timer([] { ApplySchedule(); });
	timer.callEach(30 * crl::time(1000));
	UpdateScheduleLabel();
	ApplySchedule();
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

bool HidesRead() {
	const auto &flags = Current();
	return flags.enabled && flags.read;
}

bool ReadAllowedFor(not_null<::History*> history) {
	if (!HidesRead()) {
		return true;
	}
	return Data().exceptions.contains(history->peer->id.value);
}

bool IsReadException(not_null<PeerData*> peer) {
	return Data().exceptions.contains(peer->id.value);
}

void ToggleReadException(not_null<PeerData*> peer) {
	auto &data = Data();
	const auto id = peer->id.value;
	if (data.exceptions.contains(id)) {
		data.exceptions.remove(id);
	} else {
		data.exceptions.emplace(id);
	}
	auto list = QStringList();
	for (const auto value : data.exceptions) {
		list.push_back(QString::number(value));
	}
	Settings::SetString(u"ghost.exceptions"_q, list.join(u","_q));
}

void ReadNow(not_null<::History*> history) {
	const auto peer = history->peer;
	const auto last = history->lastServerMessage();
	const auto till = last ? last->id : history->maxMsgId();
	if (!till) {
		return;
	}
	auto &api = peer->session().api();
	history->inboxRead(till);
	if (const auto channel = peer->asChannel()) {
		api.request(MTPchannels_ReadHistory(
			channel->inputChannel(),
			MTP_int(till)
		)).send();
	} else {
		api.request(MTPmessages_ReadHistory(
			peer->input(),
			MTP_int(till)
		)).done([=](const MTPmessages_AffectedMessages &result) {
			peer->session().api().applyAffectedMessages(peer, result);
		}).send();
	}
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
