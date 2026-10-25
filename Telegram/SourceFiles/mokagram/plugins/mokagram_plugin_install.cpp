// Mokagram: окно установки плагина при клике на файл .plugin в чате.
#include "mokagram/plugins/mokagram_plugin_install.h"
#include "base/call_delayed.h"

#include "mokagram/plugins/mokagram_plugins.h"
#include "mokagram/plugins/mokagram_plugin_store.h"

#include "core/application.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_session.h"
#include "data/stickers/data_custom_emoji.h"
#include "apiwrap.h"
#include "mtproto/sender.h"
#include "ui/text/text_custom_emoji.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "core/click_handler_types.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/widgets/fields/input_field.h"
#include "core/file_utilities.h"
#include "ui/effects/animations.h"
#include "history/history.h"
#include "history/history_item.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "ui/widgets/popup_menu.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/vertical_list.h"
#include "ui/abstract_button.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "styles/style_menu_icons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "styles/style_settings.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QDir>
#include <QtCore/QMimeData>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <map>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>

namespace Mokagram::Plugins {
namespace {

rpl::lifetime PendingLoad;

struct Info {
	QString id;
	QString name;
	QString description;
	QString author;
	QString version;
	QString icon;
	QStringList declared;
	QStringList detected;
	QString error;
	bool permissionsDeclared = false;
};

[[nodiscard]] QString U(const char *text) {
	return QString::fromUtf8(text);
}

[[nodiscard]] Info ParseInfo(const QByteArray &bytes) {
	const auto json = ParseMetadataJson(
		std::string(bytes.constData(), size_t(bytes.size())));
	const auto root = QJsonDocument::fromJson(
		QByteArray::fromStdString(json)).object();
	auto result = Info();
	if (!root.value("error").isNull() && !root.value("error").isUndefined()) {
		result.error = root.value("error").toString();
	}
	const auto meta = root.value("meta").toObject();
	result.id = meta.value("__id__").toString();
	result.name = meta.value("__name__").toString();
	result.description = meta.value("__description__").toString();
	result.author = meta.value("__author__").toString();
	result.version = meta.value("__version__").toString();
	result.icon = meta.value("__icon__").toString();
	if (meta.contains("__permissions__")) {
		result.permissionsDeclared = true;
		for (const auto &value : meta.value("__permissions__").toArray()) {
			result.declared.push_back(value.toString());
		}
	}
	for (const auto &value : root.value("detected").toArray()) {
		result.detected.push_back(value.toString());
	}
	return result;
}

[[nodiscard]] QString PermissionTitle(const QString &key) {
	if (key == "messages.send") return U("отправка сообщений");
	if (key == "messages.read") return U("чтение сообщений и сведений о чатах");
	if (key == "messages.edit") return U("правка ваших сообщений");
	if (key == "network") return U("доступ в интернет");
	if (key == "files") return U("доступ к файлам");
	if (key == "native") return U("запуск программ и нативного кода");
	if (key == "ui") return U("изменение интерфейса");
	if (key == "hooks") return U("перехват действий клиента");
	if (key == "settings") return U("свои настройки");
	if (key == "intents") return U("действия других приложений");
	if (key == "hooks.observe") return U("наблюдение за обновлениями и запросами");
	if (key == "hooks.intercept") return U("перехват и изменение действий клиента");
	if (key == "messages.edit") return U("правка сообщений");
	if (key == "messages.delete") return U("удаление сообщений");
	if (key == "dialogs.read") return U("список чатов");
	if (key == "storage") return U("своё хранилище");
	if (key == "background") return U("фоновые задачи");
	return key;
}

[[nodiscard]] QString SafeFileId(const QString &id) {
	static const auto bad = QRegularExpression("[^A-Za-z0-9_.-]");
	return QString(id).replace(bad, "_");
}

[[nodiscard]] QString TargetPath(const Info &info) {
	const auto dir = QDir(QString::fromStdString(PluginsDir()));
	return dir.filePath(SafeFileId(info.id) + ".plugin");
}

[[nodiscard]] QString AvatarText(const Info &info) {
	const auto icon = info.icon.trimmed();
	if (!icon.isEmpty() && !icon.contains('/')) {
		return icon.left(8);
	}
	const auto name = info.name.isEmpty() ? info.id : info.name;
	return name.isEmpty() ? QString() : name.left(1).toUpper();
}

struct AvatarState {
	std::unique_ptr<Ui::Text::CustomEmoji> emoji;
	mtpRequestId requestId = 0;
};

// __icon__ вида "ИмяНабора/номер" (как в exteraGram) или обычный эмодзи.
void AddAvatar(
		not_null<Ui::VerticalLayout*> content,
		Window::SessionController *controller,
		const Info &info) {
	constexpr auto size = 104;
	constexpr auto full = 104;
	constexpr auto emojiSize = 96;
	const auto wrap = content->add(
		object_ptr<Ui::RpWidget>(content),
		QMargins(0, 28, 0, 14));
	wrap->resize(wrap->width(), full);
	const auto avatar = Ui::CreateChild<Ui::RpWidget>(wrap);
	avatar->resize(full, full);
	wrap->widthValue() | rpl::on_next([=](int width) {
		avatar->move((width - full) / 2, 0);
	}, wrap->lifetime());

	const auto text = AvatarText(info);
		const auto state = std::make_shared<AvatarState>();

	const auto slash = info.icon.indexOf('/');
	if (controller && slash > 0) {
		const auto shortName = info.icon.left(slash).trimmed();
		const auto index = info.icon.mid(slash + 1).trimmed().toInt();
		const auto session = &controller->session();
		state->requestId = session->api().request(MTPmessages_GetStickerSet(
			MTP_inputStickerSetShortName(MTP_string(shortName)),
			MTP_int(0)
		)).done([=](const MTPmessages_StickerSet &result) {
			state->requestId = 0;
			result.match([&](const MTPDmessages_stickerSet &data) {
				const auto &list = data.vdocuments().v;
				if (index < 0 || index >= list.size()) {
					return;
				}
				const auto document = session->data().processDocument(
					list[index]);
				state->emoji = session->data().customEmojiManager().create(
					document,
					[=] { avatar->update(); },
					Data::CustomEmojiSizeTag::Normal,
					emojiSize);
				avatar->update();
			}, [](const auto &) {});
		}).fail([=] {
			state->requestId = 0;
		}).send();
		avatar->lifetime().add([=] {
			if (state->requestId) {
				session->api().request(state->requestId).cancel();
			}
		});
	}

	avatar->paintOn([=](QPainter &p) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setPen(Qt::NoPen);
		if (state->emoji) {
			state->emoji->paint(p, {
				.textColor = QColor(Qt::white),
				.now = crl::now(),
				.position = QPoint(
					(size - emojiSize) / 2,
					(size - emojiSize) / 2),
			});
		} else if (slash < 0) {
			p.setPen(Qt::white);
			p.setFont(style::font(80, 0, 0));
			p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, text);
		}
	});
}

[[nodiscard]] not_null<Ui::FlatLabel*> AddCenteredLabel(
		not_null<Ui::VerticalLayout*> content,
		const QString &text,
		const style::FlatLabel &st,
		const QMargins &margins) {
	return content->add(
		object_ptr<Ui::FlatLabel>(content, text, st),
		margins,
		style::al_top);
}

void AddButtons(
		not_null<Ui::VerticalLayout*> content,
		const QString &cancelText,
		const QString &confirmText,
		Fn<void()> cancel,
		Fn<void()> confirm) {
	static const auto pill = [](const style::RoundButton &base) {
		auto result = base;
		result.height = 48;
		result.textTop = 14;
		result.radius = 24;
		result.fullRadius = true;
		return result;
	};
	static const auto stActive = pill(st::defaultActiveButton);
	static const auto stLight = [] {
		auto result = pill(st::defaultLightButton);
		result.textBg = st::windowBg;
		result.textBgOver = st::windowBgRipple;
		return result;
	}();
	const auto row = content->add(
		object_ptr<Ui::RpWidget>(content),
		QMargins(24, 22, 24, 22));
	row->resize(row->width(), 48);
	const auto no = Ui::CreateChild<Ui::RoundButton>(
		row,
		rpl::single(cancelText),
		stLight);
	const auto yes = Ui::CreateChild<Ui::RoundButton>(
		row,
		rpl::single(confirmText),
		stActive);
	no->setClickedCallback(std::move(cancel));
	yes->setClickedCallback(std::move(confirm));
	row->widthValue() | rpl::on_next([=](int width) {
		const auto half = (width - 10) / 2;
		no->setFullWidth(half);
		yes->setFullWidth(width - 10 - half);
		no->moveToLeft(0, 0);
		yes->moveToRight(0, 0);
	}, row->lifetime());
}

void FillInstallBox(
		not_null<Ui::GenericBox*> box,
		Window::SessionController *controller,
		const Info &info,
		const QString &installed,
		Fn<void()> install) {
	// Своих кнопок у окна нет: убираем зарезервированное под них место.
	static const auto clear = style::owned_color(QColor(0, 0, 0, 0));
	static const auto noButtons = [] {
		auto result = st::defaultBox;
		result.buttonPadding = style::margins();
		result.buttonHeight = 0;
		result.bg = clear.color();
		return result;
	}();
	box->setStyle(noButtons);
	box->paintOn([=](QPainter &p) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::boxBg);
		p.drawRoundedRect(box->rect(), 28, 28);
	});
	box->setWidth(st::boxWidth);
	box->setNoContentMargin(true);
	const auto content = box->verticalLayout();
	const auto name = info.name.isEmpty() ? info.id : info.name;

	AddAvatar(content, controller, info);
	AddCenteredLabel(content, name, st::boxTitle, QMargins(24, 0, 24, 4));
	// Юзернейм берём из автора: «@name», «name» или «Имя (@name)».
	static const auto handle = QRegularExpression("@([A-Za-z0-9_]{4,32})");
	static const auto bare = QRegularExpression("^[A-Za-z0-9_]{4,32}$");
	auto username = QString();
	auto rest = info.author.trimmed();
	if (const auto m = handle.match(rest); m.hasMatch()) {
		username = m.captured(1);
		rest.remove(m.capturedStart(0), m.capturedLength(0));
		rest = rest.remove(QRegularExpression("[()\\[\\]]")).trimmed();
	} else if (bare.match(rest).hasMatch()) {
		username = rest;
		rest.clear();
	}
	const auto linkable = !username.isEmpty();
	auto lead = QString();
	if (!info.version.isEmpty()) {
		lead = U("Версия ") + info.version;
	}
	if (!lead.isEmpty() && !info.author.isEmpty()) {
		lead += U("  •  ");
	}
	lead += rest;
	if (!lead.isEmpty() || linkable) {
		const auto row = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(24, 0, 24, 0));
		const auto first = lead.isEmpty()
			? nullptr
			: Ui::CreateChild<Ui::FlatLabel>(
				row,
				lead,
				st::boxDividerLabel);
		if (first) {
			first->resizeToNaturalWidth(st::boxWidth);
		}
		static const auto linkStyle = [] {
			auto result = st::defaultLinkButton;
			result.color = st::windowSubTextFg;
			result.overColor = st::windowFg;
			result.font = st::linkFontOver;
			result.overFont = st::linkFontOver;
			return result;
		}();
		const auto link = linkable
			? Ui::CreateChild<Ui::LinkButton>(
				row,
				'@' + username,
				linkStyle)
			: nullptr;
		if (link) {
			link->setClickedCallback([=] {
				UrlClickHandler::Open(U("https://t.me/") + username);
			});
		}
		const auto firstWidth = first
			? (first->width() + (link ? 6 : 0))
			: 0;
		const auto linkWidth = link ? link->width() : 0;
		const auto height = std::max(
			first ? first->height() : 0,
			link ? link->height() : 0);
		row->resize(row->width(), height);
		row->widthValue() | rpl::on_next([=](int width) {
			auto left = (width - firstWidth - linkWidth) / 2;
			if (first) {
				first->moveToLeft(left, 0);
			}
			if (link) {
				link->moveToLeft(left + firstWidth, 0);
			}
		}, row->lifetime());
	}
	if (!info.description.isEmpty()) {
		const auto panel = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(16, 18, 16, 0));
		const auto label = Ui::CreateChild<Ui::FlatLabel>(
			panel,
			info.description,
			st::boxLabel);
		panel->paintOn([=](QPainter &p) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBg);
			p.drawRoundedRect(panel->rect(), 22, 22);
		});
		panel->widthValue() | rpl::on_next([=](int width) {
			label->resizeToWidth(width - 36);
			label->moveToLeft(18, 14);
			panel->resize(width, label->height() + 28);
		}, panel->lifetime());
	}
	if (!installed.isEmpty()) {
		AddCenteredLabel(
			content,
			U("Уже установлена версия ") + installed + U(", она будет заменена"),
			st::boxDividerLabel,
			QMargins(24, 10, 24, 0));
	}

	AddButtons(
		content,
		U("Отмена"),
		U(installed.isEmpty() ? "Установить" : "Обновить"),
		[=] { box->closeBox(); },
		[=] { install(); box->closeBox(); });

}

void Install(
		Window::SessionController *controller,
		const Info &info,
		const QByteArray &bytes) {
	const auto path = TargetPath(info);
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QFile(path);
	const auto ok = file.open(QIODevice::WriteOnly | QIODevice::Truncate)
		&& (file.write(bytes) == bytes.size());
	file.close();
	auto loadError = QString();
	if (ok) {
		const auto fileName = QFileInfo(path).fileName().toStdString();
		const auto log = [](const std::string &line) {
			LOG(("[mokagram] %1").arg(QString::fromStdString(line)));
		};
		// Хеш файла и выданные права запоминаются до запуска: иначе файл считался бы изменённым.
		RecordInstall(fileName, log);
		const auto id = LoadFile(fileName, log);
		const auto list = QJsonDocument::fromJson(
			QByteArray::fromStdString(ListJson())).array();
		for (const auto &value : list) {
			const auto entry = value.toObject();
			if (entry.value("id").toString().toStdString() == id
				&& entry.value("state").toString() == "error") {
				loadError = entry.value("error").toString();
			}
		}
	}
	if (controller) {
		const auto name = info.name.isEmpty() ? info.id : info.name;
		controller->showToast(!ok
			? U("Не удалось сохранить плагин")
			: loadError.isEmpty()
			? U("Плагин «") + name + U("» установлен")
			: U("Плагин «") + name + U("» не запустился: ") + loadError);
	}
}

void ShowInstallBox(
		Window::SessionController *controller,
		const QByteArray &bytes,
		Fn<void()> installed = nullptr) {
	const auto info = ParseInfo(bytes);
	if (!info.error.isEmpty() || info.id.isEmpty()) {
		controller->uiShow()->show(Ui::MakeInformBox(
			info.error.isEmpty()
				? U("В файле нет обязательного поля __id__. "
					"Это не похоже на плагин Mokagram.")
				: (U("Не удалось прочитать плагин: ") + info.error)));
		return;
	}
	auto version = QString();
	if (QFile::exists(TargetPath(info))) {
		auto existing = QFile(TargetPath(info));
		if (existing.open(QIODevice::ReadOnly)) {
			version = ParseInfo(existing.readAll()).version;
			if (version.isEmpty()) {
				version = U("(без номера)");
			}
		}
	}
	const auto weak = base::make_weak(controller);
	controller->uiShow()->show(Box([=](not_null<Ui::GenericBox*> box) {
		FillInstallBox(box, weak.get(), info, version, [=] {
			Install(weak.get(), info, bytes);
			if (installed) {
				installed();
			}
		});
	}));
}

} // namespace

void InstallFromBytes(
		Window::SessionController *controller,
		const QByteArray &bytes,
		Fn<void()> installed) {
	ShowInstallBox(controller, bytes, std::move(installed));
}

bool IsPluginDocument(not_null<DocumentData*> document) {
	return document->filename().endsWith(
		QStringLiteral(".plugin"),
		Qt::CaseInsensitive);
}

void OpenPluginDocument(
		Window::SessionController *controller,
		not_null<DocumentData*> document,
		Data::FileOrigin origin) {
	if (!controller) {
		if (const auto window = Core::App().activeWindow()) {
			controller = window->sessionController();
		}
	}
	if (!controller) {
		return;
	}
	const auto media = document->createMediaView();
	const auto weak = base::make_weak(controller);
	const auto finish = [=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		auto bytes = media->bytes();
		if (bytes.isEmpty()) {
			auto file = QFile(document->filepath(true));
			if (file.open(QIODevice::ReadOnly)) {
				bytes = file.readAll();
			}
		}
		if (bytes.isEmpty()) {
			strong->showToast(U("Не удалось прочитать файл плагина"));
			return;
		}
		ShowInstallBox(strong, bytes);
	};
	if (media->loaded(true)) {
		finish();
		return;
	}
	PendingLoad.destroy();
	document->session().downloaderTaskFinished(
	) | rpl::filter([=] {
		return media->loaded(true);
	}) | rpl::take(1) | rpl::on_next(finish, PendingLoad);
	document->save(origin, QString());
}

bool RunSendMessageHook(TextWithTags &text, uint64 peerId) {
	// Сообщение самого плагина (SDK send_message) через свой же хук не гоняем: петля.
	if (IsFromPlugin()) {
		return true;
	}
	auto params = QJsonObject();
	params.insert("text", text.text);
	params.insert("peer", QString::number(peerId));
	const auto answer = CallHook(
		"send_message_hook",
		QJsonDocument(params).toJson(QJsonDocument::Compact).toStdString(),
		[](const std::string &line) {
			LOG(("[mokagram] %1").arg(QString::fromStdString(line)));
		});
	if (answer.empty()) {
		return true;
	}
	const auto root = QJsonDocument::fromJson(
		QByteArray::fromStdString(answer)).object();
	if (root.value("cancel").toBool()) {
		return false;
	}
	const auto changed = root.value("value").toObject().value("text");
	if (changed.isString() && changed.toString() != text.text) {
		text.text = changed.toString();
		text.tags.clear();
	}
	return true;
}

namespace {

constexpr auto kCardPad = 16;
constexpr auto kAvatar = 48;

void RoundedNoButtons(not_null<Ui::GenericBox*> box) {
	static const auto clear = style::owned_color(QColor(0, 0, 0, 0));
	static const auto noButtons = [] {
		auto result = st::defaultBox;
		result.buttonPadding = style::margins();
		result.buttonHeight = 0;
		result.bg = clear.color();
		return result;
	}();
	box->setStyle(noButtons);
	box->paintOn([=](QPainter &p) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::boxBg);
		p.drawRoundedRect(box->rect(), 28, 28);
	});
	box->setWidth(st::boxWideWidth);
	box->setNoContentMargin(true);
}

// Переключатель в стиле Material 3: дорожка 52x32, при включении бегунок растёт.
[[nodiscard]] not_null<Ui::AbstractButton*> MakeSwitch(
		not_null<QWidget*> parent,
		bool checked,
		Fn<void(bool)> changed) {
	struct State {
		Ui::Animations::Simple animation;
		bool checked = false;
	};
	const auto button = Ui::CreateChild<Ui::AbstractButton>(parent.get());
	const auto state = button->lifetime().make_state<State>();
	state->checked = checked;
	button->resize(40, 24);
	button->setPointerCursor(true);
	button->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(button);
		auto hq = PainterHighQualityEnabler(p);
		const auto t = state->animation.value(state->checked ? 1. : 0.);
		const auto on = st::windowBgActive->c;
		const auto off = st::windowSubTextFg->c;
		const auto track = QRectF(1, 1, 38, 22);
		p.setBrush(anim::color(QColor(0, 0, 0, 0), on, t));
		p.setPen(QPen(anim::color(off, on, t), 1.6));
		p.drawRoundedRect(track, 11, 11);
		const auto size = 10. + 8. * t;
		const auto cx = 12. + 16. * t;
		p.setPen(Qt::NoPen);
		p.setBrush(anim::color(off, st::windowFgActive->c, t));
		p.drawEllipse(QPointF(cx, 12.), size / 2, size / 2);
	}, button->lifetime());
	button->setClickedCallback([=] {
		state->checked = !state->checked;
		state->animation.start(
			[=] { button->update(); },
			state->checked ? 0. : 1.,
			state->checked ? 1. : 0.,
			200);
		changed(state->checked);
	});
	return button;
}

// Автор: «@name», «name» или «Имя (@name)». Возвращает остаток без юзернейма.
[[nodiscard]] QString SplitAuthor(const QString &author, QString &username) {
	static const auto handle = QRegularExpression("@([A-Za-z0-9_]{4,32})");
	static const auto bare = QRegularExpression("^[A-Za-z0-9_]{4,32}$");
	auto rest = author.trimmed();
	username.clear();
	if (const auto m = handle.match(rest); m.hasMatch()) {
		username = m.captured(1);
		rest.remove(m.capturedStart(0), m.capturedLength(0));
		rest = rest.remove(QRegularExpression("[()\\[\\]]")).trimmed();
	} else if (bare.match(rest).hasMatch()) {
		username = rest;
		rest.clear();
	}
	return rest;
}

// «v1.0 • @автор»: юзернейм серый, подчёркнутый, ведёт на профиль.
[[nodiscard]] not_null<Ui::RpWidget*> MakeSubtitle(
		not_null<QWidget*> parent,
		const QString &version,
		const QString &author) {
	auto username = QString();
	const auto rest = SplitAuthor(author, username);
	auto lead = version.isEmpty() ? QString() : U("v") + version;
	if (!lead.isEmpty() && !author.isEmpty()) {
		lead += U("  •  ");
	}
	lead += rest;
	const auto row = Ui::CreateChild<Ui::RpWidget>(parent.get());
	const auto first = lead.isEmpty()
		? nullptr
		: Ui::CreateChild<Ui::FlatLabel>(row, lead, st::boxDividerLabel);
	if (first) {
		first->resizeToNaturalWidth(st::boxWideWidth);
		first->moveToLeft(0, 0);
	}
	static const auto linkStyle = [] {
		auto result = st::defaultLinkButton;
		result.color = st::windowSubTextFg;
		result.overColor = st::windowFg;
		result.font = st::linkFontOver;
		result.overFont = st::linkFontOver;
		return result;
	}();
	auto width = first ? first->width() + 6 : 0;
	auto height = first ? first->height() : 0;
	if (!username.isEmpty()) {
		const auto link = Ui::CreateChild<Ui::LinkButton>(
			row,
			'@' + username,
			linkStyle);
		link->setClickedCallback([=] {
			UrlClickHandler::Open(U("https://t.me/") + username);
		});
		link->moveToLeft(width, 0);
		width += link->width();
		height = std::max(height, link->height());
	}
	row->resize(width, height);
	return row;
}

struct IconState {
	std::unique_ptr<Ui::Text::CustomEmoji> emoji;
	mtpRequestId requestId = 0;
};

// __icon__ вида "ИмяНабора/номер": подгружает эмодзи из набора.
[[nodiscard]] std::shared_ptr<IconState> LoadSetEmoji(
		Window::SessionController *controller,
		const QString &icon,
		int emojiSize,
		not_null<Ui::RpWidget*> target) {
	auto result = std::make_shared<IconState>();
	const auto slash = icon.indexOf('/');
	if (!controller || slash <= 0) {
		return result;
	}
	const auto shortName = icon.left(slash).trimmed();
	const auto index = icon.mid(slash + 1).trimmed().toInt();
	const auto session = &controller->session();
	const auto state = result;
	state->requestId = session->api().request(MTPmessages_GetStickerSet(
		MTP_inputStickerSetShortName(MTP_string(shortName)),
		MTP_int(0)
	)).done([=](const MTPmessages_StickerSet &set) {
		state->requestId = 0;
		set.match([&](const MTPDmessages_stickerSet &data) {
			const auto &list = data.vdocuments().v;
			if (index < 0 || index >= list.size()) {
				return;
			}
			const auto document = session->data().processDocument(list[index]);
			state->emoji = session->data().customEmojiManager().create(
				document,
				[=] { target->update(); },
				Data::CustomEmojiSizeTag::Normal,
				emojiSize);
			target->update();
		}, [](const auto &) {});
	}).fail([=] {
		state->requestId = 0;
	}).send();
	target->lifetime().add([=] {
		if (state->requestId) {
			session->api().request(state->requestId).cancel();
		}
	});
	return result;
}

[[nodiscard]] not_null<Ui::AbstractButton*> MakeIconButton(
		not_null<QWidget*> parent,
		const style::icon &icon,
		Fn<void()> callback) {
	const auto button = Ui::CreateChild<Ui::AbstractButton>(parent.get());
	button->resize(44, 40);
	button->setPointerCursor(true);
	button->paintRequest(
	) | rpl::on_next([=, &icon] {
		auto p = QPainter(button);
		icon.paintInCenter(p, button->rect());
	}, button->lifetime());
	button->setClickedCallback(std::move(callback));
	return button;
}

// Круглая кнопка шапки: стрелка назад или лупа, рисуются вручную.
[[nodiscard]] not_null<Ui::AbstractButton*> MakeRoundButton(
		not_null<QWidget*> parent,
		bool back,
		Fn<void()> callback) {
	const auto button = Ui::CreateChild<Ui::AbstractButton>(parent.get());
	button->resize(36, 36);
	button->setPointerCursor(true);
	button->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(button);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBg);
		p.drawEllipse(button->rect());
		p.scale(0.75, 0.75);
		auto pen = QPen(st::windowFg->c, 2.2);
		pen.setCapStyle(Qt::RoundCap);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		if (back) {
			p.drawLine(QPointF(17, 17), QPointF(31, 31));
			p.drawLine(QPointF(31, 17), QPointF(17, 31));
		} else {
			p.drawEllipse(QPointF(22.5, 22.5), 7.5, 7.5);
			p.drawLine(QPointF(28, 28), QPointF(34, 34));
		}
	}, button->lifetime());
	button->setClickedCallback(std::move(callback));
	return button;
}

// Возвращает обёртку карточки и текст для поиска.
struct Card {
	not_null<Ui::SlideWrap<Ui::RpWidget>*> wrap;
	QString haystack;
};

Card AddPluginCard(
		not_null<Ui::VerticalLayout*> content,
		Window::SessionController *controller,
		const QJsonObject &entry,
		Fn<void()> removeClicked,
		Fn<void()> refresh,
		const LogFn &log) {
	const auto id = entry.value("id").toString();
	const auto name = entry.value("name").toString().isEmpty()
		? id
		: entry.value("name").toString();
	const auto fileName = entry.value("file").toString();
	const auto pinned = entry.value("pinned").toBool();
	auto description = entry.value("description").toString();
	const auto author = entry.value("author").toString();
	if (entry.value("state").toString() == "error") {
		description += (description.isEmpty() ? QString() : U("\n\n"))
			+ U("Ошибка: ") + entry.value("error").toString();
	}
	// Файл изменили после установки: не запускается, пока пользователь не подтвердит.
	const auto review = (entry.value("state").toString() == "needs_review");
	if (review) {
		description += (description.isEmpty() ? QString() : U("\n\n"))
			+ U("Файл плагина изменился после установки, он не запущен. "
				"Включите переключатель, чтобы подтвердить изменения.");
	}
	const auto icon = entry.value("icon").toString().trimmed();
	const auto isSet = icon.contains('/');
	const auto letter = (!icon.isEmpty() && !isSet)
		? icon.left(8)
		: name.left(1).toUpper();

	const auto wrap = content->add(object_ptr<Ui::SlideWrap<Ui::RpWidget>>(
		content,
		object_ptr<Ui::RpWidget>(content),
		QMargins(14, 10, 14, 0)));
	wrap->toggle(true, anim::type::instant);
	const auto panel = wrap->entity();
	const auto avatar = Ui::CreateChild<Ui::RpWidget>(panel);
	avatar->resize(kAvatar, kAvatar);
	const auto iconState = LoadSetEmoji(controller, icon, 32, avatar);
	avatar->paintOn([=](QPainter &p) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgRipple);
		p.drawRoundedRect(avatar->rect(), 14, 14);
		if (iconState->emoji) {
			iconState->emoji->paint(p, {
				.textColor = st::windowFg->c,
				.now = crl::now(),
				.position = QPoint((kAvatar - 32) / 2, (kAvatar - 32) / 2),
			});
		} else if (!isSet) {
			p.setPen(st::windowFg);
			p.setFont(style::font(24, 0, 0));
			p.drawText(avatar->rect(), Qt::AlignCenter, letter);
		}
	});
	const auto title = Ui::CreateChild<Ui::FlatLabel>(
		panel,
		name,
		st::boxTitle);
	const auto subtitle = MakeSubtitle(
		panel,
		entry.value("version").toString(),
		author);
	const auto toggle = MakeSwitch(
		panel,
		entry.value("enabled").toBool(),
		[=](bool enabled) {
			if (!review || !enabled) {
				SetEnabled(id.toStdString(), enabled, log);
				return;
			}
			const auto strong = controller;
			if (!strong) {
				refresh();
				return;
			}
			strong->show(Box([=](not_null<Ui::GenericBox*> confirm) {
				Ui::ConfirmBox(confirm, {
					.text = U("Плагин «") + name
						+ U("» изменили после установки. Права, которые он "
							"запрашивает сейчас, будут выданы заново. "
							"Запустить?"),
					.confirmed = [=](Fn<void()> close) {
						Approve(id.toStdString(), log);
						close();
						refresh();
					},
					.cancelled = [=](Fn<void()> close) {
						close();
						refresh();
					},
					.confirmText = U("Подтвердить"),
				});
			}));
		});
	const auto text = description.isEmpty()
		? nullptr
		: Ui::CreateChild<Ui::FlatLabel>(panel, description, st::boxLabel);

	const auto path = QString::fromStdString(PluginsDir())
		+ '/' + fileName;
	const auto share = MakeIconButton(panel, st::menuIconShare, [=] {
		auto *mime = new QMimeData();
		mime->setUrls({ QUrl::fromLocalFile(path) });
		QGuiApplication::clipboard()->setMimeData(mime);
		if (const auto strong = controller) {
			strong->showToast(U("Файл плагина скопирован, вставьте его в чат"));
		}
	});
	const auto open = MakeIconButton(panel, st::menuIconShowInFolder, [=] {
		File::ShowInFolder(path);
	});
	const auto pin = MakeIconButton(
		panel,
		pinned ? st::menuIconUnpin : st::menuIconPin,
		[=] {
			SetPinned(id.toStdString(), !pinned);
			refresh();
		});
	const auto trash = MakeIconButton(
		panel,
		st::menuIconDeleteAttention,
		std::move(removeClicked));

	panel->paintOn([=](QPainter &p) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBg);
		p.drawRoundedRect(panel->rect(), 22, 22);
		auto line = st::windowSubTextFg->c;
		line.setAlpha(36);
		p.fillRect(
			kCardPad,
			trash->y() - 1,
			panel->width() - 2 * kCardPad,
			st::lineWidth,
			line);
	});
	panel->widthValue() | rpl::on_next([=](int width) {
		const auto textLeft = kCardPad + kAvatar + 12;
		avatar->move(kCardPad, kCardPad);
		toggle->move(
			width - kCardPad - toggle->width(),
			kCardPad + (kAvatar - toggle->height()) / 2);
		title->resizeToNaturalWidth(toggle->x() - textLeft - 8);
		const auto block = title->height() + 2 + subtitle->height();
		const auto top = kCardPad + (kAvatar - block) / 2;
		title->move(textLeft, top);
		subtitle->move(textLeft, top + title->height() + 2);
		auto bottom = kCardPad + kAvatar;
		if (text) {
			text->resizeToWidth(width - 2 * kCardPad);
			text->move(kCardPad, bottom + 12);
			bottom += 12 + text->height();
		}
		bottom += 10;
		const auto y = bottom + 2;
		share->move(kCardPad - 8, y);
		open->move(kCardPad - 8 + share->width(), y);
		pin->move(kCardPad - 8 + 2 * share->width(), y);
		trash->move(width - kCardPad + 8 - trash->width(), y);
		panel->resize(width, y + trash->height() + 4);
	}, panel->lifetime());
	return { wrap, (name + ' ' + description + ' ' + author).toLower() };
}

} // namespace

void RoundedKeepButtons(
		not_null<Ui::GenericBox*> box,
		const style::Box &base) {
	box->setStyle(base);
	// Скругление рисует сама подложка окна, вместе с кнопками внизу.
	const auto apply = [=] {
		if (const auto layer = box->parentWidget()) {
			layer->setProperty("moka-radius", 28);
			layer->update();
		}
	};
	apply();
	base::call_delayed(0, box, apply);
}

void RoundedWindow(not_null<Ui::GenericBox*> box) {
	RoundedNoButtons(box);
}

not_null<Ui::AbstractButton*> MakeHeaderButton(
		not_null<QWidget*> parent,
		bool close,
		Fn<void()> callback) {
	return MakeRoundButton(parent, close, std::move(callback));
}

void PluginsBox(
		not_null<Ui::GenericBox*> box,
		Window::SessionController *controller) {
	RoundedNoButtons(box);
	const auto content = box->verticalLayout();
	const auto weak = base::make_weak(controller);
	const auto log = [](const std::string &line) {
		LOG(("[mokagram] %1").arg(QString::fromStdString(line)));
	};
	const auto reopen = [=] {
		box->closeBox();
		if (const auto again = weak.get()) {
			again->show(Box(PluginsBox, again));
		}
	};

	// Шапка: закрыть слева, поиск справа.
	const auto header = content->add(
		object_ptr<Ui::RpWidget>(content),
		QMargins(10, 10, 10, 0));
	header->resize(header->width(), 40);
	const auto close = MakeRoundButton(
		header,
		true,
		[=] { box->closeBox(); });
	const auto searchButton = MakeRoundButton(header, false, nullptr);
	const auto headerTitle = Ui::CreateChild<Ui::FlatLabel>(
		header,
		U("Плагины"),
		st::boxTitle);
	static const auto searchClear = style::owned_color(QColor(0, 0, 0, 0));
	static const auto fieldStyle = [] {
		auto result = st::defaultInputField;
		result.textBg = searchClear.color();
		result.textBgActive = searchClear.color();
		result.textMargins = style::margins(0, 7, 0, 7);
		result.placeholderFgActive = st::windowSubTextFg;
		result.placeholderScale = 1.;
		result.placeholderShift = 0;
		result.placeholderFont = st::normalFont;
		result.border = 0;
		result.borderActive = 0;
		result.heightMin = 36;
		return result;
	}();
	const auto pill = Ui::CreateChild<Ui::RpWidget>(header);
	pill->paintOn([=](QPainter &p) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBg);
		p.drawRoundedRect(pill->rect(), 18, 18);
	});
	const auto field = Ui::CreateChild<Ui::InputField>(
		header,
		fieldStyle,
		rpl::single(U("Поиск")));
	pill->hide();
	field->hide();
	header->widthValue() | rpl::on_next([=](int width) {
		close->move(0, (header->height() - close->height()) / 2);
		searchButton->move(
			width - searchButton->width(),
			(header->height() - searchButton->height()) / 2);
		headerTitle->move(
			(width - headerTitle->width()) / 2,
			(header->height() - headerTitle->height()) / 2);
		const auto left = close->width() + 8;
		const auto pillWidth = width - left - searchButton->width() - 8;
		pill->setGeometry(left, (header->height() - 36) / 2, pillWidth, 36);
		field->resizeToWidth(pillWidth - 28);
		field->move(
			left + 14,
			(header->height() - field->height()) / 2);
	}, header->lifetime());

	const auto engine = content->add(
		object_ptr<Ui::RpWidget>(content),
		QMargins(14, 8, 14, 4));
	engine->resize(engine->width(), 56);
	const auto engineLabel = Ui::CreateChild<Ui::FlatLabel>(
		engine,
		U("Включить систему плагинов"),
		st::boxLabel);
	const auto engineSwitch = MakeSwitch(
		engine,
		EngineEnabled(),
		[=](bool enabled) { SetEngineEnabled(enabled, log); });
	engine->paintOn([=](QPainter &p) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBg);
		p.drawRoundedRect(engine->rect(), 28, 28);
	});
	engine->widthValue() | rpl::on_next([=](int width) {
		engineLabel->move(22, (engine->height() - engineLabel->height()) / 2);
		engineSwitch->move(
			width - 20 - engineSwitch->width(),
			(engine->height() - engineSwitch->height()) / 2);
	}, engine->lifetime());

	const auto list = QJsonDocument::fromJson(
		QByteArray::fromStdString(ListJson())).array();
	if (list.isEmpty()) {
		AddCenteredLabel(
			content,
			U("Плагинов пока нет. Добавьте файл .plugin кнопкой ниже "
				"или нажмите на него в чате."),
			st::boxDividerLabel,
			QMargins(24, 16, 24, 8));
	}
	const auto cards = std::make_shared<std::vector<Card>>();
	for (const auto &value : list) {
		const auto entry = value.toObject();
		const auto id = entry.value("id").toString();
		const auto title = entry.value("name").toString();
		cards->push_back(AddPluginCard(content, controller, entry, [=] {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			strong->show(Box([=](not_null<Ui::GenericBox*> confirm) {
				Ui::ConfirmBox(confirm, {
					.text = U("Удалить плагин «")
						+ (title.isEmpty() ? id : title) + U("»?"),
					.confirmed = [=](Fn<void()> close) {
						Remove(id.toStdString(), log);
						close();
						reopen();
					},
					.confirmText = U("Удалить"),
				});
			}));
		}, reopen, log));
	}

	// Кнопка выбора файла .plugin с диска.
	static const auto stAdd = [] {
		auto result = st::defaultActiveButton;
		result.height = 44;
		result.textTop = 12;
		result.radius = 22;
		result.fullRadius = true;
		return result;
	}();
	const auto addRow = content->add(
		object_ptr<Ui::RpWidget>(content),
		QMargins(14, 8, 14, 0));
	addRow->resize(addRow->width(), stAdd.height);
	const auto add = Ui::CreateChild<Ui::RoundButton>(
		addRow,
		rpl::single(U("Добавить плагин")),
		stAdd);
	addRow->widthValue() | rpl::on_next([=](int width) {
		add->setFullWidth(width);
		add->moveToLeft(0, 0);
	}, addRow->lifetime());
	const auto weakBox = base::make_weak(box);
	add->setClickedCallback([=] {
		FileDialog::GetOpenPath(
			box.get(),
			U("Выберите плагин"),
			U("Плагин mokaGram (*.plugin);;") + FileDialog::AllFilesFilter(),
			crl::guard(box.get(), [=](FileDialog::OpenResult &&result) {
				auto bytes = result.remoteContent;
				if (bytes.isEmpty() && !result.paths.isEmpty()) {
					auto file = QFile(result.paths.front());
					if (file.open(QIODevice::ReadOnly)) {
						bytes = file.readAll();
					}
				}
				const auto strong = weak.get();
				if (!strong) {
					return;
				}
				if (bytes.isEmpty()) {
					if (!result.paths.isEmpty()) {
						strong->showToast(U("Не удалось прочитать файл плагина"));
					}
					return;
				}
				ShowInstallBox(strong, bytes, [=] {
					if (weakBox) {
						reopen();
					}
				});
			}));
	});
	{
		const auto storeRow = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(14, 8, 14, 0));
		storeRow->resize(storeRow->width(), stAdd.height);
		const auto store = Ui::CreateChild<Ui::RoundButton>(
			storeRow,
			rpl::single(U("Магазин плагинов")),
			stAdd);
		storeRow->widthValue() | rpl::on_next([=](int width) {
			store->setFullWidth(width);
			store->moveToLeft(0, 0);
		}, storeRow->lifetime());
		store->setClickedCallback([=] {
			if (const auto strong = weak.get()) {
				strong->show(Box(StoreBox, strong, reopen));
			}
		});
	}
	Ui::AddSkip(content, 14);

	// Поиск: показывает поле вместо заголовка и скрывает неподходящие карточки.
	const auto searching = std::make_shared<bool>(false);
	const auto applyFilter = [=] {
		const auto query = *searching
			? field->getLastText().trimmed().toLower()
			: QString();
		for (const auto &card : *cards) {
			card.wrap->toggle(
				query.isEmpty() || card.haystack.contains(query),
				anim::type::instant);
		}
	};
	field->changes() | rpl::on_next(applyFilter, field->lifetime());
	searchButton->setClickedCallback([=] {
		*searching = !*searching;
		headerTitle->setVisible(!*searching);
		pill->setVisible(*searching);
		field->setVisible(*searching);
		field->raise();
		if (*searching) {
			field->setFocusFast();
		} else {
			field->setText(QString());
		}
		applyFilter();
	});
}

void MakePill(not_null<Ui::RpWidget*> widget, Fn<QColor()> pageBg) {
	struct Corner {
		not_null<Ui::RpWidget*> widget;
		bool right;
		bool bottom;
	};
	struct State {
		std::vector<Corner> corners;
		int radius = 1;
	};
	const auto state = widget->lifetime().make_state<State>();
	for (const auto right : { false, true }) {
		for (const auto bottom : { false, true }) {
			const auto corner = Ui::CreateChild<Ui::RpWidget>(widget.get());
			corner->setAttribute(Qt::WA_TransparentForMouseEvents);
			corner->paintRequest(
			) | rpl::on_next([=] {
				auto p = QPainter(corner);
				auto hq = PainterHighQualityEnabler(p);
				const auto r = state->radius;
				auto path = QPainterPath();
				path.addRect(corner->rect());
				auto circle = QPainterPath();
				circle.addEllipse(QRectF(
					right ? -r : 0,
					bottom ? -r : 0,
					2 * r,
					2 * r));
				p.fillPath(path.subtracted(circle), pageBg());
			}, corner->lifetime());
			state->corners.push_back({ corner, right, bottom });
		}
	}
	widget->sizeValue(
	) | rpl::on_next([=](QSize size) {
		const auto r = std::max(1, std::min(size.width(), size.height()) / 2);
		state->radius = r;
		for (const auto &corner : state->corners) {
			corner.widget->resize(r, r);
			corner.widget->move(
				corner.right ? size.width() - r : 0,
				corner.bottom ? size.height() - r : 0);
			corner.widget->update();
			corner.widget->raise();
		}
	}, widget->lifetime());
}

namespace {
[[nodiscard]] const style::icon *MenuIconByName(QString name);
} // namespace

void AddMessageMenuItems(
		not_null<Ui::PopupMenu*> menu,
		HistoryItem *item,
		const QString &selected) {
	if (!item) {
		return;
	}
	auto ctx = QJsonObject();
	ctx.insert("id", QString::number(item->id.bare));
	ctx.insert("peer", QString::number(item->history()->peer->id.value));
	ctx.insert("text", item->originalText().text);
	ctx.insert("out", item->out());
	ctx.insert("selected", selected);
	ctx.insert("has_media", item->media() != nullptr);
	ctx.insert("date", double(item->date()));
	const auto ctxJson = QJsonDocument(ctx).toJson(
		QJsonDocument::Compact).toStdString();
	const auto list = QJsonDocument::fromJson(
		QByteArray::fromStdString(MenuItemsJson("message", ctxJson))).array();
	for (const auto &value : list) {
		const auto entry = value.toObject();
		const auto pid = entry.value("pid").toString().toStdString();
		const auto index = entry.value("index").toInt();
		menu->addAction(
			entry.value("text").toString(),
			[=] {
				MenuItemClick(pid, index, ctxJson, [](const std::string &line) {
					LOG(("[mokagram] %1").arg(QString::fromStdString(line)));
				});
			},
			MenuIconByName(entry.value("icon").toString()));
	}
}

namespace {

void LogLine(const std::string &line) {
	LOG(("[mokagram] %1").arg(QString::fromStdString(line)));
}

[[nodiscard]] const style::icon *MenuIconByName(QString name) {
	static const auto table = std::map<QString, const style::icon*>{
		{ u"reactions"_q, &st::menuIconReactions },
		{ u"reply"_q, &st::menuIconReply },
		{ u"edit"_q, &st::menuIconEdit },
		{ u"draw"_q, &st::menuIconDraw },
		{ u"pin"_q, &st::menuIconPin },
		{ u"unpin"_q, &st::menuIconUnpin },
		{ u"copy"_q, &st::menuIconCopy },
		{ u"forward"_q, &st::menuIconForward },
		{ u"delete"_q, &st::menuIconDelete },
		{ u"select"_q, &st::menuIconSelect },
		{ u"saveimage"_q, &st::menuIconSaveImage },
		{ u"stickers"_q, &st::menuIconStickers },
		{ u"emoji"_q, &st::menuIconEmoji },
		{ u"cancel"_q, &st::menuIconCancel },
		{ u"showinchat"_q, &st::menuIconShowInChat },
		{ u"gif"_q, &st::menuIconGif },
		{ u"download"_q, &st::menuIconDownload },
		{ u"link"_q, &st::menuIconLink },
		{ u"block"_q, &st::menuIconBlock },
		{ u"report"_q, &st::menuIconReport },
		{ u"fave"_q, &st::menuIconFave },
		{ u"profile"_q, &st::menuIconProfile },
		{ u"info"_q, &st::menuIconInfo },
		{ u"invite"_q, &st::menuIconInvite },
		{ u"share"_q, &st::menuIconShare },
		{ u"archive"_q, &st::menuIconArchive },
		{ u"markread"_q, &st::menuIconMarkRead },
		{ u"markunread"_q, &st::menuIconMarkUnread },
		{ u"clear"_q, &st::menuIconClear },
		{ u"manage"_q, &st::menuIconManage },
		{ u"stats"_q, &st::menuIconStats },
		{ u"schedule"_q, &st::menuIconSchedule },
		{ u"send"_q, &st::menuIconSend },
		{ u"settings"_q, &st::menuIconSettings },
		{ u"file"_q, &st::menuIconFile },
		{ u"photo"_q, &st::menuIconPhoto },
		{ u"search"_q, &st::menuIconSearch },
		{ u"translate"_q, &st::menuIconTranslate },
		{ u"channel"_q, &st::menuIconChannel },
		{ u"groups"_q, &st::menuIconGroups },
		{ u"savedmessages"_q, &st::menuIconSavedMessages },
		{ u"nightmode"_q, &st::menuIconNightMode },
		{ u"notifications"_q, &st::menuIconNotifications },
		{ u"lock"_q, &st::menuIconLock },
		{ u"bot"_q, &st::menuIconBot },
		{ u"botcommands"_q, &st::menuIconBotCommands },
		{ u"premium"_q, &st::menuIconPremium },
		{ u"shop"_q, &st::menuIconShop },
		{ u"add"_q, &st::menuIconAdd },
		{ u"restore"_q, &st::menuIconRestore },
		{ u"palette"_q, &st::menuIconPalette },
		{ u"mute"_q, &st::menuIconMute },
		{ u"unmute"_q, &st::menuIconUnmute },
		{ u"timer"_q, &st::menuIconTimer },
		{ u"topics"_q, &st::menuIconTopics },
		{ u"links"_q, &st::menuIconLinks },
		{ u"chats"_q, &st::menuIconChats },
		{ u"phone"_q, &st::menuIconPhone },
		{ u"chatbubble"_q, &st::menuIconChatBubble },
		{ u"devices"_q, &st::menuIconDevices },
		{ u"network"_q, &st::menuIconNetwork },
		{ u"storage"_q, &st::menuIconStorage },
		{ u"export"_q, &st::menuIconExport },
		{ u"leave"_q, &st::menuIconLeave },
		{ u"soundon"_q, &st::menuIconSoundOn },
		{ u"soundoff"_q, &st::menuIconSoundOff },
		{ u"expand"_q, &st::menuIconExpand },
		{ u"collapse"_q, &st::menuIconCollapse },
		{ u"replace"_q, &st::menuIconReplace },
		{ u"remove"_q, &st::menuIconRemove },
		{ u"like"_q, &st::menuIconLike },
		{ u"hourglass"_q, &st::menuIconHourglass },
		{ u"payment"_q, &st::menuIconPayment },
		{ u"ton"_q, &st::menuIconTon },
		{ u"reorder"_q, &st::menuIconReorder },
		{ u"address"_q, &st::menuIconAddress },
		{ u"faq"_q, &st::menuIconFaq },
		{ u"username"_q, &st::menuIconUsername }
	};
	name = name.toLower();
	if (name.startsWith(u"menuicon"_q)) {
		name = name.mid(8);
	}
	const auto i = table.find(name);
	return (i != end(table)) ? i->second : &st::menuIconBotCommands;
}

void FillMenuEntries(
		const std::string &location,
		const std::string &ctxJson,
		const MenuAdd &add) {
	const auto list = QJsonDocument::fromJson(QByteArray::fromStdString(
		MenuItemsJson(location, ctxJson))).array();
	for (const auto &value : list) {
		const auto entry = value.toObject();
		const auto pid = entry.value("pid").toString().toStdString();
		const auto index = entry.value("index").toInt();
		add(entry.value("text").toString(), [=] {
			MenuItemClick(pid, index, ctxJson, LogLine);
		}, MenuIconByName(entry.value("icon").toString()));
	}
}

} // namespace

void AddPeerMenuItems(
		const QString &location,
		PeerData *peer,
		const MenuAdd &add) {
	if (!peer) {
		return;
	}
	auto ctx = QJsonObject();
	ctx.insert("peer", QString::number(peer->id.value));
	ctx.insert("title", peer->name());
	const auto user = peer->asUser();
	ctx.insert("kind", user
		? (user->isBot() ? "bot" : "user")
		: (peer->isChannel() && !peer->isMegagroup() ? "channel" : "group"));
	ctx.insert("username", peer->username());
	FillMenuEntries(
		location.toStdString(),
		QJsonDocument(ctx).toJson(QJsonDocument::Compact).toStdString(),
		add);
}

std::vector<DrawerItem> DrawerMenuItems() {
	auto result = std::vector<DrawerItem>();
	FillMenuEntries("drawer", "{}", [&](
			const QString &text,
			Fn<void()> handler,
			const style::icon *icon) {
		result.push_back({ text, std::move(handler), icon });
	});
	return result;
}

} // namespace Mokagram::Plugins
