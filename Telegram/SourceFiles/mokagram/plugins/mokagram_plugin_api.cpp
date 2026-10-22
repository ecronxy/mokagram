// Mokagram: SDK плагинов на стороне клиента (мост к Qt) и входы хуков обновлений.
#include "mokagram/plugins/mokagram_plugin_api.h"

#include "mokagram/plugins/mokagram_plugins.h"
#include "mokagram/settings/mokagram_settings.h"

#include "api/api_common.h"
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

std::string Bridge(const std::string &op, const std::string &argsJson) {
	const auto args = QJsonDocument::fromJson(
		QByteArray::fromStdString(argsJson)).object();
	if (op == "toast") {
		return ShowToast(args);
	} else if (op == "send_message") {
		return SendMessage(args);
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
