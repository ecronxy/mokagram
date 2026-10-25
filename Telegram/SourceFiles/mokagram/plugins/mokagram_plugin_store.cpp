// Mokagram: магазин плагинов (подписанный каталог в репозитории mokagram-plugins).
#include "mokagram/plugins/mokagram_plugin_store.h"

#include "mokagram/plugins/mokagram_plugin_install.h"
#include "mokagram/plugins/mokagram_plugins.h"
#include "mokagram/update/mokagram_update.h"
#include "base/weak_ptr.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Mokagram::Plugins {
namespace {

constexpr auto kBaseUrl
	= "https://raw.githubusercontent.com/ecronxy/mokagram-plugins/main/";
constexpr auto kMaxIndexBytes = qint64(256 * 1024);
constexpr auto kMaxPluginBytes = qint64(2 * 1024 * 1024);
constexpr auto kTimeout = 20 * 1000;

struct Entry {
	QString id;
	QString name;
	QString description;
	QString author;
	QString version;
	QString file;
	QString permissions;
	QByteArray sha256;
	qint64 size = 0;
};

[[nodiscard]] QString U(const char *text) {
	return QString::fromUtf8(text);
}

[[nodiscard]] QNetworkAccessManager *Manager() {
	static const auto result = new QNetworkAccessManager();
	return result;
}

// Сервер не может прислать больше обещанного и увести на небезопасный адрес.
[[nodiscard]] QNetworkReply *Get(const QString &path, qint64 limit) {
	auto request = QNetworkRequest(QUrl(QString::fromLatin1(kBaseUrl) + path));
	request.setAttribute(
		QNetworkRequest::CacheLoadControlAttribute,
		QNetworkRequest::AlwaysNetwork);
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setTransferTimeout(kTimeout);
	request.setRawHeader("User-Agent", "mokaGram-store");
	const auto reply = Manager()->get(request);
	QObject::connect(reply, &QNetworkReply::downloadProgress, [=](
			qint64 received,
			qint64) {
		if (received > limit) {
			reply->abort();
		}
	});
	return reply;
}

// Только простое имя файла .plugin: ни путей, ни «..», ни схем.
[[nodiscard]] bool SafeFile(const QString &file) {
	static const auto pattern = QRegularExpression(
		QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9_.-]{0,80}\\.plugin$"));
	return pattern.match(file).hasMatch() && !file.contains(u".."_q);
}

[[nodiscard]] std::vector<Entry> ParseIndex(const QByteArray &bytes) {
	auto result = std::vector<Entry>();
	const auto document = QJsonDocument::fromJson(bytes);
	if (!document.isObject()) {
		return result;
	}
	for (const auto &value : document.object().value("plugins").toArray()) {
		const auto object = value.toObject();
		auto entry = Entry{
			.id = object.value("id").toString(),
			.name = object.value("name").toString(),
			.description = object.value("description").toString(),
			.author = object.value("author").toString(),
			.version = object.value("version").toString(),
			.file = object.value("file").toString(),
			.sha256 = QByteArray::fromHex(
				object.value("sha256").toString().toLatin1()),
			.size = qint64(object.value("size").toDouble()),
		};
		auto permissions = QStringList();
		for (const auto &item : object.value("permissions").toArray()) {
			permissions.push_back(item.toString());
		}
		entry.permissions = permissions.join(u", "_q);
		if (entry.id.isEmpty()
			|| entry.name.isEmpty()
			|| !SafeFile(entry.file)
			|| entry.sha256.size() != 32
			|| entry.size <= 0
			|| entry.size > kMaxPluginBytes) {
			continue;
		}
		result.push_back(std::move(entry));
	}
	return result;
}

[[nodiscard]] QString InstalledVersion(const QString &id) {
	const auto list = QJsonDocument::fromJson(
		QByteArray::fromStdString(ListJson())).array();
	for (const auto &value : list) {
		const auto object = value.toObject();
		if (object.value("id").toString() == id) {
			const auto version = object.value("version").toString();
			return version.isEmpty() ? u"?"_q : version;
		}
	}
	return QString();
}

} // namespace

void StoreBox(
		not_null<Ui::GenericBox*> box,
		Window::SessionController *controller,
		Fn<void()> changed) {
	RoundedWindow(box);
	const auto content = box->verticalLayout();
	const auto weak = base::make_weak(controller);
	const auto alive = base::make_weak(box);

	const auto header = content->add(
		object_ptr<Ui::RpWidget>(content),
		QMargins(10, 10, 10, 0));
	header->resize(header->width(), 40);
	const auto close = MakeHeaderButton(
		header,
		true,
		[=] { box->closeBox(); });
	const auto title = Ui::CreateChild<Ui::FlatLabel>(
		header,
		U("Магазин плагинов"),
		st::boxTitle);
	header->widthValue() | rpl::on_next([=](int width) {
		close->moveToLeft(0, 0);
		title->moveToLeft((width - title->width()) / 2, 6);
	}, header->lifetime());
	Ui::AddSkip(content, 8);

	const auto status = content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			U("Загружаю каталог..."),
			st::boxDividerLabel),
		QMargins(24, 10, 24, 14));
	const auto list = content->add(object_ptr<Ui::VerticalLayout>(content));

	const auto fail = [=](const QString &text) {
		if (!alive) {
			return;
		}
		status->setText(text);
	};
	const auto build = [=](std::vector<Entry> entries) {
		if (!alive) {
			return;
		}
		if (entries.empty()) {
			fail(U("В каталоге пока нет плагинов."));
			return;
		}
		status->setText(QString());
		status->hide();
		for (const auto &entry : entries) {
			const auto installed = InstalledVersion(entry.id);
			Ui::AddDivider(list);
			list->add(
				object_ptr<Ui::FlatLabel>(
					list,
					entry.name + u"  "_q + entry.version,
					st::boxTitle),
				QMargins(24, 12, 24, 0));
			if (!entry.description.isEmpty()) {
				list->add(
					object_ptr<Ui::FlatLabel>(
						list,
						entry.description,
						st::boxDividerLabel),
					QMargins(24, 4, 24, 0));
			}
			auto meta = QStringList();
			if (!entry.author.isEmpty()) {
				meta.push_back(entry.author);
			}
			if (!entry.permissions.isEmpty()) {
				meta.push_back(U("Доступ: ") + entry.permissions);
			}
			if (!meta.isEmpty()) {
				list->add(
					object_ptr<Ui::FlatLabel>(
						list,
						meta.join(u" · "_q),
						st::boxDividerLabel),
					QMargins(24, 2, 24, 0));
			}
			const auto label = installed.isEmpty()
				? U("Установить")
				: (installed == entry.version
					? U("Установлен")
					: U("Обновить"));
			const auto button = list->add(
				object_ptr<Ui::RoundButton>(
					list,
					rpl::single(label),
					installed == entry.version
						? st::defaultLightButton
						: st::defaultActiveButton),
				QMargins(24, 8, 24, 12));
			if (installed == entry.version) {
				button->setAttribute(Qt::WA_TransparentForMouseEvents);
				continue;
			}
			button->setClickedCallback([=] {
				button->setAttribute(Qt::WA_TransparentForMouseEvents);
				const auto reply = Get(entry.file, kMaxPluginBytes);
				QObject::connect(reply, &QNetworkReply::finished, [=] {
					reply->deleteLater();
					button->setAttribute(
						Qt::WA_TransparentForMouseEvents,
						false);
					const auto strong = weak.get();
					if (!strong) {
						return;
					}
					const auto bytes = (reply->error() == QNetworkReply::NoError)
						? reply->readAll()
						: QByteArray();
					if (bytes.size() != entry.size
						|| QCryptographicHash::hash(
							bytes,
							QCryptographicHash::Sha256) != entry.sha256) {
						strong->showToast(U(
							"Плагин скачался с ошибкой или не совпал с каталогом."));
						return;
					}
					InstallFromBytes(strong, bytes, changed);
				});
			});
		}
		box->verticalLayout()->resizeToWidth(box->width());
	};

	const auto indexReply = Get(u"index.json"_q, kMaxIndexBytes);
	QObject::connect(indexReply, &QNetworkReply::finished, [=] {
		indexReply->deleteLater();
		if (!alive) {
			return;
		}
		if (indexReply->error() != QNetworkReply::NoError) {
			fail(U("Не получилось загрузить каталог. Проверьте подключение."));
			return;
		}
		const auto index = indexReply->readAll();
		const auto signatureReply = Get(u"index.json.sig"_q, 1024);
		QObject::connect(signatureReply, &QNetworkReply::finished, [=] {
			signatureReply->deleteLater();
			if (!alive) {
				return;
			}
			const auto signature = QByteArray::fromBase64(
				signatureReply->readAll().trimmed());
			if (signatureReply->error() != QNetworkReply::NoError
				|| !Update::VerifyDetached(
					"mokagram-plugins\n" + index,
					signature)) {
				fail(U("Подпись каталога не сошлась, плагины не показываю."));
				return;
			}
			build(ParseIndex(index));
		});
	});
}

} // namespace Mokagram::Plugins
