#include "mokagram/commands/mokagram_commands.h"

#include "api/api_blocked_peers.h"
#include "api/api_common.h"
#include "api/api_chat_participants.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/unixtime.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_chat_participant_status.h"
#include "data/data_peer.h"
#include "data/data_histories.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "data/data_user.h"
#include "data/notify/data_notify_settings.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "core/application.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_boxes.h"
#include "styles/style_widgets.h"
#include "window/window_controller.h"
#include "ui/text/text_entity.h"
#include "window/window_peer_menu.h"
#include "main/main_session.h"
#include "mokagram/settings/mokagram_settings.h"
#include "ui/widgets/fields/input_field.h"
#include "window/window_session_controller.h"

#include <QtCore/QRegularExpression>

namespace Mokagram::Commands {
namespace {

namespace S = Mokagram::Settings;

[[nodiscard]] QString U(const char *text) {
	return QString::fromUtf8(text);
}

[[nodiscard]] QString Describe(int seconds) {
	if (seconds <= 0) {
		return U("навсегда");
	} else if (seconds % 86400 == 0) {
		return U("на %1 д").arg(seconds / 86400);
	} else if (seconds % 3600 == 0) {
		return U("на %1 ч").arg(seconds / 3600);
	}
	return U("на %1 мин").arg(seconds / 60);
}

[[nodiscard]] ChatRestrictions MuteFlags() {
	return ChatRestriction::SendOther
		| ChatRestriction::SendStickers
		| ChatRestriction::SendGifs
		| ChatRestriction::SendGames
		| ChatRestriction::SendInline
		| ChatRestriction::SendPolls
		| ChatRestriction::SendPhotos
		| ChatRestriction::SendVideos
		| ChatRestriction::SendVideoMessages
		| ChatRestriction::SendMusic
		| ChatRestriction::SendVoiceMessages
		| ChatRestriction::SendFiles
		| ChatRestriction::EmbedLinks;
}

[[nodiscard]] const QStringList &KnownCommands() {
	static const auto result = QStringList{
		u"mute"_q, u"unmute"_q, u"ban"_q, u"unban"_q, u"kick"_q,
		u"del"_q, u"pin"_q, u"unpin"_q, u"id"_q, u"save"_q,
	};
	return result;
}

// Строки вида «rm = del», по одной на строку.
[[nodiscard]] std::vector<std::pair<QString, QString>> ParseAliases(
		const QString &text) {
	auto result = std::vector<std::pair<QString, QString>>();
	for (const auto &raw : text.split('\n', Qt::SkipEmptyParts)) {
		auto line = raw.trimmed();
		auto parts = line.contains('=')
			? line.split('=')
			: line.split(QRegularExpression(u"\\s+"_q));
		if (parts.size() != 2) {
			continue;
		}
		auto alias = parts[0].trimmed().toLower();
		auto command = parts[1].trimmed().toLower();
		while (alias.startsWith('.')) {
			alias = alias.mid(1);
		}
		while (command.startsWith('.')) {
			command = command.mid(1);
		}
		if (!alias.isEmpty() && KnownCommands().contains(command)) {
			result.emplace_back(alias, command);
		}
	}
	return result;
}

[[nodiscard]] QString ResolveAlias(const QString &word) {
	for (const auto &[alias, command] : ParseAliases(
			S::String(u"commands.aliases"_q))) {
		if (alias == word) {
			return command;
		}
	}
	return word;
}

[[nodiscard]] QString CommandsHelp() {
	auto lines = QStringList{
		U(".mute [30m|2h|1d] глушит чат, а ответом на сообщение в группе запрещает человеку писать"),
		U(".unmute включает звук чата или снимает запрет писать"),
		U(".ban исключает человека (в группе, ответом) или блокирует его в личке"),
		U(".unban снимает бан или разблокирует человека"),
		U(".kick исключает человека, но он может вернуться (ответом, в группе)"),
		U(".del удаляет сообщение, на которое ответил"),
		U(".del N удаляет твои последние N сообщений (с ответом: N сообщений того человека)"),
		U(".del all с ответом удаляет все сообщения человека в группе, без ответа все твои загруженные"),
		U(".pin / .unpin закрепляет или открепляет сообщение (ответом)"),
		U(".id показывает ID автора, чата и сообщения, ссылку на сообщение"),
		U(".save пересылает сообщение (ответом) в «Избранное»"),
	};
	const auto aliases = ParseAliases(S::String(u"commands.aliases"_q));
	if (!aliases.empty()) {
		lines.push_back(QString());
		lines.push_back(U("Твои алиасы:"));
		for (const auto &[alias, command] : aliases) {
			lines.push_back(U(".") + alias + U(" = .") + command);
		}
	}
	return lines.join('\n');
}

[[nodiscard]] QString MessageLink(not_null<HistoryItem*> item) {
	const auto peer = item->history()->peer;
	const auto channel = peer->asChannel();
	if (!channel) {
		return QString();
	}
	const auto username = channel->username();
	return username.isEmpty()
		? (u"https://t.me/c/"_q + QString::number(peerToChannel(channel->id).bare)
			+ '/' + QString::number(item->id.bare))
		: (u"https://t.me/"_q + username + '/' + QString::number(item->id.bare));
}

void ShowInfo(
		not_null<Window::SessionController*> controller,
		not_null<History*> history,
		HistoryItem *item) {
	const auto peer = history->peer;
	PeerData *author = item
		? item->from().get()
		: (peer->isUser() ? peer.get() : nullptr);
	auto lines = QStringList();
	lines.push_back(U("ℹ️ Информация"));
	lines.push_back(QString());
	lines.push_back(U("👤 ID автора: ") + (author
		? QString::number(author->id.value & PeerId::kChatTypeMask)
		: U("неизвестен")));
	if (author && !author->username().isEmpty()) {
		lines.push_back(U("🔖 Ник: @") + author->username());
	}
	lines.push_back(U("📢 ID чата: ") + QString::number(
		peer->id.value & PeerId::kChatTypeMask)
		+ (peer->isChannel() ? U(" (-100…)") : QString()));
	if (item) {
		lines.push_back(U("💬 ID сообщения: ") + QString::number(item->id.bare));
		const auto link = MessageLink(item);
		if (!link.isEmpty()) {
			lines.push_back(QString());
			lines.push_back(U("🔗 ") + link);
		}
	}
	const auto text = lines.join('\n');
	controller->show(Ui::MakeConfirmBox({
		.text = TextWithEntities{ text },
		.confirmed = [=](Fn<void()> close) {
			TextUtilities::SetClipboardText(TextForMimeData::Simple(text));
			close();
			controller->showToast(U("Скопировано"));
		},
		.confirmText = U("Копировать"),
		.cancelText = U("Закрыть"),
		.title = rpl::single(U("ID")),
	}));
}

} // namespace

void RegisterSettings() {
	S::Register({
		.key = u"commands.enabled"_q,
		.section = U("Быстрые команды"),
		.title = U("Команды .mute и .ban в чате"),
		.description = U("Напиши в поле ввода только команду и отправь, сообщение не уйдёт. С ответом на сообщение: .mute [время], .unmute, .ban, .unban, .kick, .del [N], .pin, .unpin, .id, .save. Без ответа: .mute глушит чат, .ban блокирует человека в личке, .del [N] удаляет твои последние сообщения, .id показывает ID чата. Время: 30m, 2h, 1d."),
		.kind = S::Kind::Bool,
		.defaultValue = true,
		.category = u"chats"_q,
		.hidden = true,
	});
	S::Register({
		.key = u"commands.aliases"_q,
		.section = U("Быстрые команды"),
		.title = U("Алиасы"),
		.kind = S::Kind::String,
		.defaultValue = QString(),
		.category = u"chats"_q,
		.hidden = true,
	});
}

bool Handle(
		not_null<Window::SessionController*> controller,
		not_null< ::History*> history,
		const TextWithTags &text,
		FullReplyTo replyTo) {
	if (!S::Bool(u"commands.enabled"_q)) {
		return false;
	}
	static const auto re = QRegularExpression(
		u"^\\.([\\p{L}\\d_]+)(?:\\s+(all|\\d+)\\s*([mhdwмчдн]?))?$"_q,
		QRegularExpression::CaseInsensitiveOption);
	const auto match = re.match(text.text.trimmed());
	if (!match.hasMatch()) {
		return false;
	}
	const auto command = ResolveAlias(match.captured(1).toLower());
	if (!KnownCommands().contains(command)) {
		return false;
	}
	const auto all = (match.captured(2).toLower() == u"all"_q);
	auto seconds = 0;
	if (!all && !match.captured(2).isEmpty()) {
		const auto count = match.captured(2).toInt();
		const auto unit = match.captured(3).toLower();
		seconds = count * ((unit == u"h"_q || unit == u"ч"_q) ? 3600
			: (unit == u"d"_q || unit == u"д"_q) ? 86400
			: (unit == u"w"_q || unit == u"н"_q) ? 7 * 86400
			: 60);
	}
	const auto peer = history->peer;
	const auto toast = [=](const QString &line) {
		controller->showToast(line);
	};

	auto target = (UserData*)nullptr;
	if (replyTo.messageId) {
		if (const auto item = history->owner().message(replyTo.messageId)) {
			const auto from = item->from();
			if (from->isUser() && !from->isSelf()) {
				target = from->asUser();
			}
		}
	}
	const auto channel = peer->asChannel();
	const auto chat = peer->asChat();
	auto replied = (HistoryItem*)nullptr;
	if (replyTo.messageId) {
		replied = history->owner().message(replyTo.messageId);
	}

	if (command == u"id"_q) {
		ShowInfo(controller, history, replied);
		return true;
	}
	if (command == u"save"_q) {
		if (!replied) {
			toast(U("Ответь командой на сообщение, которое сохранить"));
			return true;
		}
		const auto &session = peer->session();
		const auto saved = session.data().history(session.user());
		auto action = Api::SendAction(saved, Api::SendOptions());
		peer->session().api().sendAction(action);
		peer->session().api().forwardMessages({
			.items = { replied },
			.options = Data::ForwardOptions::PreserveInfo,
		}, action);
		toast(U("Сохранено в «Избранное»"));
		return true;
	}
	if (command == u"pin"_q || command == u"unpin"_q) {
		if (!replied) {
			toast(U("Ответь командой на сообщение"));
		} else if (!replied->canPin()) {
			toast(U("Нет прав закреплять здесь"));
		} else {
			Window::ToggleMessagePinned(
				controller,
				replied->fullId(),
				command == u"pin"_q);
		}
		return true;
	}
	if (command == u"del"_q) {
		auto ids = MessageIdsList();
		const auto limit = all
			? 1000000
			: std::clamp(
				match.captured(2).isEmpty() ? 1 : match.captured(2).toInt(),
				1,
				100);
		const auto byUser = replied ? replied->from().get() : nullptr;
		if (all && byUser && channel && channel->canDeleteMessages()) {
			const auto name = byUser->name();
			controller->show(Ui::MakeConfirmBox({
				.text = TextWithEntities{
					U("Удалить все сообщения «%1» в этом чате?").arg(name) },
				.confirmed = [=](Fn<void()> close) {
					channel->session().api().deleteAllFromParticipant(
						channel,
						byUser);
					close();
				},
				.confirmText = U("Удалить всё"),
				.cancelText = U("Отмена"),
				.title = rpl::single(U("Удаление")),
			}));
			return true;
		}
		if (replied && match.captured(2).isEmpty() && !all) {
			if (replied->canDelete()) {
				ids.push_back(replied->fullId());
			}
		} else {
			// Свои последние сообщения, либо последние сообщения человека
			// из ответа (если есть права).
			for (auto b = history->blocks.rbegin();
				b != history->blocks.rend() && int(ids.size()) < limit;
				++b) {
				for (auto m = (*b)->messages.rbegin();
					m != (*b)->messages.rend() && int(ids.size()) < limit;
					++m) {
					const auto item = (*m)->data();
					const auto mine = byUser
						? (item->from() == byUser)
						: item->out();
					if (mine && !item->isService() && item->canDelete()) {
						ids.push_back(item->fullId());
					}
				}
			}
		}
		if (ids.empty()) {
			toast(U("Нечего удалять или нет прав"));
			return true;
		}
		const auto count = int(ids.size());
		auto owner = &peer->owner();
		controller->show(Ui::MakeConfirmBox({
			.text = TextWithEntities{ U("Удалить сообщений: %1?").arg(count) },
			.confirmed = [=, ids = std::move(ids)](Fn<void()> close) {
				owner->histories().deleteMessages(ids, true);
				close();
			},
			.confirmText = U("Удалить"),
			.cancelText = U("Отмена"),
			.title = rpl::single(U("Удаление")),
		}));
		return true;
	}
	const auto isMute = (command == u"mute"_q);
	const auto isUnmute = (command == u"unmute"_q);
	const auto isBan = (command == u"ban"_q);

	if (isMute || isUnmute) {
		if (target && channel && channel->isMegagroup()
			&& channel->canRestrictParticipant(target)) {
			const auto until = (isMute && seconds > 0)
				? (base::unixtime::now() + seconds)
				: 0;
			const auto name = target->name();
			Api::ChatParticipants::Restrict(
				channel,
				target,
				ChatRestrictionsInfo(),
				isMute
					? ChatRestrictionsInfo(MuteFlags(), until)
					: ChatRestrictionsInfo(),
				[=] {
					toast(isMute
						? (name + U(": запрещено писать ") + Describe(seconds))
						: (name + U(": снова может писать")));
				},
				[=](const QString &error) {
					toast(U("Не получилось: ") + error);
				});
			return true;
		}
		auto value = Data::MuteValue();
		if (isUnmute) {
			value.unmute = true;
		} else if (seconds > 0) {
			value.period = seconds;
		} else {
			value.forever = true;
		}
		peer->owner().notifySettings().update(peer, value);
		toast(isMute
			? (U("Чат заглушен ") + Describe(seconds))
			: U("Звук чата включён"));
		return true;
	}

	if (command == u"kick"_q && target && channel
		&& channel->canBanMembers()) {
		peer->session().api().chatParticipants().kick(
			channel,
			target,
			ChatRestrictionsInfo());
		const auto name = target->name();
		base::call_delayed(2000, controller, [=] {
			channel->session().api().chatParticipants().unblock(
				channel,
				target);
			toast(name + U(": исключён, может вернуться"));
		});
		return true;
	}
	if (target && channel
		&& (channel->isMegagroup() || channel->isBroadcast())) {
		if (!channel->canBanMembers()) {
			toast(U("Нет прав исключать участников"));
		} else if (isBan) {
			peer->session().api().chatParticipants().kick(
				channel,
				target,
				ChatRestrictionsInfo());
			toast(target->name() + U(": исключён"));
		} else {
			peer->session().api().chatParticipants().unblock(channel, target);
			toast(target->name() + U(": разбанен"));
		}
		return true;
	}
	if (target && chat) {
		if ((isBan || command == u"kick"_q) && chat->canBanMembers()) {
			peer->session().api().chatParticipants().kick(chat, target);
			toast(target->name() + U(": исключён"));
		} else {
			toast(U("В обычной группе можно только исключить (.ban)"));
		}
		return true;
	}
	if (const auto user = peer->asUser(); user && !user->isSelf()) {
		if (isBan) {
			peer->session().api().blockedPeers().block(user);
			toast(user->name() + U(": заблокирован"));
		} else if (command == u"unban"_q) {
			peer->session().api().blockedPeers().unblock(user);
			toast(user->name() + U(": разблокирован"));
		} else {
			toast(U("Эта команда работает в группах, ответом на сообщение"));
		}
		return true;
	}
	toast(U("В группе ответь командой на сообщение человека"));
	return true;
}

void ShowCommandsBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(U("Быстрые команды")));
		box->setWidth(440);
		const auto content = box->verticalLayout();
		const auto enabled = content->add(
			object_ptr<Ui::Checkbox>(
				content,
				U("Включить команды"),
				S::Bool(u"commands.enabled"_q),
				st::defaultCheckbox),
			QMargins(22, 10, 22, 6));
		enabled->checkedChanges() | rpl::on_next([=](bool on) {
			S::SetBool(u"commands.enabled"_q, on);
		}, enabled->lifetime());
		content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				U("Напиши в поле ввода только команду и отправь: сообщение не уйдёт, команда выполнится."),
				st::boxLabel),
			QMargins(22, 4, 22, 8));
		Ui::AddSubsectionTitle(content, rpl::single(U("Команды")));
		content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				CommandsHelp(),
				st::boxLabel),
			QMargins(22, 4, 22, 8));
		Ui::AddSubsectionTitle(content, rpl::single(U("Алиасы")));
		content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				U("Свои короткие имена. По одной строке: имя = команда, например rm = del."),
				st::boxLabel),
			QMargins(22, 4, 22, 4));
		const auto field = content->add(
			object_ptr<Ui::InputField>(
				content,
				st::defaultInputField,
				Ui::InputField::Mode::MultiLine,
				rpl::single(U("Алиасы")),
				S::String(u"commands.aliases"_q)),
			QMargins(22, 4, 22, 8));
		box->addButton(rpl::single(U("Сохранить")), [=] {
			S::SetString(u"commands.aliases"_q, field->getLastText());
			box->closeBox();
		});
		box->addButton(rpl::single(U("Закрыть")), [=] { box->closeBox(); });
	}));
}

} // namespace Mokagram::Commands
