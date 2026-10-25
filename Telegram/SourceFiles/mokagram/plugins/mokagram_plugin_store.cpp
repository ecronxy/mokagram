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
#include <QtCore/QDateTime>
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
constexpr auto kAvatar = 48;
constexpr auto kCardPad = 16;

struct Entry {
	QString id;
	QString name;
	QString description;
	QString author;
	QString version;
	QString icon;
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
	auto request = QNetworkRequest(QUrl(QString::fromLatin1(kBaseUrl) + path
			// Уникальная приписка: иначе CDN отдаёт старый каталог со свежей подписью.
			+ u"?t="_q + QString::number(QDateTime::currentSecsSinceEpoch())));
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
			.icon = object.value("icon").toString(),
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
			const auto current = (installed == entry.version);
			const auto wrap = list->add(
				object_ptr<Ui::RpWidget>(list),
				QMargins(14, 10, 14, 0));
			const auto avatar = Ui::CreateChild<Ui::RpWidget>(wrap);
			avatar->resize(kAvatar, kAvatar);
			const auto letter = entry.icon.isEmpty()
				? entry.name.left(1).toUpper()
				: entry.icon.left(8);
			avatar->paintOn([=](QPainter &p) {
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowBgRipple);
				p.drawRoundedRect(avatar->rect(), 14, 14);
				p.setPen(st::windowFg);
				p.setFont(style::font(24, 0, 0));
				p.drawText(avatar->rect(), Qt::AlignCenter, letter);
			});
			const auto title = Ui::CreateChild<Ui::FlatLabel>(
				wrap,
				entry.name,
				st::boxTitle);
			auto subtitleText = U("v") + entry.version;
			if (!entry.author.isEmpty()) {
				subtitleText += U("  •  ") + entry.author;
			}
			const auto subtitle = Ui::CreateChild<Ui::FlatLabel>(
				wrap,
				subtitleText,
				st::boxDividerLabel);
			const auto text = Ui::CreateChild<Ui::FlatLabel>(
				wrap,
				entry.description,
				st::boxLabel);
			const auto access = entry.permissions.isEmpty()
				? nullptr
				: Ui::CreateChild<Ui::FlatLabel>(
					wrap,
					U("Доступ: ") + entry.permissions,
					st::boxDividerLabel);
			static const auto stInstall = [] {
				auto result = st::defaultActiveButton;
				result.height = 34;
				result.textTop = 7;
				result.radius = 17;
				result.fullRadius = true;
				return result;
			}();
			static const auto stDone = [] {
				auto result = st::defaultLightButton;
				result.height = 34;
				result.textTop = 7;
				result.radius = 17;
				result.fullRadius = true;
				return result;
			}();
			const auto button = Ui::CreateChild<Ui::RoundButton>(
				wrap,
				rpl::single(installed.isEmpty()
					? U("Установить")
					: (current ? U("Установлен") : U("Обновить"))),
				current ? stDone : stInstall);
			wrap->paintOn([=](QPainter &p) {
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowBg);
				p.drawRoundedRect(wrap->rect(), 22, 22);
			});
			wrap->widthValue() | rpl::on_next([=](int width) {
				const auto textLeft = kCardPad + kAvatar + 12;
				avatar->move(kCardPad, kCardPad);
				title->resizeToNaturalWidth(width - kCardPad - textLeft);
				const auto block = title->height() + 2 + subtitle->height();
				const auto top = kCardPad + (kAvatar - block) / 2;
				title->move(textLeft, top);
				subtitle->move(textLeft, top + title->height() + 2);
				auto bottom = kCardPad + kAvatar;
				text->resizeToWidth(width - 2 * kCardPad);
				text->move(kCardPad, bottom + 12);
				bottom += 12 + text->height();
				if (access) {
					access->resizeToWidth(width - 2 * kCardPad);
					access->move(kCardPad, bottom + 6);
					bottom += 6 + access->height();
				}
				button->move(width - kCardPad - button->width(), bottom + 12);
				wrap->resize(width, bottom + 12 + button->height() + kCardPad);
			}, wrap->lifetime());
			wrap->showChildren();
			if (current) {
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
		Ui::AddSkip(list, 14);
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
