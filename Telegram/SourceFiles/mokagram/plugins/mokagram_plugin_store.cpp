// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: fec14fab
// Mokagram: магазин плагинов (подписанный каталог в репозитории mokagram-plugins).
#include "mokagram/plugins/mokagram_plugin_store.h"
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
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "window/window_session_controller.h"
#include "mokagram/settings/mokagram_settings.h"
#include "mokagram/settings/mokagram_settings_box.h"
#include "window/window_controller.h"
#include "core/application.h"
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
		U(Mokagram::Lang::Tr("Магазин плагинов")),
		st::boxTitle);
	header->widthValue() | rpl::on_next([=](int width) {
		close->moveToLeft(0, 0);
		title->moveToLeft((width - title->width()) / 2, 6);
	}, header->lifetime());
	Ui::AddSkip(content, 8);

	const auto status = content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			U(Mokagram::Lang::Tr("Загружаю каталог...")),
			st::boxDividerLabel),
		QMargins(24, 10, 24, 14));
	const auto list = content->add(object_ptr<Ui::VerticalLayout>(content));

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
			if (label) {
				label->setText(installsText(counts->values.value(id, 0)));
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
			const auto wrap = list->add(
				object_ptr<Ui::RpWidget>(list),
				QMargins(14, 10, 14, 0));
			const auto avatar = Ui::CreateChild<Ui::RpWidget>(wrap);
			avatar->resize(kAvatar, kAvatar);
			const auto letter = entry.icon.isEmpty()
				? entry.name.left(1).toUpper()
				: entry.icon.left(8);
			// Эмодзи значка берём из набора iOSTelegram (анимированные), пока он не загрузился или эмодзи там нет, рисуем обычный.
			auto packState = std::make_shared<std::shared_ptr<void>>();
			if (controller) {
				Settings::PreloadRowIcons(&controller->session());
				Settings::RowIconsUpdated(
				) | rpl::on_next([=] {
					avatar->update();
				}, avatar->lifetime());
			}
			const auto iconAlt = entry.icon;
			avatar->paintOn([=](QPainter &p) {
				{
					auto hq = PainterHighQualityEnabler(p);
					p.setPen(Qt::NoPen);
					p.setBrush(st::windowBgRipple);
					p.drawRoundedRect(avatar->rect(), 14, 14);
				}
				if (!iconAlt.isEmpty() && Settings::PaintPackIcon(
						p,
						avatar->rect(),
						iconAlt,
						*packState,
						avatar)) {
					return;
				}
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
			// Подробное описание: под кнопкой «Подробнее», раскрывается в карточке.
			const auto more = entry.details.isEmpty()
				? nullptr
				: Ui::CreateChild<Ui::LinkButton>(
					wrap,
					U(Mokagram::Lang::Tr("Подробнее")));
			// Полный текст создаём только при раскрытии и убираем при сворачивании.
			const auto moreText = std::make_shared<QPointer<Ui::FlatLabel>>();
			const auto expanded = std::make_shared<bool>(false);
			const auto relayout = std::make_shared<Fn<void()>>();
			const auto access = entry.permissions.isEmpty()
				? nullptr
				: Ui::CreateChild<Ui::FlatLabel>(
					wrap,
					U(Mokagram::Lang::Tr("Доступ: ")) + entry.permissions,
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
					? U(Mokagram::Lang::Tr("Установить"))
					: (current ? U(Mokagram::Lang::Tr("Установлен")) : U(Mokagram::Lang::Tr("Обновить")))),
				current ? stDone : stInstall);
			const auto installs = Ui::CreateChild<Ui::FlatLabel>(
				wrap,
				QString(),
				st::boxDividerLabel);
			installs->hide();
			counts->labels.emplace_back(entry.id, installs);
			if (counts->loaded) {
				installs->setText(installsText(counts->values.value(entry.id, 0)));
				installs->show();
			}
			wrap->paintOn([=](QPainter &p) {
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowBg);
				p.drawRoundedRect(wrap->rect(), 22, 22);
			});
			*relayout = [=] {
				const auto width = wrap->width();
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
				if (more) {
					more->move(kCardPad, bottom + 4);
					bottom += 4 + more->height();
					if (*expanded && *moreText) {
						(*moreText)->resizeToWidth(width - 2 * kCardPad);
						(*moreText)->move(kCardPad, bottom + 6);
						bottom += 6 + (*moreText)->height();
					}
				}
				if (access) {
					access->resizeToWidth(width - 2 * kCardPad);
					access->move(kCardPad, bottom + 6);
					bottom += 6 + access->height();
				}
				button->move(width - kCardPad - button->width(), bottom + 12);
				installs->moveToLeft(
					kCardPad,
					bottom + 12 + (button->height() - installs->height()) / 2);
				wrap->resize(width, bottom + 12 + button->height() + kCardPad);
			};
			if (more) {
				more->setClickedCallback([=] {
					*expanded = !*expanded;
					if (*expanded) {
						*moreText = Ui::CreateChild<Ui::FlatLabel>(
							wrap,
							entry.details,
							st::boxLabel);
						(*moreText)->show();
					} else if (*moreText) {
						(*moreText)->deleteLater();
						*moreText = nullptr;
					}
					more->setText(*expanded
						? U(Mokagram::Lang::Tr("Свернуть"))
						: U(Mokagram::Lang::Tr("Подробнее")));
					(*relayout)();
				});
			}
			wrap->widthValue() | rpl::on_next([=](int) {
				(*relayout)();
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
							Mokagram::Lang::Tr("Плагин скачался с ошибкой или не совпал с каталогом.")));
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
