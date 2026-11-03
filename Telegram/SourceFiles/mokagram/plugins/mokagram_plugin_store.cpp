// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: fec14fab
// Mokagram: магазин плагинов (подписанный каталог в репозитории mokagram-plugins).
#include "mokagram/plugins/mokagram_plugin_store.h"
#include "mokagram/plugins/mokagram_plugin_sources.h"
#include "mokagram/lang/mokagram_lang.h"

#include "mokagram/plugins/mokagram_plugin_install.h"
#include "mokagram/plugins/mokagram_plugins.h"
#include "mokagram/update/mokagram_update.h"
#include "mokagram/misc/mokagram_misc.h"
#include "lang/lang_keys.h"
#include "base/weak_ptr.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "window/window_session_controller.h"
#include "mokagram/settings/mokagram_settings.h"
#include "mokagram/settings/mokagram_settings_box.h"
#include "window/window_controller.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "ui/widgets/fields/input_field.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>
#include <QtCore/QSet>
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
constexpr auto kCardPad = 16;
constexpr auto kAvatar = 48;

struct Entry {
	QString id;
	QString name;
	QString description;
	QString details;
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
			.details = object.value("details").toString().left(4000),
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

[[nodiscard]] QNetworkReply *GetSource(const QUrl &url, qint64 limit) {
	auto request = QNetworkRequest(url);
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setTransferTimeout(kTimeout);
	request.setRawHeader("User-Agent", "mokaGram-plugin-sources");
	const auto reply = Manager()->get(request);
	QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [=](qint64 received, qint64) {
		if (received > limit) {
			reply->abort();
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
	return reply;
}

void InstallSource(
		Window::SessionController *controller,
		const QUrl &url,
		Fn<void()> changed,
		const QByteArray &expectedHash = {},
		qint64 expectedSize = 0) {
	const auto weak = base::make_weak(controller);
	const auto reply = GetSource(url, kMaxPluginBytes);
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto bytes = reply->readAll();
		if (reply->error() != QNetworkReply::NoError
			|| !Sources::VerifyDownload(bytes, expectedHash, expectedSize, kMaxPluginBytes)) {
			strong->showToast(U(Mokagram::Lang::Tr("Не удалось загрузить плагин из источника")));
			return;
		}
		InstallFromBytes(strong, bytes, changed);
	});
}

void RepositoryBox(
		not_null<Ui::GenericBox*> box,
		Window::SessionController *controller,
		const QString &repository,
		const QString &branch,
		Fn<void()> changed) {
	box->setTitle(rpl::single(repository));
	const auto content = box->verticalLayout();
	const auto status = content->add(object_ptr<Ui::FlatLabel>(
		content,
		U(Mokagram::Lang::Tr("Загружаю каталог...")),
		st::boxLabel), st::boxPadding);
	box->addButton(rpl::single(U(Mokagram::Lang::Tr("Закрыть"))), [=] { box->closeBox(); });
	const auto weak = base::make_weak(controller);
	const auto treeUrl = QUrl(u"https://api.github.com/repos/"_q + repository
		+ u"/git/trees/"_q + QString::fromLatin1(QUrl::toPercentEncoding(branch))
		+ u"?recursive=1"_q);
	const auto reply = GetSource(treeUrl, kMaxIndexBytes);
	QObject::connect(reply, &QNetworkReply::finished, box.get(), [=] {
		const auto root = QJsonDocument::fromJson(reply->readAll()).object();
		if (reply->error() != QNetworkReply::NoError || !root.contains("tree")) {
			status->setText(U(Mokagram::Lang::Tr("Не получилось загрузить каталог. Проверьте подключение.")));
			return;
		}
		auto files = QStringList();
		for (const auto &value : root.value("tree").toArray()) {
			const auto item = value.toObject();
			const auto path = item.value("path").toString();
			if (item.value("type").toString() == u"blob"_q
				&& path.endsWith(u".plugin"_q, Qt::CaseInsensitive)
				&& !path.split('/').contains(u".."_q)) {
				files.push_back(path);
			}
		}
		files.sort(Qt::CaseInsensitive);
		status->setText(files.isEmpty()
			? U(Mokagram::Lang::Tr("В каталоге пока нет плагинов."))
			: U(Mokagram::Lang::Tr("Внешний источник. Код и доступ будут показаны перед установкой.")));
		for (const auto &path : files.mid(0, 200)) {
			const auto link = content->add(object_ptr<Ui::LinkButton>(content, path), st::boxPadding);
			link->setClickedCallback([=] {
				if (const auto strong = weak.get()) {
					const auto encoded = QString::fromLatin1(QUrl::toPercentEncoding(path, "/"));
					InstallSource(strong, QUrl(u"https://raw.githubusercontent.com/"_q
						+ repository + '/' + branch + '/' + encoded), changed);
				}
			});
		}
		if (root.value("truncated").toBool() || files.size() > 200) {
			content->add(object_ptr<Ui::FlatLabel>(content,
				U(Mokagram::Lang::Tr("Показана только часть каталога.")), st::boxDividerLabel), st::boxPadding);
		}
	});
}

void IndexBox(
		not_null<Ui::GenericBox*> box,
		Window::SessionController *controller,
		const QUrl &indexUrl,
		const QString &repository,
		const QString &branch,
		Fn<void()> changed) {
	box->setTitle(rpl::single(U(Mokagram::Lang::Tr("Каталог плагинов"))));
	const auto content = box->verticalLayout();
	const auto status = content->add(object_ptr<Ui::FlatLabel>(content,
		U(Mokagram::Lang::Tr("Загружаю каталог...")), st::boxLabel), st::boxPadding);
	box->addButton(rpl::single(U(Mokagram::Lang::Tr("Закрыть"))), [=] { box->closeBox(); });
	const auto weak = base::make_weak(controller);
	const auto reply = GetSource(indexUrl, kMaxIndexBytes);
	QObject::connect(reply, &QNetworkReply::finished, box.get(), [=] {
		const auto root = QJsonDocument::fromJson(reply->readAll()).object();
		if (reply->error() != QNetworkReply::NoError && !repository.isEmpty()) {
			if (const auto strong = weak.get()) {
				box->closeBox();
				strong->show(Box(RepositoryBox, strong, repository, branch, changed));
			}
			return;
		}
		if (reply->error() != QNetworkReply::NoError
			|| root.value("schema").toInt() != 1
			|| root.value("name").toString().isEmpty()
			|| !root.value("plugins").isArray()) {
			status->setText(U(Mokagram::Lang::Tr("Не удалось прочитать каталог плагинов")));
			return;
		}
		box->setTitle(rpl::single(root.value("name").toString().left(200)));
		status->setText(U(Mokagram::Lang::Tr("Внешний источник. Код и доступ будут показаны перед установкой.")));
		auto seen = QSet<QString>();
		for (const auto &value : root.value("plugins").toArray()) {
			const auto item = value.toObject();
			const auto parsed = Sources::ParseEntry(item, indexUrl, kMaxPluginBytes);
			if (!parsed || seen.contains(parsed->id)) {
				continue;
			}
			const auto &entry = *parsed;
			const auto &id = entry.id;
			const auto &name = entry.name;
			const auto &version = entry.version;
			const auto file = entry.url;
			const auto hash = entry.sha256;
			const auto size = entry.size;
			seen.insert(id);
			const auto link = content->add(object_ptr<Ui::LinkButton>(content,
				name.left(200) + u" · "_q + version.left(40)), st::boxPadding);
			link->setClickedCallback([=] {
				if (const auto strong = weak.get()) {
					InstallSource(strong, file, changed, hash, size);
				}
			});
			if (seen.size() >= 200) {
				content->add(object_ptr<Ui::FlatLabel>(content,
					U(Mokagram::Lang::Tr("Показана только часть каталога.")), st::boxDividerLabel), st::boxPadding);
				break;
			}
		}
		if (seen.isEmpty()) {
			status->setText(U(Mokagram::Lang::Tr("В каталоге пока нет плагинов.")));
		}
	});
}

[[nodiscard]] bool OpenSource(
		Window::SessionController *controller,
		const QString &source,
		Fn<void()> changed) {
	auto url = QUrl(source.trimmed());
	if (!Sources::IsSecure(url)) {
		return false;
	}
	if (url.host().compare(u"t.me"_q, Qt::CaseInsensitive) == 0) {
		UrlClickHandler::Open(url.toString());
		return true;
	}
	if (url.host().compare(u"github.com"_q, Qt::CaseInsensitive) == 0) {
		const auto parts = url.path().split('/', Qt::SkipEmptyParts);
		if (parts.size() >= 2) {
			auto name = parts[1];
			if (name.endsWith(u".git"_q)) {
				name.chop(4);
			}
			static const auto component = QRegularExpression(u"^[A-Za-z0-9_.-]+$"_q);
			if (!component.match(parts[0]).hasMatch() || !component.match(name).hasMatch()) {
				return false;
			}
			const auto repository = parts[0] + '/' + name;
			if (parts.size() >= 5 && parts[2] == u"blob"_q
				&& (parts.back().endsWith(u".plugin"_q, Qt::CaseInsensitive)
					|| parts.back() == u"extera-plugins.json"_q)) {
				url = QUrl(u"https://raw.githubusercontent.com/"_q + repository
					+ '/' + parts.mid(3).join('/'));
			} else if (parts.size() == 2 || (parts.size() >= 4 && parts[2] == u"tree"_q)) {
				const auto branch = parts.size() >= 4 ? parts[3] : u"HEAD"_q;
				const auto directory = parts.size() > 4 ? parts.mid(4).join('/') + '/' : QString();
				const auto index = QUrl(u"https://raw.githubusercontent.com/"_q
					+ repository + '/' + branch + '/' + directory + u"extera-plugins.json"_q);
				controller->show(Box(IndexBox, controller, index, repository, branch, changed));
				return true;
			} else {
				return false;
			}
		}
	}
	if (url.path().endsWith(u"/extera-plugins.json"_q)) {
		controller->show(Box(IndexBox, controller, url, QString(), QString(), changed));
		return true;
	}
	if (!url.path().endsWith(u".plugin"_q, Qt::CaseInsensitive)) {
		return false;
	}
	InstallSource(controller, url, changed);
	return true;
}

void SourcesBox(
		not_null<Ui::GenericBox*> box,
		Window::SessionController *controller,
		Fn<void()> changed) {
	box->setTitle(rpl::single(U(Mokagram::Lang::Tr("Источники плагинов"))));
	const auto content = box->verticalLayout();
	content->add(object_ptr<Ui::FlatLabel>(content,
		U(Mokagram::Lang::Tr("Добавьте GitHub-репозиторий, ссылку на .plugin или Telegram-канал. В канале нажмите на файл плагина для установки.")),
		st::boxLabel), st::boxPadding);
	const auto field = content->add(object_ptr<Ui::InputField>(
		content, st::defaultInputField, Ui::InputField::Mode::SingleLine,
		rpl::single(u"https://github.com/owner/repository"_q)), st::boxPadding);
	field->setInputMethodHints(Qt::ImhUrlCharactersOnly | Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText);
	const auto weak = base::make_weak(controller);
	const auto open = [=](const QString &source) {
		const auto strong = weak.get();
		if (!strong || !OpenSource(strong, source, changed)) {
			field->showError();
			return;
		}
		auto sources = Settings::String(u"plugins.sources"_q).split('\n', Qt::SkipEmptyParts);
		sources.removeAll(source);
		sources.prepend(source);
		Settings::SetString(u"plugins.sources"_q, sources.mid(0, 20).join('\n'));
	};
	for (const auto &source : Settings::String(u"plugins.sources"_q).split('\n', Qt::SkipEmptyParts).mid(0, 20)) {
		const auto link = content->add(object_ptr<Ui::LinkButton>(content, source), st::boxPadding);
		link->setClickedCallback([=] { open(source); });
	}
	box->addButton(rpl::single(U(Mokagram::Lang::Tr("Открыть"))), [=] { open(field->getLastText().trimmed()); });
	box->addButton(rpl::single(U(Mokagram::Lang::Tr("Очистить источники"))), [=] {
		Settings::SetString(u"plugins.sources"_q, QString());
		box->closeBox();
	});
	box->addButton(rpl::single(U(Mokagram::Lang::Tr("Закрыть"))), [=] { box->closeBox(); });
	box->setFocusCallback([=] { field->setFocus(); });
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
		object_ptr<Ui::FixedHeightWidget>(content, 48),
		QMargins(16, 16, 16, 0));
	const auto close = MakeHeaderButton(
		header,
		true,
		[=] { box->closeBox(); });
	const auto title = Ui::CreateChild<Ui::FlatLabel>(
		header,
		U(Mokagram::Lang::Tr("Магазин плагинов")),
		st::boxTitle);
	header->widthValue() | rpl::on_next([=](int width) {
		close->moveToLeft(0, 0);
		title->resizeToNaturalWidth(std::max(0, width - 112));
		title->moveToLeft(56, (48 - title->height()) / 2);
	}, header->lifetime());
	static const auto storeAction = [] {
		auto result = st::defaultLightButton;
		result.height = 36;
		result.textTop = 8;
		result.radius = 18;
		result.fullRadius = true;
		return result;
	}();
	const auto sources = content->add(object_ptr<Ui::RoundButton>(content,
		rpl::single(U(Mokagram::Lang::Tr("Источники плагинов"))), storeAction),
		QMargins(24, 12, 24, 12));
	sources->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	sources->setClickedCallback([=] {
		if (const auto strong = weak.get()) {
			strong->show(Box(SourcesBox, strong, changed));
		}
	});

	const auto status = content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			U(Mokagram::Lang::Tr("Загружаю каталог...")),
			st::boxDividerLabel),
		QMargins(24, 8, 24, 16));
	const auto retryArea = content->add(object_ptr<Ui::VerticalLayout>(content));
	const auto list = content->add(object_ptr<Ui::VerticalLayout>(content));
	box->setMinHeight(240);

	// Счётчики установок приходят от бота (inline «pstats»): плагин -> число людей.
	struct Counts {
		bool loaded = false;
		QHash<QString, int> values;
		std::vector<std::pair<QString, QPointer<Ui::FlatLabel>>> labels;
	};
	const auto counts = std::make_shared<Counts>();
	const auto installsText = [](int n) {
		if (::Lang::Id().startsWith(u"ru"_q)) {
			const auto mod100 = n % 100;
			const auto mod10 = n % 10;
			const auto word = (mod100 >= 11 && mod100 <= 14) ? u"установок"_q
				: (mod10 == 1) ? u"установка"_q
				: (mod10 >= 2 && mod10 <= 4) ? u"установки"_q
				: u"установок"_q;
			return QString::number(n) + ' ' + word;
		}
		return QString::number(n) + (n == 1 ? u" install"_q : u" installs"_q);
	};
	const auto updateCounts = [=] {
		for (const auto &[id, label] : counts->labels) {
			if (label && counts->values.contains(id)) {
				label->setText(installsText(counts->values.value(id)));
				label->show();
			}
		}
	};
	Mokagram::Bot::Ask(u"pstats"_q, [=](Mokagram::Bot::Results results) {
		for (const auto &result : results) {
			for (const auto &part : result.title.split(',', Qt::SkipEmptyParts)) {
				const auto at = part.lastIndexOf(':');
				if (at > 0) {
					counts->values[part.left(at)] = part.mid(at + 1).toInt();
				}
			}
		}
		counts->loaded = true;
		updateCounts();
	});

	const auto fail = [=](const QString &text) {
		if (!alive) {
			return;
		}
		status->setText(text);
		status->show();
		retryArea->clear();
		const auto retry = retryArea->add(object_ptr<Ui::RoundButton>(retryArea,
			tr::lng_bot_download_retry(), storeAction), QMargins(24, 0, 24, 20));
		retry->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		retry->setClickedCallback([=] {
			if (const auto strong = weak.get()) {
				box->closeBox();
				strong->show(Box(StoreBox, strong, changed));
			}
		});
	};
	const auto build = [=](std::vector<Entry> entries) {
		if (!alive) {
			return;
		}
		if (entries.empty()) {
			fail(U(Mokagram::Lang::Tr("В каталоге пока нет плагинов.")));
			return;
		}
		status->setText(QString());
		status->hide();
		for (const auto &entry : entries) {
			const auto installed = InstalledVersion(entry.id);
			const auto current = (installed == entry.version);
			const auto wrap = list->add(object_ptr<Ui::VerticalLayout>(list),
				QMargins(14, 10, 14, 0));
			const auto heading = wrap->add(object_ptr<Ui::FixedHeightWidget>(wrap, kAvatar),
				QMargins(kCardPad, kCardPad, kCardPad, 0));
			const auto avatar = Ui::CreateChild<Ui::RpWidget>(heading);
			avatar->resize(kAvatar, kAvatar);
			const auto letter = entry.icon.isEmpty()
				? entry.name.left(1).toUpper() : entry.icon.left(8);
			const auto packState = std::make_shared<std::shared_ptr<void>>();
			if (controller) {
				Settings::PreloadRowIcons(&controller->session());
				Settings::RowIconsUpdated() | rpl::on_next([=] {
					avatar->update();
				}, avatar->lifetime());
			}
			avatar->paintOn([=](QPainter &p) {
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowBgRipple);
				p.drawRoundedRect(avatar->rect(), 14, 14);
				if (!entry.icon.isEmpty() && Settings::PaintPackIcon(
						p, avatar->rect(), entry.icon, *packState, avatar)) {
					return;
				}
				p.setPen(st::windowFg);
				p.setFont(style::font(24, 0, 0));
				p.drawText(avatar->rect(), Qt::AlignCenter, letter);
			});
			const auto title = Ui::CreateChild<Ui::FlatLabel>(heading, entry.name, st::boxTitle);
			auto subtitleText = u"v"_q + entry.version;
			if (!entry.author.isEmpty()) {
				subtitleText += u" · "_q + entry.author;
			}
			const auto subtitle = Ui::CreateChild<Ui::FlatLabel>(heading,
				subtitleText, st::boxDividerLabel);
			heading->widthValue() | rpl::on_next([=](int width) {
				const auto left = kAvatar + 12;
				const auto available = std::max(0, width - left);
				title->resizeToNaturalWidth(available);
				subtitle->resizeToNaturalWidth(available);
				const auto top = (kAvatar - title->height() - 2 - subtitle->height()) / 2;
				title->moveToLeft(left, top);
				subtitle->moveToLeft(left, top + title->height() + 2);
			}, heading->lifetime());
			heading->showChildren();
			if (!entry.description.isEmpty()) {
				wrap->add(object_ptr<Ui::FlatLabel>(wrap,
					entry.description, st::boxLabel),
					QMargins(kCardPad, 12, kCardPad, 0));
			}
			if (!entry.details.isEmpty()) {
				const auto more = wrap->add(object_ptr<Ui::LinkButton>(wrap,
					U(Mokagram::Lang::Tr("Подробнее"))),
					QMargins(kCardPad, 8, kCardPad, 0));
				const auto details = wrap->add(object_ptr<Ui::VerticalLayout>(wrap));
				const auto expanded = more->lifetime().make_state<bool>(false);
				more->setClickedCallback([=] {
					*expanded = !*expanded;
					details->clear();
					if (*expanded) {
						details->add(object_ptr<Ui::FlatLabel>(details,
							entry.details, st::boxLabel),
							QMargins(kCardPad, 8, kCardPad, 0));
					}
					more->setText(U(Mokagram::Lang::Tr(
						*expanded ? "Свернуть" : "Подробнее")));
				});
			}
			if (!entry.permissions.isEmpty()) {
				wrap->add(object_ptr<Ui::FlatLabel>(wrap,
					U(Mokagram::Lang::Tr("Доступ: ")) + entry.permissions,
					st::boxDividerLabel), QMargins(kCardPad, 8, kCardPad, 0));
			}
			static const auto installStyle = [] {
				auto result = st::defaultActiveButton;
				result.height = 36;
				result.textTop = 8;
				result.radius = 18;
				result.fullRadius = true;
				return result;
			}();
			const auto actions = wrap->add(object_ptr<Ui::FixedHeightWidget>(wrap, 36),
				QMargins(kCardPad, 16, kCardPad, kCardPad));
			const auto button = Ui::CreateChild<Ui::RoundButton>(actions,
				rpl::single(U(Mokagram::Lang::Tr(installed.isEmpty()
					? "Установить" : (current ? "Установлен" : "Обновить")))),
				current ? storeAction : installStyle);
			button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
			button->setDisabled(current);
			const auto installs = Ui::CreateChild<Ui::FlatLabel>(actions,
				QString(), st::boxDividerLabel);
			installs->hide();
			counts->labels.emplace_back(entry.id, installs);
			if (counts->loaded && counts->values.contains(entry.id)) {
				installs->setText(installsText(counts->values.value(entry.id)));
				installs->show();
			}
			actions->widthValue() | rpl::on_next([=](int width) {
				button->moveToRight(0, 0);
				installs->resizeToWidth(std::max(0, width - button->width() - 12));
				installs->moveToLeft(0, (36 - installs->height()) / 2);
			}, actions->lifetime());
			actions->showChildren();
			installs->setVisible(counts->loaded && counts->values.contains(entry.id));
			wrap->paintOn([=](QPainter &p) {
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowBg);
				p.drawRoundedRect(wrap->rect(), 22, 22);
			});
			if (current) {
				button->setAttribute(Qt::WA_TransparentForMouseEvents);
				continue;
			}
			button->setClickedCallback([=] {
				button->setAttribute(Qt::WA_TransparentForMouseEvents);
				const auto reply = Get(entry.file, kMaxPluginBytes);
				QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
				QObject::connect(reply, &QNetworkReply::finished, button, [=] {
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
							Mokagram::Lang::Tr("Плагин скачался с ошибкой или не совпал с каталогом.")));
						return;
					}
					InstallFromBytes(strong, bytes, changed);
				});
			});
		}
		Ui::AddSkip(list, 12);
		box->verticalLayout()->resizeToWidth(box->width());
	};

	const auto indexReply = Get(u"index.json"_q, kMaxIndexBytes);
	QObject::connect(indexReply, &QNetworkReply::finished, [=] {
		indexReply->deleteLater();
		if (!alive) {
			return;
		}
		if (indexReply->error() != QNetworkReply::NoError) {
			fail(U(Mokagram::Lang::Tr("Не получилось загрузить каталог. Проверьте подключение.")));
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
				fail(U(Mokagram::Lang::Tr("Подпись каталога не сошлась, плагины не показываю.")));
				return;
			}
			build(ParseIndex(index));
		});
	});
}


// ---- автообновление ----

namespace {

[[nodiscard]] bool VersionNewer(const QString &catalog, const QString &installed) {
	const auto a = catalog.split('.');
	const auto b = installed.split('.');
	for (auto i = 0; i != std::max(a.size(), b.size()); ++i) {
		const auto x = (i < a.size()) ? a[i].toInt() : 0;
		const auto y = (i < b.size()) ? b[i].toInt() : 0;
		if (x != y) {
			return x > y;
		}
	}
	return false;
}

[[nodiscard]] QStringList InstalledPermissions(const QString &id) {
	auto result = QStringList();
	const auto list = QJsonDocument::fromJson(
		QByteArray::fromStdString(ListJson())).array();
	for (const auto &value : list) {
		const auto object = value.toObject();
		if (object.value("id").toString() == id) {
			for (const auto &item : object.value("permissions").toArray()) {
				result.push_back(item.toString());
			}
		}
	}
	return result;
}

void Notify(const QString &text) {
	if (const auto window = Core::App().activeWindow()) {
		if (const auto controller = window->sessionController()) {
			controller->showToast(text);
		}
	}
}

// Ставит файл поверх старого без окна: только если права не выросли.
// Возвращает true, если обновил.
[[nodiscard]] bool UpdateSilently(const Entry &entry, const QByteArray &bytes) {
	const auto source = QString::fromUtf8(bytes).toStdString();
	const auto info = QJsonDocument::fromJson(QByteArray::fromStdString(
		ParseMetadataJson(source))).object();
	const auto meta = info.value("meta").toObject();
	if (!info.value("error").isNull() && !info.value("error").toString().isEmpty()) {
		return false;
	}
	if (meta.value("__id__").toString() != entry.id) {
		return false;
	}
	const auto have = InstalledPermissions(entry.id);
	for (const auto &item : meta.value("__permissions__").toArray()) {
		if (!have.contains(item.toString())) {
			return false;
		}
	}
	static const auto bad = QRegularExpression("[^A-Za-z0-9_.-]");
	auto id = entry.id;
	const auto fileName = id.replace(bad, "_") + u".plugin"_q;
	const auto path = QDir(QString::fromStdString(PluginsDir())).filePath(fileName);
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
		|| file.write(bytes) != bytes.size()) {
		return false;
	}
	file.close();
	const auto log = [](const std::string &line) {
		LOG(("[mokagram] %1").arg(QString::fromStdString(line)));
	};
	RecordInstall(fileName.toStdString(), log);
	LoadFile(fileName.toStdString(), log);
	return true;
}

} // namespace

void CheckPluginUpdates() {
	if (!Mokagram::Settings::Bool(u"plugins.autoupdate"_q)) {
		LOG(("[mokagram] plugin updates: off"));
		return;
	}
	LOG(("[mokagram] plugin updates: checking"));
	const auto indexReply = Get(u"index.json"_q, kMaxIndexBytes);
	QObject::connect(indexReply, &QNetworkReply::finished, [=] {
		indexReply->deleteLater();
		if (indexReply->error() != QNetworkReply::NoError) {
			return;
		}
		const auto index = indexReply->readAll();
		const auto signatureReply = Get(u"index.json.sig"_q, 1024);
		QObject::connect(signatureReply, &QNetworkReply::finished, [=] {
			signatureReply->deleteLater();
			const auto signature = QByteArray::fromBase64(
				signatureReply->readAll().trimmed());
			if (signatureReply->error() != QNetworkReply::NoError
				|| !Update::VerifyDetached("mokagram-plugins\n" + index, signature)) {
				return;
			}
			const auto needApproval = std::make_shared<QStringList>();
			const auto updated = std::make_shared<QStringList>();
			const auto pending = std::make_shared<int>(0);
			const auto finish = [=] {
				if (--*pending > 0) {
					return;
				}
				if (!updated->isEmpty()) {
					Notify(U(Mokagram::Lang::Tr("Плагины обновлены: ")) + updated->join(u", "_q));
				}
				if (!needApproval->isEmpty()) {
					Notify(U(Mokagram::Lang::Tr("Есть обновления, которым нужны новые права: "))
						+ needApproval->join(u", "_q)
						+ U(Mokagram::Lang::Tr(". Откройте магазин плагинов.")));
				}
			};
			for (const auto &entry : ParseIndex(index)) {
				const auto installed = InstalledVersion(entry.id);
				LOG(("[mokagram] plugin updates: %1 catalog %2 installed %3").arg(entry.id, entry.version, installed));
				if (installed.isEmpty() || !VersionNewer(entry.version, installed)) {
					continue;
				}
				++*pending;
				const auto reply = Get(entry.file, kMaxPluginBytes);
				QObject::connect(reply, &QNetworkReply::finished, [=] {
					reply->deleteLater();
					const auto bytes = (reply->error() == QNetworkReply::NoError)
						? reply->readAll()
						: QByteArray();
					if (bytes.size() == entry.size
						&& QCryptographicHash::hash(
							bytes,
							QCryptographicHash::Sha256) == entry.sha256) {
						if (UpdateSilently(entry, bytes)) {
							updated->push_back(entry.name);
						} else {
							needApproval->push_back(entry.name);
						}
					}
					finish();
				});
			}
		});
	});
}

void StartPluginAutoUpdate() {
	static auto first = QTimer();
	static auto repeat = QTimer();
	first.setSingleShot(true);
	QObject::connect(&first, &QTimer::timeout, [] {
		CheckPluginUpdates();
		repeat.start();
	});
	repeat.setInterval(12 * 3600 * 1000);
	QObject::connect(&repeat, &QTimer::timeout, [] { CheckPluginUpdates(); });
	first.start(90 * 1000);
}

} // namespace Mokagram::Plugins
