// Mokagram: SDK плагинов на стороне клиента (мост к Qt) и входы хуков обновлений.
#include "mokagram/plugins/mokagram_plugin_api.h"

#include "mokagram/plugins/mokagram_plugins.h"
#include "mokagram/settings/mokagram_settings.h"

#include "api/api_common.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/toast/toast.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
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
#include "history/history_item.h"
#include "ui/text/text_utilities.h"
#include "apiwrap.h"
#include "core/application.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QThread>

#include <algorithm>
#include <map>
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

std::string SendMessage(const QJsonObject &args) {
	const auto peerId = PeerId(args.value("peer").toString().toULongLong());
	const auto text = args.value("text").toString();
	const auto replyTo = int64(args.value("reply_to").toDouble());
	const auto silent = args.value("silent").toBool();
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

std::string GetMessage(const QJsonObject &args) {
	return OnMain([=]() -> std::string {
		const auto item = FindItem(args);
		if (!item) {
			return Reply(false, U("сообщение не загружено"));
		}
		auto result = QJsonObject();
		result.insert("ok", true);
		result.insert("text", item->originalText().text);
		result.insert("out", item->out());
		result.insert("date", double(item->date()));
		result.insert("from", QString::number(item->from()->id.value));
		result.insert("has_media", item->media() != nullptr);
		result.insert("can_edit", item->canBeEdited());
		return QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
	});
}

std::string EditMessage(const QJsonObject &args) {
	const auto text = args.value("text").toString();
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
			TextWithEntities{ text },
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
		controller->showPeerHistory(peer);
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
		return PluginUnloaded(args);
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
	} else if (op == "setting_get") {
		return GetSetting(args);
	} else if (op == "setting_set") {
		return SetSetting(args);
	}
	return Reply(false, U("неизвестная операция"));
}

} // namespace

void InstallBridge() {
	SetBridge(Bridge);
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
