// Mokagram: SDK плагинов на стороне клиента (мост к Qt) и входы хуков обновлений.
#include "mokagram/plugins/mokagram_plugin_api.h"

#include "mokagram/lang/mokagram_lang.h"
#include "mokagram/plugins/mokagram_plugins.h"
#include "mokagram/plugins/mokagram_plugin_install.h"
#include "mokagram/plugins/mokagram_plugin_store.h"
#include "mokagram/settings/mokagram_settings.h"

#include "api/api_common.h"
#include "data/data_histories.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/view/history_view_element.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_prepare.h"
#include "storage/localimageloader.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/toast/toast.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "api/api_editing.h"
#include "core/click_handler_types.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_drafts.h"
#include "data/data_user.h"
#include "data/data_file_origin.h"
#include "history/history_item_components.h"
#include "ui/image/image.h"
#include "history/history_item_reply_markup.h"
#include "data/data_media_types.h"
#include "data/data_photo_media.h"
#include "data/data_photo.h"
#include "data/data_document.h"
#include "data/stickers/data_custom_emoji.h"
#include "history/history_item.h"
#include "ui/text/text_utilities.h"
#include "apiwrap.h"
#include "core/application.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "data/data_changes.h"
#include "data/data_peer_values.h"
#include "base/unixtime.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "mtproto/details/mtproto_serialized_request.h"
#include "mtproto/mtp_instance.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>
#include <QtCore/QTimer>

#include <algorithm>
#include <map>
#include <set>
#include <mutex>
#include <atomic>
#include <chrono>
#include <future>

namespace Mokagram::Plugins {
namespace {

rpl::lifetime BridgeLifetime;

[[nodiscard]] QString U(const char *text) {
	return QString::fromUtf8(text);
}

[[nodiscard]] std::string Reply(bool ok, const QString &error = {}) {
	auto result = QJsonObject();
	result.insert("ok", ok);
	if (!error.isEmpty()) {
		result.insert("error", error);
	}
	return QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
}

[[nodiscard]] bool IsMainThread() {
	return QThread::currentThread() == QCoreApplication::instance()->thread();
}

[[nodiscard]] Main::Session *ActiveSession() {
	// До появления аккаунтов activeAccount() падает на проверке: плагин может спросить раньше.
	if (Core::App().domain().accounts().empty()) {
		return nullptr;
	}
	return Core::App().activeAccount().maybeSession();
}

// Выполняет работу в главном потоке. Из потока плагина ждёт результат,
// но не дольше нескольких секунд и не во время выгрузки (главный поток может ждать нас).
template <typename Callback>
[[nodiscard]] std::string OnMain(Callback callback) {
	if (IsMainThread()) {
		return callback();
	}
	auto promise = std::make_shared<std::promise<std::string>>();
	auto future = promise->get_future();
	crl::on_main([=]() mutable {
		promise->set_value(callback());
	});
	for (auto waited = 0; waited != 50; ++waited) {
		if (future.wait_for(std::chrono::milliseconds(100))
			== std::future_status::ready) {
			return future.get();
		}
		if (Stopping()) {
			break;
		}
	}
	return Reply(false, U("клиент не ответил вовремя"));
}

std::string ShowToast(const QJsonObject &args) {
	const auto text = args.value("text").toString().left(200);
	const auto duration = args.value("duration").toDouble(2.5);
	return OnMain([=]() -> std::string {
		if (const auto window = Core::App().activeWindow()) {
			if (const auto controller = window->sessionController()) {
				controller->showToast(
					text,
					crl::time(std::clamp(duration, 1., 10.) * 1000));
				return Reply(true);
			}
		}
		LOG(("[mokagram] toast без окна: %1").arg(text));
		return Reply(false, U("нет открытого окна"));
	});
}

// ---- форматирование: сущности <-> JSON ----

[[nodiscard]] EntitiesInText EntitiesFromJson(
		const QJsonArray &list,
		int textLength,
		bool localOnly = false) {
	auto result = EntitiesInText();
	for (const auto &value : list) {
		const auto o = value.toObject();
		const auto type = o.value("type").toString();
		const auto offset = o.value("offset").toInt();
		const auto length = o.value("length").toInt();
		if (offset < 0 || length <= 0 || offset + length > textLength) {
			continue;
		}
		auto kind = EntityType::Invalid;
		auto data = QString();
		if (type == u"bold"_q) {
			kind = EntityType::Bold;
		} else if (type == u"italic"_q) {
			kind = EntityType::Italic;
		} else if (type == u"underline"_q) {
			kind = EntityType::Underline;
		} else if (type == u"strike"_q) {
			kind = EntityType::StrikeOut;
		} else if (type == u"code"_q) {
			kind = EntityType::Code;
		} else if (type == u"pre"_q) {
			kind = EntityType::Pre;
			data = o.value("language").toString().left(32);
		} else if (type == u"spoiler"_q) {
			kind = EntityType::Spoiler;
		} else if (type == u"quote"_q) {
			kind = EntityType::Blockquote;
		} else if (type == u"link"_q) {
			kind = EntityType::CustomUrl;
			data = o.value("url").toString().left(2048);
			if (data.isEmpty()) {
				continue;
			}
		} else if (type == u"userpic"_q) {
			// Аватарка пользователя прямо в тексте: рисуется только у вас, на сервер не уходит.
			if (!localOnly) {
				continue;
			}
			const auto session = ActiveSession();
			const auto id = PeerId(o.value("user_id").toString().toULongLong());
			const auto peer = (session && id)
				? session->data().peerLoaded(id)
				: nullptr;
			if (!peer) {
				continue;
			}
			kind = EntityType::CustomEmoji;
			data = session->data().customEmojiManager().peerUserpicEmojiData(peer);
		} else if (type == u"custom_emoji"_q) {
			kind = EntityType::CustomEmoji;
			const auto id = o.value("document_id").toString().toULongLong();
			if (!id) {
				continue;
			}
			data = Data::SerializeCustomEmojiId(DocumentId(id));
		} else {
			continue;
		}
		result.push_back(EntityInText(kind, offset, length, data));
		if (result.size() >= 200) {
			break;
		}
	}
	return result;
}

[[nodiscard]] QJsonArray ButtonsJson(not_null<const HistoryItem*> item) {
	auto result = QJsonArray();
	const auto markup = item->inlineReplyMarkup();
	if (!markup) {
		return result;
	}
	using Type = HistoryMessageMarkupButton::Type;
	for (const auto &row : markup->data.rows) {
		auto line = QJsonArray();
		for (const auto &button : row) {
			auto o = QJsonObject();
			o.insert("text", button.text);
			if (button.type == Type::Callback) {
				o.insert("kind", "callback");
				o.insert("data", QString::fromLatin1(button.data.toBase64()));
			} else if (button.type == Type::Url) {
				o.insert("kind", "url");
				o.insert("url", QString::fromUtf8(button.data));
			} else {
				o.insert("kind", "other");
			}
			line.push_back(o);
		}
		if (!line.isEmpty()) {
			result.push_back(line);
		}
	}
	return result;
}

[[nodiscard]] QJsonArray EntitiesToJson(
		const EntitiesInText &list,
		const QString &text = QString()) {
	auto result = QJsonArray();
	for (const auto &entity : list) {
		auto name = QString();
		auto o = QJsonObject();
		switch (entity.type()) {
		case EntityType::Bold: name = u"bold"_q; break;
		case EntityType::Italic: name = u"italic"_q; break;
		case EntityType::Underline: name = u"underline"_q; break;
		case EntityType::StrikeOut: name = u"strike"_q; break;
		case EntityType::Code: name = u"code"_q; break;
		case EntityType::Pre:
			name = u"pre"_q;
			o.insert("language", entity.data());
			break;
		case EntityType::Spoiler: name = u"spoiler"_q; break;
		case EntityType::Blockquote: name = u"quote"_q; break;
		case EntityType::CustomUrl:
			name = u"link"_q;
			o.insert("url", entity.data());
			break;
		case EntityType::Url: name = u"url"_q; break;
		case EntityType::Mention: {
			name = u"mention"_q;
			// Если пользователь уже известен клиенту, отдаём его id.
			const auto session = ActiveSession();
			const auto username = text.mid(entity.offset() + 1, entity.length() - 1);
			if (session && !username.isEmpty()) {
				if (const auto peer = session->data().peerByUsername(username)) {
					o.insert("user_id", QString::number(peer->id.value));
				}
			}
		} break;
		case EntityType::MentionName: {
			name = u"mention_name"_q;
			o.insert("user_id", entity.data().section('.', 0, 0));
		} break;
		case EntityType::Hashtag: name = u"hashtag"_q; break;
		case EntityType::BotCommand: name = u"bot_command"_q; break;
		case EntityType::CustomEmoji:
			name = u"custom_emoji"_q;
			o.insert("document_id", QString::number(
				Data::ParseCustomEmojiData(entity.data())));
			break;
		default: continue;
		}
		o.insert("type", name);
		o.insert("offset", entity.offset());
		o.insert("length", entity.length());
		result.push_back(o);
	}
	return result;
}

std::string SendMessage(const QJsonObject &args) {
	const auto peerId = PeerId(args.value("peer").toString().toULongLong());
	const auto text = args.value("text").toString();
	const auto replyTo = int64(args.value("reply_to").toDouble());
	const auto silent = args.value("silent").toBool();
	const auto entities = args.value("entities").toArray();
	if (!peerId || text.isEmpty()) {
		return Reply(false, U("неверные параметры"));
	}
	return OnMain([=]() -> std::string {
		const auto session = ActiveSession();
		if (!session) {
			return Reply(false, U("нет открытой сессии"));
		}
		// Только в чаты, которые уже есть у пользователя: не спам по чужим id.
		const auto history = session->data().historyLoaded(peerId);
		if (!history) {
			return Reply(false, U("чата нет среди загруженных диалогов"));
		}
		// Отправка не из стека вызова плагина: хук может быть внутри ApiWrap::sendMessage.
		const auto weak = base::make_weak(session);
		crl::on_main([=] {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			const auto history = strong->data().historyLoaded(peerId);
			if (!history) {
				return;
			}
			auto message = Api::MessageToSend(Api::SendAction(history));
			message.textWithTags.text = text;
			if (!entities.isEmpty()) {
				message.textWithTags.tags = TextUtilities::ConvertEntitiesToTextTags(
					EntitiesFromJson(entities, text.size()));
			}
			message.action.options.silent = silent;
			if (replyTo > 0) {
				message.action.replyTo.messageId = FullMsgId(
					peerId,
					MsgId(replyTo));
			}
			MarkFromPlugin(true);
			strong->api().sendMessage(std::move(message));
			MarkFromPlugin(false);
		});
		return Reply(true);
	});
}

[[nodiscard]] const Settings::Entry *FindEntry(const QString &key) {
	for (const auto &entry : Settings::Entries()) {
		if (entry.key == key) {
			return &entry;
		}
	}
	return nullptr;
}

[[nodiscard]] QJsonValue CurrentValue(const Settings::Entry &entry) {
	switch (entry.kind) {
	case Settings::Kind::Bool: return Settings::Bool(entry.key);
	case Settings::Kind::Int:
	case Settings::Kind::Choice: return Settings::Int(entry.key);
	default: return Settings::String(entry.key);
	}
}

void PluginLog(const std::string &line);

std::string RegisterSettings(const QJsonObject &args) {
	return OnMain([=]() -> std::string {
		for (const auto &value : args.value("entries").toArray()) {
			const auto item = value.toObject();
			const auto key = item.value("key").toString();
			if (!key.startsWith(U("plugin.")) || key.size() > 200) {
				continue;
			}
			auto entry = Settings::Entry();
			entry.key = key;
			entry.section = item.value("section").toString();
			entry.title = item.value("title").toString();
			entry.description = item.value("description").toString();
			entry.defaultValue = item.value("default");
			const auto kind = item.value("kind").toString();
			if (kind == "bool") {
				entry.kind = Settings::Kind::Bool;
			} else if (kind == "choice") {
				entry.kind = Settings::Kind::Choice;
				for (const auto &choice : item.value("choices").toArray()) {
					entry.choices.push_back(choice.toString());
				}
				entry.minValue = 0;
				entry.maxValue = std::max(int(entry.choices.size()) - 1, 0);
			} else if (kind == "string") {
				entry.kind = Settings::Kind::String;
			} else if (kind == "button") {
				entry.kind = Settings::Kind::String;
				entry.icon = item.value("icon").toString();
				const auto pid = item.value("pid").toString().toStdString();
				const auto cb = item.value("cb").toString().toStdString();
				entry.action = [=] {
					Invoke(pid, cb, "{}", PluginLog);
				};
			} else {
				continue;
			}
			Settings::Register(std::move(entry));
		}
		return Reply(true);
	});
}

std::string GetSetting(const QJsonObject &args) {
	return OnMain([=]() -> std::string {
		const auto entry = FindEntry(args.value("key").toString());
		if (!entry) {
			return Reply(false, U("настройка не зарегистрирована"));
		}
		auto result = QJsonObject();
		result.insert("ok", true);
		result.insert("v", CurrentValue(*entry));
		return QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
	});
}

// Несколько настроек клиента, которые плагинам можно читать (не писать).
std::string ClientSettingGet(const QJsonObject &args) {
	const auto key = args.value("key").toString();
	static const auto allowed = std::set<QString>{
		u"plugins.prefix"_q,
		u"plugins.dev_reload"_q,
	};
	return OnMain([=]() -> std::string {
		const auto entry = allowed.contains(key) ? FindEntry(key) : nullptr;
		if (!entry) {
			return Reply(false, U("настройка закрыта"));
		}
		auto result = QJsonObject();
		result.insert("ok", true);
		result.insert("v", CurrentValue(*entry));
		return QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
	});
}

std::string SetSetting(const QJsonObject &args) {
	const auto key = args.value("key").toString();
	const auto value = args.value("value");
	return OnMain([=]() -> std::string {
		const auto entry = FindEntry(key);
		if (!entry || !key.startsWith(U("plugin."))) {
			return Reply(false, U("настройка не зарегистрирована"));
		}
		switch (entry->kind) {
		case Settings::Kind::Bool:
			Settings::SetBool(key, value.toBool());
			break;
		case Settings::Kind::Int:
		case Settings::Kind::Choice:
			Settings::SetInt(key, std::clamp(
				value.toInt(),
				entry->minValue,
				entry->maxValue));
			break;
		default:
			Settings::SetString(key, value.toString().left(500));
			break;
		}
		return Reply(true);
	});
}

[[nodiscard]] HistoryItem *FindItem(const QJsonObject &args) {
	const auto session = ActiveSession();
	const auto peerId = PeerId(args.value("peer").toString().toULongLong());
	const auto id = MsgId(args.value("id").toString().toLongLong());
	if (!session || !peerId || !id) {
		return nullptr;
	}
	return session->data().message(peerId, id);
}

[[nodiscard]] QJsonObject ItemBrief(not_null<const HistoryItem*> item);

std::string GetMessage(const QJsonObject &args) {
	return OnMain([=]() -> std::string {
		const auto item = FindItem(args);
		if (!item) {
			return Reply(false, U("сообщение не загружено"));
		}
		auto result = ItemBrief(item);
		result.insert("ok", true);
		result.insert("can_edit", item->canBeEdited());
		return QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
	});
}

std::string EditMessage(const QJsonObject &args) {
	const auto text = args.value("text").toString();
	const auto entities = args.value("entities").toArray();
	if (text.isEmpty()) {
		return Reply(false, U("пустой текст"));
	}
	return OnMain([=]() -> std::string {
		const auto item = FindItem(args);
		if (!item || !item->out() || !item->canBeEdited()) {
			return Reply(false, U("это сообщение нельзя изменить"));
		}
		MarkFromPlugin(true);
		Api::EditTextMessage(
			item,
			TextWithEntities{ text, EntitiesFromJson(entities, text.size()) },
			Data::WebPageDraft(),
			Api::SendOptions(),
			[](mtpRequestId) {},
			[](const QString &error, mtpRequestId) {
				LOG(("[mokagram] edit_message: %1").arg(error));
			},
			false);
		MarkFromPlugin(false);
		return Reply(true);
	});
}

std::string ChatInfo(const QJsonObject &args) {
	const auto peerId = PeerId(args.value("peer").toString().toULongLong());
	return OnMain([=]() -> std::string {
		const auto session = ActiveSession();
		const auto peer = (session && peerId)
			? session->data().peerLoaded(peerId)
			: nullptr;
		if (!peer) {
			return Reply(false, U("чата нет среди загруженных"));
		}
		auto result = QJsonObject();
		result.insert("ok", true);
		result.insert("title", peer->name());
		result.insert("username", peer->username());
		const auto user = peer->asUser();
		result.insert("kind", user
			? (user->isBot() ? "bot" : "user")
			: (peer->isChannel() && !peer->isMegagroup() ? "channel" : "group"));
		if (const auto channel = peer->asChannel()) {
			result.insert("members", channel->membersCount());
		} else if (const auto chat = peer->asChat()) {
			result.insert("members", chat->count);
		}
		return QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
	});
}

std::string OpenChat(const QJsonObject &args) {
	const auto peerId = PeerId(args.value("peer").toString().toULongLong());
	const auto msgId = MsgId(args.value("msg_id").toString().toLongLong());
	return OnMain([=]() -> std::string {
		const auto session = ActiveSession();
		const auto peer = (session && peerId)
			? session->data().peerLoaded(peerId)
			: nullptr;
		const auto window = Core::App().activeWindow();
		const auto controller = window ? window->sessionController() : nullptr;
		if (!peer || !controller) {
			return Reply(false, U("не удалось открыть чат"));
		}
		if (msgId > 0) {
			controller->showPeerHistory(
				peer,
				Window::SectionShow::Way::Forward,
				msgId);
		} else {
			controller->showPeerHistory(peer);
		}
		return Reply(true);
	});
}

std::string OpenUrl(const QJsonObject &args) {
	const auto url = args.value("url").toString().left(2000);
	if (!url.startsWith(U("https://")) && !url.startsWith(U("http://"))) {
		return Reply(false, U("разрешены только http и https"));
	}
	return OnMain([=]() -> std::string {
		UrlClickHandler::Open(url);
		return Reply(true);
	});
}

std::string CopyText(const QJsonObject &args) {
	const auto text = args.value("text").toString().left(10000);
	return OnMain([=]() -> std::string {
		TextUtilities::SetClipboardText(TextForMimeData::Simple(text));
		return Reply(true);
	});
}

// ---- свои окна плагинов ----

struct DialogState {
	std::string pid;
	QPointer<Ui::GenericBox> box;
	QPointer<Ui::FlatLabel> message;
	QPointer<Ui::RpWidget> bar;
	std::shared_ptr<double> progress = std::make_shared<double>(0.);
};

std::map<int, std::shared_ptr<DialogState>> Dialogs;
int DialogNext = 0;

void PluginLog(const std::string &line) {
	LOG(("[mokagram] %1").arg(QString::fromStdString(line)));
}

[[nodiscard]] std::string Json(const QJsonObject &object) {
	return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

std::string DialogShow(const QJsonObject &args) {
	return OnMain([=]() -> std::string {
		const auto window = Core::App().activeWindow();
		const auto controller = window ? window->sessionController() : nullptr;
		if (!controller) {
			return Reply(false, U("нет открытого окна"));
		}
		const auto id = ++DialogNext;
		const auto state = std::make_shared<DialogState>();
		state->pid = args.value("pid").toString().toStdString();
		Dialogs[id] = state;
		const auto closeCb = args.value("close_cb").toString().toStdString();
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			state->box = box;
			box->setTitle(rpl::single(args.value("title").toString()));
			const auto content = box->verticalLayout();
			const auto message = args.value("message").toString();
			Ui::AddSkip(content);
			state->message = content->add(
				object_ptr<Ui::FlatLabel>(content, message, st::boxLabel),
				st::boxPadding);
			if (message.isEmpty()) {
				state->message->hide();
			}
			using Getter = std::pair<QString, Fn<QJsonValue()>>;
			const auto getters = std::make_shared<std::vector<Getter>>();
			for (const auto &value : args.value("fields").toArray()) {
				const auto field = value.toObject();
				const auto type = field.value("type").toString();
				const auto key = field.value("key").toString();
				const auto text = field.value("text").toString();
				const auto def = field.value("default");
				if (type == u"header"_q) {
					Ui::AddSubsectionTitle(content, rpl::single(text));
				} else if (type == u"text"_q) {
					content->add(
						object_ptr<Ui::FlatLabel>(content, text, st::boxLabel),
						st::boxPadding);
				} else if (type == u"input"_q) {
					const auto multiline = field.value("multiline").toBool();
					const auto input = content->add(
						object_ptr<Ui::InputField>(
							content,
							st::defaultInputField,
							multiline
								? Ui::InputField::Mode::MultiLine
								: Ui::InputField::Mode::SingleLine,
							rpl::single(text),
							TextWithTags{ def.toString() }),
						st::boxPadding);
					getters->push_back({ key, [=] {
						return QJsonValue(input->getLastText());
					} });
				} else if (type == u"password"_q) {
					// PasswordInput не RpWidget: кладём его в контейнер фиксированной высоты.
					const auto holder = content->add(
						object_ptr<Ui::FixedHeightWidget>(
							content,
							st::defaultInputField.heightMin),
						st::boxPadding);
					const auto input = Ui::CreateChild<Ui::PasswordInput>(
						holder,
						st::defaultInputField,
						rpl::single(text),
						def.toString());
					holder->widthValue(
					) | rpl::on_next([=](int width) {
						input->setGeometry(0, 0, width, holder->height());
					}, holder->lifetime());
					box->setFocusCallback([=] {
						input->setFocus();
					});
					getters->push_back({ key, [=] {
						return QJsonValue(input->getLastText());
					} });
				} else if (type == u"switch"_q) {
					const auto check = content->add(
						object_ptr<Ui::Checkbox>(content, text, def.toBool()),
						st::boxPadding);
					getters->push_back({ key, [=] {
						return QJsonValue(check->checked());
					} });
				} else if (type == u"select"_q) {
					const auto group = std::make_shared<Ui::RadiobuttonGroup>(
						def.toInt());
					if (!text.isEmpty()) {
						content->add(
							object_ptr<Ui::FlatLabel>(content, text, st::boxLabel),
							st::boxPadding);
					}
					auto index = 0;
					for (const auto &item : field.value("items").toArray()) {
						content->add(
							object_ptr<Ui::Radiobutton>(
								content,
								group,
								index++,
								item.toString()),
							st::boxPadding);
					}
					getters->push_back({ key, [=] {
						return QJsonValue(group->current());
					} });
				} else if (type == u"progress"_q) {
					const auto bar = content->add(
						object_ptr<Ui::RpWidget>(content),
						st::boxPadding);
					bar->resize(bar->width(), 8);
					bar->setMinimumHeight(8);
					bar->setMaximumHeight(8);
					state->progress = std::make_shared<double>(def.toDouble());
					const auto progress = state->progress;
					bar->paintOn([=](QPainter &p) {
						auto hq = PainterHighQualityEnabler(p);
						p.setPen(Qt::NoPen);
						p.setBrush(st::windowBgOver);
						p.drawRoundedRect(bar->rect(), 4, 4);
						p.setBrush(st::windowActiveTextFg);
						p.drawRoundedRect(
							QRectF(
								0,
								0,
								bar->width() * std::clamp(*progress, 0., 100.) / 100.,
								bar->height()),
							4,
							4);
					});
					state->bar = bar;
				}
			}
			Ui::AddSkip(content);
			const auto pid = state->pid;
			const auto cb = args.value("cb").toString().toStdString();
			for (const auto &value : args.value("buttons").toArray()) {
				const auto button = value.toObject();
				const auto buttonId = button.value("id").toString();
				const auto keepOpen = button.value("keep_open").toBool();
				const auto style = button.value("style").toString();
				const auto click = [=] {
					auto values = QJsonObject();
					for (const auto &getter : *getters) {
						values.insert(getter.first, getter.second());
					}
					auto payload = QJsonObject();
					payload.insert("button", buttonId);
					payload.insert("values", values);
					const auto weak = state->box;
					Invoke(pid, cb, Json(payload), PluginLog);
					if (!keepOpen && weak) {
						weak->closeBox();
					}
				};
				const auto text = rpl::single(button.value("text").toString());
				if (style == u"cancel"_q) {
					box->addButton(text, click, st::defaultBoxButton);
				} else if (style == u"red"_q) {
					box->addButton(text, click, st::attentionBoxButton);
				} else {
					box->addButton(text, click);
				}
			}
			box->boxClosing() | rpl::on_next([=] {
				Dialogs.erase(id);
				Invoke(state->pid, closeCb, "{}", PluginLog);
			}, box->lifetime());
		}));
		auto result = QJsonObject();
		result.insert("ok", true);
		result.insert("id", id);
		return Json(result);
	});
}

std::string DialogUpdate(const QJsonObject &args) {
	return OnMain([=]() -> std::string {
		const auto i = Dialogs.find(args.value("id").toInt());
		if (i == end(Dialogs)) {
			return Reply(false, U("окно закрыто"));
		}
		const auto &state = i->second;
		if (args.contains("message") && state->message) {
			const auto text = args.value("message").toString();
			state->message->setText(text);
			state->message->setVisible(!text.isEmpty());
		}
		if (args.contains("title") && state->box) {
			state->box->setTitle(rpl::single(args.value("title").toString()));
		}
		if (args.contains("progress")) {
			*state->progress = args.value("progress").toDouble();
			if (state->bar) {
				state->bar->update();
			}
		}
		return Reply(true);
	});
}

std::string DialogClose(const QJsonObject &args) {
	return OnMain([=]() -> std::string {
		const auto i = Dialogs.find(args.value("id").toInt());
		if (i != end(Dialogs) && i->second->box) {
			i->second->box->closeBox();
		}
		return Reply(true);
	});
}

std::string PluginUnloaded(const QJsonObject &args) {
	const auto pid = args.value("pid").toString().toStdString();
	return OnMain([=]() -> std::string {
		auto boxes = std::vector<QPointer<Ui::GenericBox>>();
		for (const auto &[id, state] : Dialogs) {
			if (state->pid == pid) {
				boxes.push_back(state->box);
			}
		}
		for (const auto &box : boxes) {
			if (box) {
				box->closeBox();
			}
		}
		return Reply(true);
	});
}

std::string ShowBulletin(const QJsonObject &args) {
	return OnMain([=]() -> std::string {
		const auto window = Core::App().activeWindow();
		const auto controller = window ? window->sessionController() : nullptr;
		if (!controller) {
			return Reply(false, U("нет открытого окна"));
		}
		const auto kind = args.value("kind").toString();
		const auto subtitle = args.value("subtitle").toString();
		const auto button = args.value("button").toString();
		const auto prefix = (kind == u"success"_q)
			? U("\u2713 ")
			: (kind == u"error"_q) ? U("\u26a0 ") : QString();
		auto config = Ui::Toast::Config();
		auto text = TextWithEntities{ args.value("text").toString() };
		if (!subtitle.isEmpty()) {
			config.title = prefix + text.text;
			text = TextWithEntities{ subtitle };
		} else {
			text.text = prefix + text.text;
		}
		if (!button.isEmpty()) {
			text.append(u"  "_q);
			text.append(Ui::Text::Link(button, u"moka://bulletin"_q));
			const auto pid = args.value("pid").toString().toStdString();
			const auto cb = args.value("cb").toString().toStdString();
			config.filter = [=](const ClickHandlerPtr &, Qt::MouseButton) {
				Invoke(pid, cb, "{}", PluginLog);
				return false;
			};
			config.acceptinput = true;
		}
		config.text = std::move(text);
		config.duration = crl::time(args.value("duration").toDouble(3.) * 1000);
		controller->showToast(std::move(config));
		return Reply(true);
	});
}

// ---- события клиента ----

[[nodiscard]] QJsonObject PeerBrief(not_null<PeerData*> peer);

std::mutex EventMutex;
std::map<std::string, std::set<std::string>> EventSubscribers;
std::set<std::string> EventWanted;
rpl::lifetime SessionLifetime;

[[nodiscard]] const std::set<std::string> &KnownEvents() {
	static const auto result = std::set<std::string>{
		"new_message", "message_edited", "message_deleted",
		"peer_updated", "user_status", "new_reaction", "chat_opened", "typing",
		"message_sent",
	};
	return result;
}

[[nodiscard]] bool WantEvent(const std::string &name) {
	const auto lock = std::lock_guard(EventMutex);
	return EventWanted.contains(name);
}

std::string EventsSubscribe(const QJsonObject &args) {
	auto names = std::set<std::string>();
	for (const auto &name : args.value("events").toArray()) {
		const auto value = name.toString().toStdString();
		if (KnownEvents().contains(value)) {
			names.insert(value);
		}
	}
	const auto lock = std::lock_guard(EventMutex);
	const auto pid = args.value("pid").toString().toStdString();
	if (!names.empty()) {
		EventSubscribers[pid] = std::move(names);
	} else {
		EventSubscribers.erase(pid);
	}
	EventWanted.clear();
	for (const auto &[_, list] : EventSubscribers) {
		EventWanted.insert(list.begin(), list.end());
	}
	return Reply(true);
}

[[nodiscard]] QString MediaKind(not_null<const HistoryItem*> item) {
	const auto media = item->media();
	if (!media) {
		return QString();
	}
	if (media->photo()) {
		return u"photo"_q;
	}
	if (const auto document = media->document()) {
		if (document->isVoiceMessage()) {
			return u"voice"_q;
		} else if (document->isVideoMessage()) {
			return u"round"_q;
		} else if (document->sticker()) {
			return u"sticker"_q;
		} else if (document->isAnimation()) {
			return u"gif"_q;
		} else if (document->isVideoFile()) {
			return u"video"_q;
		} else if (document->isAudioFile()) {
			return u"audio"_q;
		}
		return u"document"_q;
	}
	return u"other"_q;
}

void EmitMessageEvent(
		const char *name,
		not_null<const HistoryItem*> item) {
	if (!WantEvent(name)) {
		return;
	}
	auto payload = QJsonObject();
	payload.insert("id", QString::number(item->id.bare));
	const auto peer = item->history()->peer;
	payload.insert("peer", QString::number(peer->id.value));
	payload.insert("peer_title", peer->name());
	payload.insert("peer_kind", PeerBrief(peer).value("kind"));
	payload.insert("media", MediaKind(item));
	if (const auto group = item->groupId().raw()) {
		payload.insert("grouped_id", QString::number(group));
	}
	payload.insert("from", QString::number(item->from()->id.value));
	payload.insert("text", item->originalText().text);
	payload.insert("out", item->out());
	payload.insert("date", double(item->date()));
	payload.insert("has_media", item->media() != nullptr);
	payload.insert("reply_to", QString::number(item->replyToId().bare));
	payload.insert("entities", EntitiesToJson(item->originalText().entities));
	if (const auto buttons = ButtonsJson(item); !buttons.isEmpty()) {
		payload.insert("buttons", buttons);
	}
	QueueEvent(name, Json(payload));
}

void SubscribeSession(Main::Session *session) {
	SessionLifetime.destroy();
	if (!session) {
		return;
	}
	session->data().newItemAdded(
	) | rpl::on_next([](not_null<HistoryItem*> item) {
		EmitMessageEvent("new_message", item);
	}, SessionLifetime);
	session->data().itemRemoved(
	) | rpl::on_next([](not_null<const HistoryItem*> item) {
		EmitMessageEvent("message_deleted", item);
	}, SessionLifetime);
	session->changes().messageUpdates(
		Data::MessageUpdate::Flag::Edited
	) | rpl::on_next([](const Data::MessageUpdate &update) {
		EmitMessageEvent("message_edited", update.item);
	}, SessionLifetime);
	session->data().itemIdChanged(
	) | rpl::on_next([](const Data::Session::IdChange &change) {
		if (!WantEvent("message_sent")) {
			return;
		}
		const auto item = change.newId.msg
			? ActiveSession()->data().message(change.newId)
			: nullptr;
		if (!item || !item->out()) {
			return;
		}
		auto payload = QJsonObject();
		payload.insert("id", QString::number(change.newId.msg.bare));
		payload.insert("old_id", QString::number(change.oldId.bare));
		payload.insert("peer", QString::number(item->history()->peer->id.value));
		payload.insert("text", item->originalText().text);
		QueueEvent("message_sent", Json(payload));
	}, SessionLifetime);
	session->changes().messageUpdates(
		Data::MessageUpdate::Flag::NewUnreadReaction
	) | rpl::on_next([](const Data::MessageUpdate &update) {
		EmitMessageEvent("new_reaction", update.item);
	}, SessionLifetime);

	using PeerFlag = Data::PeerUpdate::Flag;
	session->changes().peerUpdates(
		PeerFlag::Name
			| PeerFlag::Username
			| PeerFlag::Photo
			| PeerFlag::About
			| PeerFlag::IsBlocked
			| PeerFlag::EmojiStatus
	) | rpl::on_next([](const Data::PeerUpdate &update) {
		if (!WantEvent("peer_updated")) {
			return;
		}
		auto changed = QJsonArray();
		const auto add = [&](PeerFlag flag, const char *name) {
			if (update.flags & flag) {
				changed.push_back(QString::fromLatin1(name));
			}
		};
		add(PeerFlag::Name, "name");
		add(PeerFlag::Username, "username");
		add(PeerFlag::Photo, "photo");
		add(PeerFlag::About, "about");
		add(PeerFlag::IsBlocked, "blocked");
		add(PeerFlag::EmojiStatus, "emoji_status");
		auto payload = PeerBrief(update.peer);
		payload.insert("changed", changed);
		QueueEvent("peer_updated", Json(payload));
	}, SessionLifetime);
	session->changes().peerUpdates(
		PeerFlag::OnlineStatus
	) | rpl::on_next([](const Data::PeerUpdate &update) {
		if (!WantEvent("user_status")) {
			return;
		}
		const auto user = update.peer->asUser();
		if (!user) {
			return;
		}
		auto payload = PeerBrief(user);
		payload.insert(
			"online",
			Data::OnlineTextActive(user, base::unixtime::now()));
		QueueEvent("user_status", Json(payload));
	}, SessionLifetime);
}

std::string SendFile(const QJsonObject &args) {
	const auto peerId = PeerId(args.value("peer").toString().toULongLong());
	const auto path = args.value("path").toString();
	const auto caption = args.value("caption").toString();
	const auto asPhoto = args.value("kind").toString() == u"photo"_q;
	const auto silent = args.value("silent").toBool();
	const auto info = QFileInfo(path);
	if (!peerId || !info.isFile() || info.size() <= 0 || info.size() > (qint64(2000) << 20)) {
		return Reply(false, U("файл не найден или слишком большой"));
	}
	return OnMain([=]() -> std::string {
		const auto session = ActiveSession();
		const auto history = session ? session->data().historyLoaded(peerId) : nullptr;
		if (!history) {
			return Reply(false, U("чата нет среди загруженных диалогов"));
		}
		auto list = Storage::PrepareMediaList(
			QStringList{ path },
			st::sendMediaPreviewSize,
			session->user()->isPremium());
		if (list.files.empty() && list.filesToProcess.empty()) {
			return Reply(false, U("файл нельзя отправить"));
		}
		if (!list.files.empty()) {
			list.files.front().caption = TextWithTags{ caption };
		}
		auto action = Api::SendAction(history);
		action.options.silent = silent;
		action.clearDraft = false;
		const auto weak = base::make_weak(session);
		crl::on_main([=, list = std::move(list)]() mutable {
			if (const auto strong = weak.get()) {
				MarkFromPlugin(true);
				strong->api().sendFiles(
					std::move(list),
					asPhoto ? SendMediaType::Photo : SendMediaType::File,
					nullptr,
					action);
				MarkFromPlugin(false);
			}
		});
		return Reply(true);
	});
}

[[nodiscard]] QJsonObject PeerBrief(not_null<PeerData*> peer) {
	auto result = QJsonObject();
	result.insert("peer", QString::number(peer->id.value));
	result.insert("title", peer->name());
	result.insert("username", peer->username());
	const auto user = peer->asUser();
	result.insert("kind", user
		? (user->isBot() ? "bot" : "user")
		: (peer->isChannel() && !peer->isMegagroup() ? "channel" : "group"));
	return result;
}

[[nodiscard]] QJsonObject ItemBrief(not_null<const HistoryItem*> item) {
	auto result = QJsonObject();
	result.insert("id", QString::number(item->id.bare));
	result.insert("peer", QString::number(item->history()->peer->id.value));
	result.insert("from", QString::number(item->from()->id.value));
	result.insert("text", item->originalText().text);
	result.insert("out", item->out());
	result.insert("date", double(item->date()));
	result.insert("has_media", item->media() != nullptr);
	result.insert("entities", EntitiesToJson(
		item->originalText().entities,
		item->originalText().text));
	result.insert("reply_to", QString::number(item->replyToId().bare));
	if (const auto media = item->media()) {
		result.insert("media", MediaKind(item));
		if (const auto document = media->document()) {
			result.insert("file_name", document->filename());
			result.insert("mime", document->mimeString());
			result.insert("size", double(document->size));
		}
	}
	if (const auto views = item->Get<HistoryMessageViews>()) {
		if (views->views.count >= 0) {
			result.insert("views", views->views.count);
		}
		if (views->forwardsCount >= 0) {
			result.insert("forwards", views->forwardsCount);
		}
	}
	if (const auto buttons = ButtonsJson(item); !buttons.isEmpty()) {
		result.insert("buttons", buttons);
	}
	return result;
}

std::string GetDialogs(const QJsonObject &args) {
	const auto limit = std::clamp(args.value("limit").toInt(50), 1, 200);
	return OnMain([=]() -> std::string {
		const auto session = ActiveSession();
		if (!session) {
			return Reply(false, U("нет открытой сессии"));
		}
		auto list = QJsonArray();
		for (const auto &row : session->data().chatsList()->indexed()->all()) {
			if (const auto history = row->history()) {
				auto brief = PeerBrief(history->peer);
				brief.insert("unread", history->unreadCount());
				list.push_back(brief);
				if (list.size() >= limit) {
					break;
				}
			}
		}
		auto result = QJsonObject();
		result.insert("ok", true);
		result.insert("dialogs", list);
		return Json(result);
	});
}

std::string GetHistory(const QJsonObject &args) {
	const auto peerId = PeerId(args.value("peer").toString().toULongLong());
	const auto limit = std::clamp(args.value("limit").toInt(30), 1, 100);
	return OnMain([=]() -> std::string {
		const auto session = ActiveSession();
		const auto history = (session && peerId)
			? session->data().historyLoaded(peerId)
			: nullptr;
		if (!history) {
			return Reply(false, U("чата нет среди загруженных"));
		}
		auto list = QJsonArray();
		for (auto b = history->blocks.rbegin(); b != history->blocks.rend(); ++b) {
			const auto &messages = (*b)->messages;
			for (auto m = messages.rbegin(); m != messages.rend(); ++m) {
				list.push_front(ItemBrief((*m)->data()));
				if (list.size() >= limit) {
					break;
				}
			}
			if (list.size() >= limit) {
				break;
			}
		}
		auto result = QJsonObject();
		result.insert("ok", true);
		result.insert("messages", list);
		return Json(result);
	});
}

std::string CurrentChat(const QJsonObject &) {
	return OnMain([]() -> std::string {
		const auto window = Core::App().activeWindow();
		const auto controller = window ? window->sessionController() : nullptr;
		const auto history = controller
			? controller->activeChatCurrent().history()
			: nullptr;
		if (!history) {
			return Reply(false, U("чат не открыт"));
		}
		auto result = PeerBrief(history->peer);
		result.insert("ok", true);
		return Json(result);
	});
}

std::string MarkRead(const QJsonObject &args) {
	const auto peerId = PeerId(args.value("peer").toString().toULongLong());
	return OnMain([=]() -> std::string {
		const auto session = ActiveSession();
		const auto history = (session && peerId)
			? session->data().historyLoaded(peerId)
			: nullptr;
		if (!history) {
			return Reply(false, U("чата нет среди загруженных"));
		}
		session->data().histories().readInbox(history);
		return Reply(true);
	});
}

std::string GetUserInfo(const QJsonObject &args) {
	const auto peerId = PeerId(args.value("peer").toString().toULongLong());
	return OnMain([=]() -> std::string {
		const auto session = ActiveSession();
		const auto peer = (session && peerId)
			? session->data().peerLoaded(peerId)
			: nullptr;
		if (!peer) {
			return Reply(false, U("чата нет среди загруженных"));
		}
		auto result = PeerBrief(peer);
		result.insert("ok", true);
		result.insert("about", peer->about());
		if (const auto user = peer->asUser()) {
			result.insert("premium", user->isPremium());
			result.insert("verified", user->isVerified());
			result.insert("scam", user->isScam());
			result.insert("fake", user->isFake());
			result.insert("contact", user->isContact());
			result.insert("blocked", user->isBlocked());
			result.insert(
				"online",
				Data::OnlineTextActive(user, base::unixtime::now()));
		}
		return Json(result);
	});
}

std::string GetMe(const QJsonObject &) {
	return OnMain([]() -> std::string {
		const auto session = ActiveSession();
		if (!session) {
			return Reply(false, U("нет открытой сессии"));
		}
		auto result = PeerBrief(session->user());
		result.insert("ok", true);
		result.insert("premium", session->user()->isPremium());
		return Json(result);
	});
}

std::string ClipboardGet(const QJsonObject &) {
	return OnMain([]() -> std::string {
		auto result = QJsonObject();
		result.insert("ok", true);
		result.insert("text", QGuiApplication::clipboard()->text().left(100000));
		return Json(result);
	});
}

std::string PinMessage(const QJsonObject &args) {
	const auto pin = args.value("pin").toBool(true);
	return OnMain([=]() -> std::string {
		const auto item = FindItem(args);
		if (!item || !item->id.bare || item->isService()) {
			return Reply(false, U("сообщение не найдено"));
		}
		const auto peer = item->history()->peer;
		const auto session = &peer->session();
		auto flags = MTPmessages_UpdatePinnedMessage::Flags(0);
		if (!pin) {
			flags |= MTPmessages_UpdatePinnedMessage::Flag::f_unpin;
		}
		session->api().request(MTPmessages_UpdatePinnedMessage(
			MTP_flags(flags),
			peer->input(),
			MTP_int(item->id.bare)
		)).done([=](const MTPUpdates &result) {
			session->api().applyUpdates(result);
		}).send();
		return Reply(true);
	});
}

std::string DeleteMessages(const QJsonObject &args) {
	const auto peerId = PeerId(args.value("peer").toString().toULongLong());
	const auto revoke = args.value("revoke").toBool();
	const auto list = args.value("ids").toArray();
	return OnMain([=]() -> std::string {
		const auto session = ActiveSession();
		if (!session || !peerId || list.isEmpty() || list.size() > 100) {
			return Reply(false, U("нет сообщений для удаления"));
		}
		auto ids = MessageIdsList();
		for (const auto &value : list) {
			const auto id = MsgId(value.toString().toLongLong());
			if (const auto item = session->data().message(peerId, id)) {
				ids.push_back(item->fullId());
			}
		}
		if (ids.empty()) {
			return Reply(false, U("сообщения не найдены"));
		}
		session->data().histories().deleteMessages(ids, revoke);
		session->data().sendHistoryChangeNotifications();
		return Reply(true);
	});
}

std::string SendAlbum(const QJsonObject &args) {
	const auto peerId = PeerId(args.value("peer").toString().toULongLong());
	const auto caption = args.value("caption").toString();
	const auto asPhoto = args.value("kind").toString() == u"photo"_q;
	const auto silent = args.value("silent").toBool();
	auto paths = QStringList();
	for (const auto &value : args.value("paths").toArray()) {
		const auto info = QFileInfo(value.toString());
		if (!info.isFile() || info.size() <= 0 || info.size() > (qint64(2000) << 20)) {
			return Reply(false, U("файл не найден или слишком большой"));
		}
		paths.push_back(info.absoluteFilePath());
	}
	if (!peerId || paths.size() < 2 || paths.size() > 10) {
		return Reply(false, U("в альбоме от 2 до 10 файлов"));
	}
	return OnMain([=]() -> std::string {
		const auto session = ActiveSession();
		const auto history = session ? session->data().historyLoaded(peerId) : nullptr;
		if (!history) {
			return Reply(false, U("чата нет среди загруженных диалогов"));
		}
		auto list = Storage::PrepareMediaList(
			paths,
			st::sendMediaPreviewSize,
			session->user()->isPremium());
		if (list.files.size() < 2 || !list.filesToProcess.empty()) {
			return Reply(false, U("файлы нельзя отправить альбомом"));
		}
		list.files.front().caption = TextWithTags{ caption };
		auto action = Api::SendAction(history);
		action.options.silent = silent;
		action.clearDraft = false;
		const auto weak = base::make_weak(session);
		crl::on_main([=, list = std::move(list)]() mutable {
			if (const auto strong = weak.get()) {
				MarkFromPlugin(true);
				strong->api().sendFiles(
					std::move(list),
					asPhoto ? SendMediaType::Photo : SendMediaType::File,
					std::make_shared<SendingAlbum>(),
					action);
				MarkFromPlugin(false);
			}
		});
		return Reply(true);
	});
}

// Сохраняет медиа сообщения в файл. Итог приходит плагину событием __cb: {ok, path} или {ok:false, error}.
std::string DownloadMedia(const QJsonObject &args) {
	const auto pid = args.value("pid").toString();
	const auto token = args.value("token").toString();
	const auto path = args.value("path").toString();
	if (path.isEmpty()) {
		return Reply(false, U("нет пути"));
	}
	return OnMain([=]() -> std::string {
		const auto item = FindItem(args);
		const auto media = item ? item->media() : nullptr;
		if (!media) {
			return Reply(false, U("в сообщении нет медиа"));
		}
		const auto deliver = [=](bool ok, const QString &error) {
			auto payload = QJsonObject();
			payload.insert("ok", ok);
			if (ok) {
				payload.insert("path", path);
			} else {
				payload.insert("error", error);
			}
			auto wrapper = QJsonObject();
			wrapper.insert("pid", pid);
			wrapper.insert("token", token);
			wrapper.insert("payload", payload);
			QueueEvent("__cb", Json(wrapper));
		};
		QDir().mkpath(QFileInfo(path).absolutePath());
		auto check = Fn<bool()>();
		if (const auto document = media->document()) {
			document->save(item->fullId(), path, LoadFromCloudOrLocal, false);
			check = [=] {
				const auto info = QFileInfo(path);
				return !document->loading()
					&& info.exists()
					&& info.size() > 0
					&& (document->size <= 0 || info.size() >= document->size);
			};
		} else if (const auto photo = media->photo()) {
			const auto view = photo->createMediaView();
			view->wanted(Data::PhotoSize::Large, item->fullId());
			check = [=] {
				const auto image = view->image(Data::PhotoSize::Large);
				return image && image->original().save(path, "JPG", 95);
			};
		} else {
			return Reply(false, U("это медиа нельзя сохранить"));
		}
		const auto timer = new QTimer(QCoreApplication::instance());
		const auto started = crl::now();
		QObject::connect(timer, &QTimer::timeout, [=] {
			if (check()) {
				timer->deleteLater();
				deliver(true, QString());
			} else if (crl::now() - started > 300 * crl::time(1000)) {
				timer->deleteLater();
				deliver(false, u"timeout"_q);
			}
		});
		timer->start(300);
		return Reply(true);
	});
}

std::string ClientDataDir(const QJsonObject &) {
	auto result = QJsonObject();
	result.insert("ok", true);
	result.insert("dir", cWorkingDir() + u"tdata/"_q);
	return Json(result);
}

std::string ClientLang(const QJsonObject &) {
	// Tr() возвращает русский текст как есть только в русском, украинском и белорусском интерфейсе.
	const auto russian = (std::string(Mokagram::Lang::Tr("Да")) == "Да");
	auto result = QJsonObject();
	result.insert("ok", true);
	result.insert("lang", russian ? "ru" : "en");
	return Json(result);
}

// Окно установки плагина из файла (права, версия): пользователь подтверждает сам.
std::string InstallPluginFile(const QJsonObject &args) {
	const auto path = args.value("path").toString();
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() > (1 << 20)) {
		return Reply(false, U("файл плагина не прочитан"));
	}
	const auto bytes = file.readAll();
	return OnMain([=]() -> std::string {
		const auto window = Core::App().activeWindow();
		const auto controller = window ? window->sessionController() : nullptr;
		if (!controller) {
			return Reply(false, U("нет открытого окна"));
		}
		InstallFromBytes(controller, bytes, [] {});
		return Reply(true);
	});
}

// ---- универсальный вызов TL ----

[[nodiscard]] std::string HexOfPrimes(const mtpPrime *data, size_t ints) {
	return QByteArray(
		reinterpret_cast<const char*>(data),
		int(ints * sizeof(mtpPrime))).toHex().toStdString();
}

template <typename T>
[[nodiscard]] std::string SerializeHex(const T &value) {
	auto buffer = mtpBuffer();
	value.template write<mtpBuffer>(buffer);
	return HexOfPrimes(buffer.constData(), size_t(buffer.size()));
}

// kind: peer | user | channel. Возвращает сериализованный InputPeer/InputUser/InputChannel,
// чтобы Python собрал запрос с верным access_hash.
std::string TlInput(const QJsonObject &args) {
	const auto peerId = PeerId(args.value("peer").toString().toULongLong());
	const auto kind = args.value("kind").toString();
	return OnMain([=]() -> std::string {
		const auto session = ActiveSession();
		const auto peer = (session && peerId)
			? session->data().peerLoaded(peerId)
			: nullptr;
		if (!peer) {
			return Reply(false, U("чата нет среди загруженных"));
		}
		const auto user = peer->asUser();
		const auto channel = peer->asChannel();
		auto result = QJsonObject();
		result.insert("ok", true);
		result.insert("kind", user ? "user" : channel ? "channel" : "chat");
		result.insert("bare", QString::number(user
			? peerToUser(peer->id).bare
			: channel
			? peerToChannel(peer->id).bare
			: peerToChat(peer->id).bare));
		if (kind == u"peer"_q) {
			result.insert("hex", QString::fromStdString(
				SerializeHex(peer->input())));
		} else if (kind == u"user"_q && user) {
			result.insert("hex", QString::fromStdString(
				SerializeHex(user->inputUser())));
		} else if (kind == u"channel"_q && channel) {
			result.insert("hex", QString::fromStdString(
				SerializeHex(channel->inputChannel())));
		} else {
			return Reply(false, U("нет такого вида ввода для этого чата"));
		}
		return Json(result);
	});
}

// Отправляет готовое тело запроса. Ответ уходит плагину как событие __cb
// (pid, token, payload) в его рабочем потоке. Ответы с обновлениями применяются в клиенте.
std::string InvokeRaw(const QJsonObject &args) {
	const auto pid = args.value("pid").toString().toStdString();
	const auto token = args.value("token").toString().toStdString();
	const auto bytes = QByteArray::fromHex(
		args.value("hex").toString().toLatin1());
	if (bytes.size() < 4 || (bytes.size() & 3) || bytes.size() > (1 << 20)) {
		return Reply(false, U("плохой запрос"));
	}
	return OnMain([=]() -> std::string {
		const auto session = ActiveSession();
		if (!session) {
			return Reply(false, U("нет открытой сессии"));
		}
		const auto ints = uint32(bytes.size() / 4);
		auto request = MTP::details::SerializedRequest::Prepare(ints);
		const auto old = request->size();
		request->resize(old + int(ints));
		memcpy(request->data() + old, bytes.constData(), size_t(bytes.size()));

		const auto weak = base::make_weak(session);
		const auto deliver = [=](QJsonObject payload) {
			auto wrapper = QJsonObject();
			wrapper.insert("pid", QString::fromStdString(pid));
			wrapper.insert("token", QString::fromStdString(token));
			wrapper.insert("payload", payload);
			QueueEvent("__cb", Json(wrapper));
		};
		const auto id = MTP::details::GetNextRequestId();
		session->mtp().sendSerialized(id, std::move(request), MTP::ResponseHandler{
			.done = [=](const MTP::Response &response) -> bool {
				const auto &reply = response.reply;
				if (!reply.isEmpty()) {
					switch (mtpTypeId(reply.front())) {
					case mtpc_updatesTooLong:
					case mtpc_updateShortMessage:
					case mtpc_updateShortChatMessage:
					case mtpc_updateShort:
					case mtpc_updatesCombined:
					case mtpc_updates:
					case mtpc_updateShortSentMessage: {
						auto from = reply.constData();
						const auto end = from + reply.size();
						auto updates = MTPUpdates();
						if (updates.read(from, end)) {
							if (const auto strong = weak.get()) {
								strong->api().applyUpdates(updates);
							}
						}
					} break;
					}
				}
				auto payload = QJsonObject();
				payload.insert("ok", true);
				payload.insert("hex", QString::fromStdString(HexOfPrimes(
					reply.constData(),
					size_t(reply.size()))));
				deliver(payload);
				return true;
			},
			.fail = [=](const MTP::Error &error, const MTP::Response &) -> bool {
				auto payload = QJsonObject();
				payload.insert("ok", false);
				payload.insert("code", int(error.code()));
				payload.insert("type", error.type());
				payload.insert("description", error.description());
				deliver(payload);
				return true;
			},
		}, 0, 0, 0);
		return Reply(true);
	});
}

// ---- фильтр скрытия сообщений ----

std::atomic<int> FilterCount = 0;
std::set<std::string> FilterPlugins;
std::map<FullMsgId, bool> FilterCache;

std::atomic<int> TextHookCount = 0;
std::set<std::string> TextHookPlugins;

std::atomic<int> BadgeHookCount = 0;
std::set<std::string> BadgeHookPlugins;
std::map<FullMsgId, std::pair<TimeId, QString>> BadgeCache;

std::string FilterSubscribe(const QJsonObject &args) {
	return OnMain([=]() -> std::string {
		const auto pid = args.value("pid").toString().toStdString();
		const auto on = args.value("on").toBool();
		const auto kind = args.value("kind").toString();
		const auto text = (kind == u"text"_q);
		auto &set = (kind == u"badge"_q)
			? BadgeHookPlugins
			: text
			? TextHookPlugins
			: FilterPlugins;
		if (on) {
			set.insert(pid);
		} else {
			set.erase(pid);
		}
		FilterCount = int(FilterPlugins.size());
		TextHookCount = int(TextHookPlugins.size());
		BadgeHookCount = int(BadgeHookPlugins.size());
		FilterCache.clear();
		BadgeCache.clear();
		return Reply(true);
	});
}

std::string Bridge(const std::string &op, const std::string &argsJson) {
	const auto args = QJsonDocument::fromJson(
		QByteArray::fromStdString(argsJson)).object();
	if (op == "toast") {
		return ShowToast(args);
	} else if (op == "send_message") {
		return SendMessage(args);
	} else if (op == "dialog_show") {
		return DialogShow(args);
	} else if (op == "dialog_update") {
		return DialogUpdate(args);
	} else if (op == "dialog_close") {
		return DialogClose(args);
	} else if (op == "plugin_unloaded") {
		EventsSubscribe(QJsonObject{
			{ "pid", args.value("pid") },
			{ "events", QJsonArray() } });
		FilterSubscribe(QJsonObject{
			{ "pid", args.value("pid") },
			{ "on", false } });
		FilterSubscribe(QJsonObject{
			{ "pid", args.value("pid") },
			{ "kind", "text" },
			{ "on", false } });
		FilterSubscribe(QJsonObject{
			{ "pid", args.value("pid") },
			{ "kind", "badge" },
			{ "on", false } });
		return PluginUnloaded(args);
	} else if (op == "filter_subscribe") {
		return FilterSubscribe(args);
	} else if (op == "get_dialogs") {
		return GetDialogs(args);
	} else if (op == "get_history") {
		return GetHistory(args);
	} else if (op == "current_chat") {
		return CurrentChat(args);
	} else if (op == "mark_read") {
		return MarkRead(args);
	} else if (op == "get_user_info") {
		return GetUserInfo(args);
	} else if (op == "get_me") {
		return GetMe(args);
	} else if (op == "send_album") {
		return SendAlbum(args);
	} else if (op == "download_media") {
		return DownloadMedia(args);
	} else if (op == "client_data_dir") {
		return ClientDataDir(args);
	} else if (op == "client_lang") {
		return ClientLang(args);
	} else if (op == "install_plugin_file") {
		return InstallPluginFile(args);
	} else if (op == "tl_input") {
		return TlInput(args);
	} else if (op == "invoke_raw") {
		return InvokeRaw(args);
	} else if (op == "clipboard_get") {
		return ClipboardGet(args);
	} else if (op == "pin_message") {
		return PinMessage(args);
	} else if (op == "delete_messages") {
		return DeleteMessages(args);
	} else if (op == "send_file") {
		return SendFile(args);
	} else if (op == "run_main") {
		const auto pid = args.value("pid").toString().toStdString();
		const auto cb = args.value("cb").toString().toStdString();
		const auto delay = std::clamp(args.value("delay").toInt(), 0, 3600000);
		crl::on_main([=] {
			QTimer::singleShot(delay, [=] {
				Invoke(pid, cb, "{}", PluginLog);
			});
		});
		return Reply(true);
	} else if (op == "events_subscribe") {
		return EventsSubscribe(args);
	} else if (op == "bulletin") {
		return ShowBulletin(args);
	} else if (op == "get_message") {
		return GetMessage(args);
	} else if (op == "edit_message") {
		return EditMessage(args);
	} else if (op == "chat_info") {
		return ChatInfo(args);
	} else if (op == "open_chat") {
		return OpenChat(args);
	} else if (op == "open_url") {
		return OpenUrl(args);
	} else if (op == "copy_text") {
		return CopyText(args);
	} else if (op == "settings_register") {
		return RegisterSettings(args);
	} else if (op == "client_setting_get") {
		return ClientSettingGet(args);
	} else if (op == "setting_get") {
		return GetSetting(args);
	} else if (op == "setting_set") {
		return SetSetting(args);
	}
	return Reply(false, U("неизвестная операция"));
}

} // namespace

EntitiesInText EntitiesFromJsonArray(const QJsonArray &list, int textLength) {
	return EntitiesFromJson(list, textLength);
}

QJsonArray EntitiesToJsonArray(const EntitiesInText &list) {
	return EntitiesToJson(list);
}

namespace {

void CopyTree(const QString &from, const QString &to) {
	const auto dir = QDir(from);
	QDir().mkpath(to);
	for (const auto &info : dir.entryInfoList(
			QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
		const auto target = QDir(to).filePath(info.fileName());
		if (info.isDir()) {
			CopyTree(info.absoluteFilePath(), target);
		} else if (info.fileName() != u"secrets.enc"_q) {
			QFile::remove(target);
			QFile::copy(info.absoluteFilePath(), target);
		}
	}
}

} // namespace

void ApplyPendingRestore() {
	const auto tdata = cWorkingDir() + u"tdata/"_q;
	const auto root = tdata + u"restore-pending/"_q;
	if (!QDir(root).exists()) {
		return;
	}
	for (const auto &name : {
		u"moka_settings.json"_q,
		u"moka_notes.json"_q,
		u"moka_snippets.json"_q,
		u"mokagram_filters.json"_q,
	}) {
		const auto from = root + name;
		if (!QFile::exists(from)) {
			continue;
		}
		QFile::remove(tdata + name + u".bak"_q);
		QFile::rename(tdata + name, tdata + name + u".bak"_q);
		QFile::copy(from, tdata + name);
	}
	if (QDir(root + u"plugin_data"_q).exists()) {
		const auto base = QDir::cleanPath(
			QString::fromStdString(PluginsDir()) + u"/../plugin_data"_q);
		CopyTree(root + u"plugin_data"_q, base);
	}
	QDir(root).removeRecursively();
}

void EmitChatOpened(PeerData *peer) {
	if (!peer || !WantEvent("chat_opened")) {
		return;
	}
	QueueEvent("chat_opened", Json(PeerBrief(peer)));
}

void EmitTyping(PeerData *peer, UserData *user, mtpTypeId type) {
	if (!peer || !user || !WantEvent("typing")) {
		return;
	}
	auto action = "other";
	switch (type) {
	case mtpc_sendMessageTypingAction: action = "typing"; break;
	case mtpc_sendMessageCancelAction: action = "cancel"; break;
	case mtpc_sendMessageRecordAudioAction: action = "record_voice"; break;
	case mtpc_sendMessageRecordVideoAction: action = "record_video"; break;
	case mtpc_sendMessageUploadAudioAction: action = "upload_voice"; break;
	case mtpc_sendMessageUploadVideoAction: action = "upload_video"; break;
	case mtpc_sendMessageUploadPhotoAction: action = "upload_photo"; break;
	case mtpc_sendMessageUploadDocumentAction: action = "upload_file"; break;
	case mtpc_sendMessageChooseStickerAction: action = "choose_sticker"; break;
	}
	auto payload = PeerBrief(peer);
	payload.insert("from", QString::number(user->id.value));
	payload.insert("from_title", user->name());
	payload.insert("action", QString::fromLatin1(action));
	QueueEvent("typing", Json(payload));
}

void InstallBridge() {
	SetBridge(Bridge);
	StartPluginAutoUpdate();
	// Нормальный выход: плагины получают STOP и on_plugin_unload, пока интерпретатор ещё жив.
	QObject::connect(qGuiApp, &QCoreApplication::aboutToQuit, qGuiApp, [] {
		Shutdown(PluginLog);
	});
	QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged, qGuiApp, [](Qt::ApplicationState state) {
		static auto active = true;
		const auto now = (state == Qt::ApplicationActive);
		if (now == active) {
			return;
		}
		active = now;
		auto payload = QJsonObject();
		payload.insert("event", now ? "RESUME" : "PAUSE");
		QueueEvent("app_event", Json(payload));
	});
	Core::App().domain().activeSessionValue(
	) | rpl::on_next(SubscribeSession, BridgeLifetime);
	// Изменения настроек plugin.<id>.<key> доходят до плагина как on_settings_changed.
	Settings::Changed() | rpl::on_next([](const QString &key) {
		if (!key.startsWith(U("plugin."))) {
			return;
		}
		const auto entry = FindEntry(key);
		if (!entry) {
			return;
		}
		// Значение отправляем как массив из одного элемента: скаляр в JSON верхнего уровня Qt не держит.
		NotifySettingChanged(
			key.toStdString(),
			QJsonDocument(QJsonArray{ CurrentValue(*entry) })
				.toJson(QJsonDocument::Compact).toStdString(),
			[](const std::string &line) {
				LOG(("[mokagram] %1").arg(QString::fromStdString(line)));
			});
	}, BridgeLifetime);
}

bool HasMessageFilter() {
	return FilterCount.load(std::memory_order_relaxed) > 0;
}

void TransformMessageText(
		not_null<const HistoryItem*> item,
		TextWithEntities &text) {
	if (TextHookCount.load(std::memory_order_relaxed) <= 0
		|| !IsMainThread()
		|| text.text.isEmpty()
		|| item->out()) {
		return;
	}
	auto message = QJsonObject();
	message.insert("id", QString::number(item->id.bare));
	message.insert("peer", QString::number(item->history()->peer->id.value));
	message.insert("text", text.text);
	message.insert("entities", EntitiesToJson(text.entities, text.text));
	message.insert("date", double(item->date()));
	const auto answer = CallTextHook(Json(message), PluginLog);
	if (answer.size() < 1) {
		return;
	}
	if (answer[0] == 'T') {
		const auto changed = QString::fromStdString(answer.substr(1));
		if (changed != text.text) {
			text = TextWithEntities{ changed.left(8192) };
		}
	} else if (answer[0] == 'J') {
		// Плагин вернул и текст, и сущности.
		const auto object = QJsonDocument::fromJson(
			QByteArray::fromStdString(answer.substr(1))).object();
		const auto changed = object.value("text").toString().left(8192);
		text = TextWithEntities{
			changed,
			EntitiesFromJson(object.value("entities").toArray(), changed.size(), true),
		};
	}
}

QString MessageBadge(not_null<const HistoryItem*> item) {
	if (BadgeHookCount.load(std::memory_order_relaxed) <= 0
		|| !IsMainThread()) {
		return QString();
	}
	const auto id = item->fullId();
	const auto edited = TimeId(qHash(item->originalText().text) & 0x7fffffff);
	const auto i = BadgeCache.find(id);
	if (i != BadgeCache.end() && i->second.first == edited) {
		return i->second.second;
	}
	if (BadgeCache.size() > 5000) {
		BadgeCache.clear();
	}
	const auto answer = CallBadgeHook(Json(ItemBrief(item)), PluginLog);
	const auto result = QString::fromStdString(answer).left(40);
	BadgeCache[id] = { edited, result };
	return result;
}

bool HiddenByPlugin(not_null<const HistoryItem*> item) {
	if (FilterCount.load(std::memory_order_relaxed) <= 0 || !IsMainThread()) {
		return false;
	}
	const auto id = item->fullId();
	const auto i = FilterCache.find(id);
	if (i != FilterCache.end()) {
		return i->second;
	}
	if (FilterCache.size() > 5000) {
		FilterCache.clear();
	}
	const auto result = CallMessageFilter(Json(ItemBrief(item)), PluginLog);
	FilterCache[id] = result;
	return result;
}

void RunUpdateHook(const MTPUpdate &update) {
	const auto type = uint32_t(update.type());
	if (!HookWanted(HookKind::Update, type)) {
		return;
	}
	auto buffer = mtpBuffer();
	update.write<mtpBuffer>(buffer);
	QueueHook(
		HookKind::Update,
		type,
		buffer.constData(),
		size_t(buffer.size()) * sizeof(mtpPrime));
}

void RunUpdatesHook(const MTPUpdates &updates) {
	const auto type = uint32_t(updates.type());
	if (!HookWanted(HookKind::Updates, type)) {
		return;
	}
	auto buffer = mtpBuffer();
	updates.write<mtpBuffer>(buffer);
	QueueHook(
		HookKind::Updates,
		type,
		buffer.constData(),
		size_t(buffer.size()) * sizeof(mtpPrime));
}

} // namespace Mokagram::Plugins
