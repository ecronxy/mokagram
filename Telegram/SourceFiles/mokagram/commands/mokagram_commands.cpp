// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 57e0ff9e
#include "mokagram/commands/mokagram_commands.h"
#include "mokagram/lang/mokagram_lang.h"

#include "api/api_blocked_peers.h"
#include "api/api_text_entities.h"
#include "data/data_document.h"
#include "data/data_file_click_handler.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "core/application.h"
#include "ui/image/image.h"
#include "api/api_common.h"
#include "api/api_toggling_media.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_prepare.h"
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
#include "mokagram/notes/mokagram_notes.h"
#include "mokagram/snippets/mokagram_snippets.h"
#include "ui/widgets/fields/input_field.h"
#include "window/window_session_controller.h"

#include <QtCore/QProcess>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include <cmath>

namespace Mokagram::Commands {
namespace {

namespace S = Mokagram::Settings;

[[nodiscard]] QString U(const char *text) {
	return QString::fromUtf8(text);
}

[[nodiscard]] QString Describe(int seconds) {
	if (seconds <= 0) {
		return U(Mokagram::Lang::Tr("навсегда"));
	} else if (seconds % 86400 == 0) {
		return U(Mokagram::Lang::Tr("на %1 д")).arg(seconds / 86400);
	} else if (seconds % 3600 == 0) {
		return U(Mokagram::Lang::Tr("на %1 ч")).arg(seconds / 3600);
	}
	return U(Mokagram::Lang::Tr("на %1 мин")).arg(seconds / 60);
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
		u"tr"_q, u"calc"_q, u"archive"_q, u"unarchive"_q, u"dl"_q,
		u"tag"_q, u"note"_q, u"snip"_q, u"gif"_q,
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
		U(Mokagram::Lang::Tr(".unmute включает звук чата или снимает запрет писать")),
		U(Mokagram::Lang::Tr(".ban исключает человека (в группе, ответом) или блокирует его в личке")),
		U(Mokagram::Lang::Tr(".unban снимает бан или разблокирует человека")),
		U(Mokagram::Lang::Tr(".kick исключает человека, но он может вернуться (ответом, в группе)")),
		U(Mokagram::Lang::Tr(".del удаляет сообщение, на которое ответил")),
		U(Mokagram::Lang::Tr(".del N удаляет твои последние N сообщений (с ответом: N сообщений того человека)")),
		U(Mokagram::Lang::Tr(".del all с ответом удаляет все сообщения человека в группе, без ответа все твои загруженные")),
		U(Mokagram::Lang::Tr(".pin / .unpin закрепляет или открепляет сообщение (ответом)")),
		U(Mokagram::Lang::Tr(".gif превращает видео (ответом) в гиф без звука и сохраняет в «Мои GIF»")),
		U(Mokagram::Lang::Tr(".id показывает ID автора, чата и сообщения, ссылку на сообщение")),
		U(Mokagram::Lang::Tr(".save пересылает сообщение (ответом) в «Избранное»")),
	};
	const auto aliases = ParseAliases(S::String(u"commands.aliases"_q));
	if (!aliases.empty()) {
		lines.push_back(QString());
		lines.push_back(U(Mokagram::Lang::Tr("Твои алиасы:")));
		for (const auto &[alias, command] : aliases) {
			lines.push_back(U(".") + alias + U(" = .") + command);
		}
	}
	return lines.join('\n');
}

// Мини-калькулятор: + - * / ^ ( ), проценты, функции и константы.
class Calc final {
public:
	explicit Calc(QString text) : _s(std::move(text)) {
		_s.replace(',', '.').replace(QChar(0xD7), '*').replace(QChar(0xF7), '/');
		_s.remove(' ');
	}
	[[nodiscard]] bool run(double &out) {
		_ok = true;
		out = expr();
		return _ok && _i == _s.size() && std::isfinite(out);
	}

private:
	[[nodiscard]] QChar peek() const {
		return (_i < _s.size()) ? _s[_i] : QChar();
	}
	double expr() {
		auto v = term();
		while (peek() == '+' || peek() == '-') {
			const auto op = _s[_i++];
			const auto r = term();
			v = (op == '+') ? (v + r) : (v - r);
		}
		return v;
	}
	double term() {
		auto v = power();
		while (true) {
			if (peek() == '*' && _i + 1 < _s.size() && _s[_i + 1] == '*') {
				break;
			} else if (peek() == '*') {
				++_i;
				v *= power();
			} else if (peek() == '/') {
				++_i;
				const auto r = power();
				if (r == 0) {
					_ok = false;
				}
				v = (r == 0) ? 0 : (v / r);
			} else {
				break;
			}
		}
		return v;
	}
	double power() {
		auto v = unary();
		if (peek() == '^' || (peek() == '*' && _i + 1 < _s.size() && _s[_i + 1] == '*')) {
			_i += (peek() == '^') ? 1 : 2;
			v = std::pow(v, power());
		}
		return v;
	}
	double unary() {
		if (peek() == '-') {
			++_i;
			return -unary();
		} else if (peek() == '+') {
			++_i;
			return unary();
		}
		return postfix();
	}
	double postfix() {
		auto v = atom();
		while (peek() == '%') {
			++_i;
			v /= 100.;
		}
		return v;
	}
	double atom() {
		if (peek() == '(') {
			++_i;
			const auto v = expr();
			if (peek() != ')') {
				_ok = false;
			} else {
				++_i;
			}
			return v;
		}
		if (peek().isDigit() || peek() == '.') {
			const auto start = _i;
			while (peek().isDigit() || peek() == '.') {
				++_i;
			}
			auto good = false;
			const auto v = _s.mid(start, _i - start).toDouble(&good);
			_ok = _ok && good;
			return v;
		}
		if (peek().isLetter()) {
			const auto start = _i;
			while (peek().isLetter()) {
				++_i;
			}
			const auto name = _s.mid(start, _i - start).toLower();
			if (name == u"pi"_q) {
				return M_PI;
			} else if (name == u"e"_q) {
				return M_E;
			}
			if (peek() != '(') {
				_ok = false;
				return 0;
			}
			++_i;
			const auto v = expr();
			if (peek() != ')') {
				_ok = false;
			} else {
				++_i;
			}
			if (name == u"sqrt"_q) return std::sqrt(v);
			if (name == u"abs"_q) return std::abs(v);
			if (name == u"sin"_q) return std::sin(v);
			if (name == u"cos"_q) return std::cos(v);
			if (name == u"tan"_q) return std::tan(v);
			if (name == u"ln"_q) return std::log(v);
			if (name == u"log"_q) return std::log10(v);
			if (name == u"floor"_q) return std::floor(v);
			if (name == u"ceil"_q) return std::ceil(v);
			if (name == u"round"_q) return std::round(v);
			_ok = false;
			return 0;
		}
		_ok = false;
		return 0;
	}

	QString _s;
	int _i = 0;
	bool _ok = true;

};

[[nodiscard]] QString FormatNumber(double v) {
	if (std::abs(v - std::round(v)) < 1e-9 && std::abs(v) < 1e15) {
		return QString::number(qint64(std::llround(v)));
	}
	return QString::number(v, 'g', 12);
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
	lines.push_back(U(Mokagram::Lang::Tr("ℹ️ Информация")));
	lines.push_back(QString());
	lines.push_back(U(Mokagram::Lang::Tr("👤 ID автора: ")) + (author
		? QString::number(author->id.value & PeerId::kChatTypeMask)
		: U(Mokagram::Lang::Tr("неизвестен"))));
	if (author && !author->username().isEmpty()) {
		lines.push_back(U(Mokagram::Lang::Tr("🔖 Ник: @")) + author->username());
	}
	lines.push_back(U(Mokagram::Lang::Tr("📢 ID чата: ")) + QString::number(
		peer->id.value & PeerId::kChatTypeMask)
		+ (peer->isChannel() ? U(" (-100…)") : QString()));
	if (item) {
		lines.push_back(U(Mokagram::Lang::Tr("💬 ID сообщения: ")) + QString::number(item->id.bare));
		const auto link = MessageLink(item);
		if (!link.isEmpty()) {
			lines.push_back(QString());
			lines.push_back(U("🔗 ") + link);
		}
	}
	const auto text = lines.join('\n');
	QGuiApplication::clipboard()->setText(text);
	controller->showToast(U(Mokagram::Lang::Tr("Скопировано. ID автора: "))
		+ (author
			? QString::number(author->id.value & PeerId::kChatTypeMask)
			: U(Mokagram::Lang::Tr("неизвестен"))));
}

} // namespace

void RegisterSettings() {
	S::Register({
		.key = u"commands.enabled"_q,
		.section = U(Mokagram::Lang::Tr("Быстрые команды")),
		.title = U(Mokagram::Lang::Tr("Команды .mute и .ban в чате")),
		.description = U(Mokagram::Lang::Tr("Напиши в поле ввода только команду и отправь, сообщение не уйдёт. С ответом на сообщение: .mute [время], .unmute, .ban, .unban, .kick, .del [N], .pin, .unpin, .id, .save. Без ответа: .mute глушит чат, .ban блокирует человека в личке, .del [N] удаляет твои последние сообщения, .id показывает ID чата. Время: 30m, 2h, 1d.")),
		.kind = S::Kind::Bool,
		.defaultValue = true,
		.category = u"chats"_q,
		.hidden = true,
	});
	S::Register({
		.key = u"commands.aliases"_q,
		.section = U(Mokagram::Lang::Tr("Быстрые команды")),
		.title = U(Mokagram::Lang::Tr("Алиасы")),
		.kind = S::Kind::String,
		.defaultValue = QString(),
		.category = u"chats"_q,
		.hidden = true,
	});
}

// .gif: видео из реплая -> mp4 без звука -> отправка как гиф -> «Мои GIF».
void WatchSentGif(
		not_null<Window::SessionController*> controller,
		not_null< ::History*> history,
		TimeId since,
		int left) {
	const auto weak = base::make_weak(history.get());
	QTimer::singleShot(1000, [=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto item = strong->lastMessage();
		if (item
			&& item->out()
			&& !item->isSending()
			&& item->date() >= since
			&& item->media()
			&& item->media()->document()
			&& item->media()->document()->isGifv()) {
			Api::ToggleSavedGif(
				controller->uiShow(),
				item->media()->document(),
				item->fullId(),
				true);
			controller->showToast(
				U(Mokagram::Lang::Tr("Гиф добавлена в «Мои GIF»")));
		} else if (left > 0) {
			WatchSentGif(controller, strong, since, left - 1);
		}
	});
}

void ConvertAndSendGif(
		not_null<Window::SessionController*> controller,
		not_null< ::History*> history,
		const QString &source,
		FullReplyTo replyTo) {
	const auto output = QStandardPaths::writableLocation(
		QStandardPaths::TempLocation)
		+ u"/mokagram_gif_"_q
		+ QString::number(base::unixtime::now())
		+ u".mp4"_q;
	const auto process = new QProcess(qApp);
	QObject::connect(process, &QProcess::finished, [=](int code) {
		process->deleteLater();
		if (code != 0 || !QFileInfo(output).size()) {
			controller->showToast(
				U(Mokagram::Lang::Tr("Не получилось сконвертировать видео")));
			return;
		}
		auto list = Storage::PrepareMediaList(
			QStringList{ output },
			st::sendMediaPreviewSize,
			history->session().premium());
		if (list.error != Ui::PreparedList::Error::None) {
			controller->showToast(
				U(Mokagram::Lang::Tr("Не получилось отправить гиф")));
			return;
		}
		const auto since = base::unixtime::now();
		auto action = Api::SendAction(history, Api::SendOptions());
		action.replyTo = replyTo;
		action.clearDraft = false;
		history->session().api().sendFiles(
			std::move(list),
			SendMediaType::Photo,
			nullptr,
			std::move(action));
		WatchSentGif(controller, history, since - 1, 90);
	});
	QObject::connect(process, &QProcess::errorOccurred, [=](
			QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart) {
			process->deleteLater();
			controller->showToast(
				U(Mokagram::Lang::Tr("Нужен ffmpeg в системе")));
		}
	});
	process->start(u"ffmpeg"_q, {
		u"-y"_q, u"-i"_q, source,
		u"-an"_q,
		u"-c:v"_q, u"libx264"_q,
		u"-pix_fmt"_q, u"yuv420p"_q,
		u"-crf"_q, u"26"_q,
		u"-vf"_q, u"scale='min(720,iw)':-2"_q,
		u"-movflags"_q, u"+faststart"_q,
		output,
	});
}

void WaitForVideoAndConvert(
		not_null<Window::SessionController*> controller,
		not_null< ::History*> history,
		not_null<DocumentData*> document,
		FullMsgId itemId,
		FullReplyTo replyTo,
		int left) {
	const auto path = document->filepath(true);
	if (!path.isEmpty()) {
		ConvertAndSendGif(controller, history, path, replyTo);
		return;
	}
	if (left <= 0) {
		controller->showToast(
			U(Mokagram::Lang::Tr("Не получилось скачать видео")));
		return;
	}
	const auto weak = base::make_weak(history.get());
	QTimer::singleShot(700, [=] {
		const auto strong = weak.get();
		const auto item = strong
			? strong->owner().message(itemId)
			: nullptr;
		if (!item
			|| !item->media()
			|| item->media()->document() != document) {
			return;
		}
		WaitForVideoAndConvert(
			controller,
			strong,
			document,
			itemId,
			replyTo,
			left - 1);
	});
}

bool Handle(
		not_null<Window::SessionController*> controller,
		not_null< ::History*> history,
		const TextWithTags &text,
		FullReplyTo replyTo,
		TextWithTags *replacement) {
	if (!S::Bool(u"commands.enabled"_q)) {
		return false;
	}
	static const auto re = QRegularExpression(
		u"^\\.([\\p{L}\\d_]+)(?:\\s+(.+))?$"_q,
		QRegularExpression::CaseInsensitiveOption
			| QRegularExpression::DotMatchesEverythingOption);
	const auto match = re.match(text.text.trimmed());
	if (!match.hasMatch()) {
		return false;
	}
	const auto command = ResolveAlias(match.captured(1).toLower());
	if (!KnownCommands().contains(command)) {
		return false;
	}
	const auto arg = match.captured(2).trimmed();
	static const auto numberRe = QRegularExpression(
		u"^(\\d+)\\s*([mhdwмчдн]?)$"_q,
		QRegularExpression::CaseInsensitiveOption);
	const auto number = numberRe.match(arg);
	const auto all = (arg.toLower() == u"all"_q);
	const auto freeText = (command == u"tr"_q)
		|| (command == u"calc"_q)
		|| (command == u"tag"_q)
		|| (command == u"note"_q)
		|| (command == u"snip"_q);
	if (!freeText && !arg.isEmpty() && !all && !number.hasMatch()) {
		// Обычная фраза, которая случайно начинается с команды.
		return false;
	}
	const auto hasNumber = number.hasMatch();
	const auto numberValue = hasNumber ? number.captured(1).toInt() : 0;
	auto seconds = 0;
	if (hasNumber) {
		const auto unit = number.captured(2).toLower();
		seconds = numberValue * ((unit == u"h"_q || unit == u"ч"_q) ? 3600
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

	if (command == u"calc"_q) {
		auto value = 0.;
		if (arg.isEmpty() || !Calc(arg).run(value)) {
			toast(U(Mokagram::Lang::Tr("Не получилось посчитать")));
			if (replacement) {
				*replacement = text;
			}
			return true;
		}
		if (replacement) {
			*replacement = TextWithTags{ arg + u" = "_q + FormatNumber(value) };
		}
		return true;
	}
	if (command == u"archive"_q || command == u"unarchive"_q) {
		Window::ToggleHistoryArchived(
			controller->uiShow(),
			history,
			(command == u"archive"_q));
		return true;
	}
	if (command == u"tag"_q || command == u"note"_q) {
		const auto isTag = (command == u"tag"_q);
		if (isTag) {
			Notes::SetTag(peer->id, arg);
		} else {
			Notes::SetNote(peer->id, arg);
		}
		history->updateChatListEntry();
		toast(arg.isEmpty()
			? U(isTag ? Mokagram::Lang::Tr("Тег убран") : Mokagram::Lang::Tr("Заметка убрана"))
			: U(isTag ? Mokagram::Lang::Tr("Тег сохранён") : Mokagram::Lang::Tr("Заметка сохранена")));
		return true;
	}
	if (command == u"snip"_q) {
		const auto split = arg.indexOf(QRegularExpression(u"\\s"_q));
		const auto first = (split < 0) ? arg : arg.left(split);
		const auto rest = (split < 0) ? QString() : arg.mid(split).trimmed();
		static const auto nameRe = QRegularExpression(
			u"^[\\p{L}\\d_]{1,24}$"_q);
		if (first.isEmpty() || first.toLower() == u"list"_q) {
			const auto names = Snippets::Names();
			toast(names.isEmpty()
				? U(Mokagram::Lang::Tr("Сниппетов нет. Пример: .snip привет Здравствуйте!"))
				: (U(Mokagram::Lang::Tr("Сниппеты: ;")) + names.join(u", ;"_q)));
		} else if (first.toLower() == u"del"_q) {
			toast(Snippets::Remove(rest)
				? U(Mokagram::Lang::Tr("Сниппет удалён"))
				: U(Mokagram::Lang::Tr("Такого сниппета нет")));
		} else if (!nameRe.match(first).hasMatch()) {
			toast(U(Mokagram::Lang::Tr("Имя сниппета: буквы, цифры и _")));
		} else if (rest.isEmpty()) {
			const auto value = Snippets::Get(first);
			toast(value.text.isEmpty()
				? U(Mokagram::Lang::Tr("Такого сниппета нет. Пример: .snip ")) + first + U(Mokagram::Lang::Tr(" текст"))
				: value.text.left(200));
		} else {
			// Берём текст вместе с разметкой и премиум-эмодзи из поля ввода.
			const auto at = text.text.lastIndexOf(rest);
			Snippets::Set(
				first,
				(at >= 0)
					? Snippets::Slice(text, at, rest.size())
					: TextWithTags{ rest });
			toast(U(Mokagram::Lang::Tr("Сохранено. Пиши ;")) + first.toLower() + U(Mokagram::Lang::Tr(" в тексте")));
		}
		return true;
	}
	if (command == u"gif"_q) {
		const auto document = (replied && replied->media())
			? replied->media()->document()
			: nullptr;
		if (!document
			|| !(document->isVideoFile() || document->isAnimation())) {
			toast(U(Mokagram::Lang::Tr("Ответь командой на видео")));
			return true;
		}
		toast(U(Mokagram::Lang::Tr("Делаю гиф…")));
		if (document->filepath(true).isEmpty()) {
			document->save(
				replied->fullId(),
				QStandardPaths::writableLocation(
					QStandardPaths::TempLocation)
				+ u"/mokagram_gif_src_"_q
				+ QString::number(document->id)
				+ u".mp4"_q);
		}
		WaitForVideoAndConvert(
			controller,
			history,
			document,
			replied->fullId(),
			replyTo,
			170);
		return true;
	}
	if (command == u"dl"_q) {
		if (!replied || !replied->media()) {
			toast(U(Mokagram::Lang::Tr("Ответь командой на сообщение с файлом или фото")));
			return true;
		}
		const auto media = replied->media();
		if (const auto document = media->document()) {
			DocumentSaveClickHandler::SaveAndTrack(
				replied->fullId(),
				document,
				DocumentSaveClickHandler::Mode::ToFile);
			toast(U(Mokagram::Lang::Tr("Сохраняю в загрузки")));
		} else if (const auto photo = media->photo()) {
			const auto itemId = replied->fullId();
			const auto view = photo->createMediaView();
			photo->load(itemId);
			const auto folder = QStandardPaths::writableLocation(
				QStandardPaths::DownloadLocation);
			const auto attempt = std::make_shared<Fn<void(int)>>();
			*attempt = [=](int left) {
				const auto image = view->image(Data::PhotoSize::Large);
				if (image && !image->isNull()) {
					const auto path = folder + u"/photo_"_q
						+ QString::number(photo->id) + u".jpg"_q;
					toast(image->original().save(path, "JPG", 95)
						? (U(Mokagram::Lang::Tr("Сохранено: ")) + path)
						: U(Mokagram::Lang::Tr("Не получилось сохранить")));
				} else if (left > 0) {
					QTimer::singleShot(700, [=] { (*attempt)(left - 1); });
				} else {
					toast(U(Mokagram::Lang::Tr("Не получилось скачать фото")));
				}
			};
			(*attempt)(8);
		} else {
			toast(U(Mokagram::Lang::Tr("В этом сообщении нечего сохранять")));
		}
		return true;
	}
	if (command == u"tr"_q) {
		const auto space = arg.indexOf(QRegularExpression(u"\\s"_q));
		const auto lang = (space < 0 ? arg : arg.left(space)).toLower();
		auto source = (space < 0) ? QString() : arg.mid(space + 1).trimmed();
		static const auto langRe = QRegularExpression(
			u"^[a-z]{2,3}(-[a-z]{2,4})?$"_q);
		if (!langRe.match(lang).hasMatch()) {
			toast(U(Mokagram::Lang::Tr("Укажи язык: .tr en")));
			if (replacement) {
				*replacement = text;
			}
			return true;
		}
		auto entities = EntitiesInText();
		if (source.isEmpty() && replied) {
			source = replied->originalText().text;
			entities = replied->originalText().entities;
		}
		if (source.isEmpty()) {
			toast(U(Mokagram::Lang::Tr("Напиши текст после языка или ответь на сообщение")));
			if (replacement) {
				*replacement = text;
			}
			return true;
		}
		const auto session = &peer->session();
		auto list = QVector<MTPTextWithEntities>();
		list.push_back(MTP_textWithEntities(
			MTP_string(source),
			Api::EntitiesToMTP(
				session,
				entities,
				Api::ConvertOption::SkipLocal)));
		toast(U(Mokagram::Lang::Tr("Перевожу…")));
		session->api().request(MTPmessages_TranslateText(
			MTP_flags(MTPmessages_TranslateText::Flag::f_text),
			MTP_inputPeerEmpty(),
			MTPVector<MTPint>(),
			MTP_vector<MTPTextWithEntities>(list),
			MTP_string(lang),
			MTPstring()
		)).done([=](const MTPmessages_TranslatedText &result) {
			const auto &v = result.data().vresult().v;
			if (v.isEmpty()) {
				toast(U(Mokagram::Lang::Tr("Перевод не получился")));
				return;
			}
			const auto translated = Api::ParseTextWithEntities(
				session,
				v.front());
			auto action = Api::SendAction(history, Api::SendOptions());
			action.replyTo = replyTo;
			auto message = Api::MessageToSend(std::move(action));
			message.textWithTags = TextWithTags{
				translated.text,
				TextUtilities::ConvertEntitiesToTextTags(translated.entities),
			};
			session->api().sendMessage(std::move(message));
		}).fail([=](const MTP::Error &error) {
			toast(U(Mokagram::Lang::Tr("Не получилось перевести: ")) + error.type());
		}).send();
		return true;
	}
	if (command == u"id"_q) {
		ShowInfo(controller, history, replied);
		return true;
	}
	if (command == u"save"_q) {
		if (!replied) {
			toast(U(Mokagram::Lang::Tr("Ответь командой на сообщение, которое сохранить")));
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
		toast(U(Mokagram::Lang::Tr("Сохранено в «Избранное»")));
		return true;
	}
	if (command == u"pin"_q || command == u"unpin"_q) {
		if (!replied) {
			toast(U(Mokagram::Lang::Tr("Ответь командой на сообщение")));
		} else if (!replied->canPin()) {
			toast(U(Mokagram::Lang::Tr("Нет прав закреплять здесь")));
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
				hasNumber ? numberValue : 1,
				1,
				100);
		const auto byUser = replied ? replied->from().get() : nullptr;
		if (all && byUser && channel && channel->canDeleteMessages()) {
			const auto name = byUser->name();
			controller->show(Ui::MakeConfirmBox({
				.text = TextWithEntities{
					U(Mokagram::Lang::Tr("Удалить все сообщения «%1» в этом чате?")).arg(name) },
				.confirmed = [=](Fn<void()> close) {
					channel->session().api().deleteAllFromParticipant(
						channel,
						byUser);
					close();
				},
				.confirmText = U(Mokagram::Lang::Tr("Удалить всё")),
				.cancelText = U(Mokagram::Lang::Tr("Отмена")),
				.title = rpl::single(U(Mokagram::Lang::Tr("Удаление"))),
			}));
			return true;
		}
		if (replied && !hasNumber && !all) {
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
			toast(U(Mokagram::Lang::Tr("Нечего удалять или нет прав")));
			return true;
		}
		const auto count = int(ids.size());
		auto owner = &peer->owner();
		controller->show(Ui::MakeConfirmBox({
			.text = TextWithEntities{ U(Mokagram::Lang::Tr("Удалить сообщений: %1?")).arg(count) },
			.confirmed = [=, ids = std::move(ids)](Fn<void()> close) {
				owner->histories().deleteMessages(ids, true);
				close();
			},
			.confirmText = U(Mokagram::Lang::Tr("Удалить")),
			.cancelText = U(Mokagram::Lang::Tr("Отмена")),
			.title = rpl::single(U(Mokagram::Lang::Tr("Удаление"))),
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
						? (name + U(Mokagram::Lang::Tr(": запрещено писать ")) + Describe(seconds))
						: (name + U(Mokagram::Lang::Tr(": снова может писать"))));
				},
				[=](const QString &error) {
					toast(U(Mokagram::Lang::Tr("Не получилось: ")) + error);
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
			? (U(Mokagram::Lang::Tr("Чат заглушен ")) + Describe(seconds))
			: U(Mokagram::Lang::Tr("Звук чата включён")));
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
			toast(name + U(Mokagram::Lang::Tr(": исключён, может вернуться")));
		});
		return true;
	}
	if (target && channel
		&& (channel->isMegagroup() || channel->isBroadcast())) {
		if (!channel->canBanMembers()) {
			toast(U(Mokagram::Lang::Tr("Нет прав исключать участников")));
		} else if (isBan) {
			peer->session().api().chatParticipants().kick(
				channel,
				target,
				ChatRestrictionsInfo());
			toast(target->name() + U(Mokagram::Lang::Tr(": исключён")));
		} else {
			peer->session().api().chatParticipants().unblock(channel, target);
			toast(target->name() + U(Mokagram::Lang::Tr(": разбанен")));
		}
		return true;
	}
	if (target && chat) {
		if ((isBan || command == u"kick"_q) && chat->canBanMembers()) {
			peer->session().api().chatParticipants().kick(chat, target);
			toast(target->name() + U(Mokagram::Lang::Tr(": исключён")));
		} else {
			toast(U(Mokagram::Lang::Tr("В обычной группе можно только исключить (.ban)")));
		}
		return true;
	}
	if (const auto user = peer->asUser(); user && !user->isSelf()) {
		if (isBan) {
			peer->session().api().blockedPeers().block(user);
			toast(user->name() + U(Mokagram::Lang::Tr(": заблокирован")));
		} else if (command == u"unban"_q) {
			peer->session().api().blockedPeers().unblock(user);
			toast(user->name() + U(Mokagram::Lang::Tr(": разблокирован")));
		} else {
			toast(U(Mokagram::Lang::Tr("Эта команда работает в группах, ответом на сообщение")));
		}
		return true;
	}
	toast(U(Mokagram::Lang::Tr("В группе ответь командой на сообщение человека")));
	return true;
}

std::vector<std::pair<QString, QString>> Aliases() {
	return ParseAliases(S::String(u"commands.aliases"_q));
}

void SetAliases(const std::vector<std::pair<QString, QString>> &list) {
	auto lines = QStringList();
	for (const auto &[alias, command] : list) {
		lines.push_back(alias + u" = "_q + command);
	}
	S::SetString(u"commands.aliases"_q, lines.join('\n'));
}

QStringList CommandNames() {
	return KnownCommands();
}

} // namespace Mokagram::Commands
