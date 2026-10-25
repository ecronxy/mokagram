#include "mokagram/quote/mokagram_quote.h"

#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "mokagram/settings/mokagram_settings.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"
#include "ui/chat/chat_style.h"
#include "ui/empty_userpic.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QStandardPaths>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QTextLayout>
#include <QtGui/QTextOption>

#include <algorithm>
#include <memory>
#include <vector>

namespace Mokagram::Quote {
namespace {

namespace S = Settings;

enum class Mask { None, Blur, Pixelate, Fill };

constexpr auto kRender = 2; // качество: во сколько раз больше экрана

[[nodiscard]] QString U(const char *text) {
	return QString::fromUtf8(text);
}

struct Line {
	QString name;
	QColor nameColor;
	QImage avatar; // квадрат, уже круглый
	QString text;
	QString time;
	bool out = false;
	bool first = true; // первый в серии одного автора
};

[[nodiscard]] QColor ParseColor(const QString &text, QColor fallback) {
	const auto result = QColor(text.trimmed());
	return (!text.trimmed().isEmpty() && result.isValid()) ? result : fallback;
}

[[nodiscard]] QColor NameColor(uint64 id) {
	static const auto colors = std::array<QColor, 7>{
		QColor(0xe1, 0x70, 0x76), QColor(0xe0, 0x93, 0x5a),
		QColor(0xa0, 0x6c, 0xd8), QColor(0x65, 0xb4, 0x6a),
		QColor(0x4f, 0xb9, 0xc9), QColor(0x4f, 0x9d, 0xe6),
		QColor(0xd8, 0x6a, 0xa8) };
	return colors[id % colors.size()];
}

// Размытие и пиксели через двойное масштабирование: без лишних зависимостей.
[[nodiscard]] QImage Masked(QImage image, Mask mask) {
	if (mask == Mask::None || image.isNull()) {
		return image;
	}
	const auto size = image.size();
	if (mask == Mask::Fill) {
		auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
		result.setDevicePixelRatio(image.devicePixelRatio());
		result.fill(Qt::transparent);
		auto p = QPainter(&result);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(120, 120, 120));
		p.drawRoundedRect(QRectF(QPointF(), QSizeF(size)).adjusted(0, size.height() * 0.1, 0, -size.height() * 0.1), size.height() * 0.2, size.height() * 0.2);
		return result;
	}
	const auto factor = (mask == Mask::Blur) ? 6 : 9;
	const auto small = QSize(
		std::max(1, size.width() / factor),
		std::max(1, size.height() / factor));
	auto down = image.scaled(
		small,
		Qt::IgnoreAspectRatio,
		(mask == Mask::Blur)
			? Qt::SmoothTransformation
			: Qt::FastTransformation);
	if (mask == Mask::Blur) {
		// Два прохода делают размытие мягче.
		down = down.scaled(
			QSize(std::max(1, small.width() / 2), std::max(1, small.height() / 2)),
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
	}
	auto result = down.scaled(
		size,
		Qt::IgnoreAspectRatio,
		(mask == Mask::Blur)
			? Qt::SmoothTransformation
			: Qt::FastTransformation);
	result.setDevicePixelRatio(image.devicePixelRatio());
	return result;
}

[[nodiscard]] QImage AvatarImage(not_null<PeerData*> peer, int size) {
	auto image = QImage(
		QSize(size, size),
		QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	auto p = QPainter(&image);
	p.setRenderHint(QPainter::Antialiasing);
	auto view = peer->createUserpicView();
	peer->paintUserpic(p, view, 0, 0, size, true);
	return image;
}

struct Options {
	bool dark = true;
	Mask names = Mask::None;
	Mask avatars = Mask::None;
	bool time = true;
	bool transparent = false;
	int width = 640;
	QColor bubbleIn, bubbleOut, background;
};

[[nodiscard]] Options CurrentOptions() {
	auto o = Options();
	o.dark = (S::Int(u"quote.theme"_q) == 0);
	o.names = Mask(std::clamp(S::Int(u"quote.names"_q), 0, 3));
	o.avatars = Mask(std::clamp(S::Int(u"quote.avatars"_q), 0, 3));
	o.time = S::Bool(u"quote.time"_q);
	o.transparent = S::Bool(u"quote.transparent"_q);
	o.width = std::clamp(S::Int(u"quote.width"_q), 360, 1000);
	o.bubbleIn = ParseColor(S::String(u"quote.bubble_in"_q), o.dark ? QColor(0x21, 0x2d, 0x3b) : QColor(0xff, 0xff, 0xff));
	o.bubbleOut = ParseColor(S::String(u"quote.bubble_out"_q), o.dark ? QColor(0x2b, 0x52, 0x78) : QColor(0xef, 0xfd, 0xde));
	o.background = ParseColor(S::String(u"quote.background"_q), o.dark ? QColor(0x0e, 0x16, 0x21) : QColor(0xe6, 0xeb, 0xee));
	return o;
}

[[nodiscard]] QImage Render(const std::vector<Line> &lines, const Options &o) {
	const auto k = kRender;
	const auto pad = 24, gap = 6, avatar = 40, avatarGap = 10;
	const auto maxBubble = o.width - pad * 2 - avatar - avatarGap;
	auto textFont = QFont();
	textFont.setPixelSize(17);
	auto nameFont = textFont;
	nameFont.setBold(true);
	nameFont.setPixelSize(15);
	auto timeFont = textFont;
	timeFont.setPixelSize(12);
	const auto fm = QFontMetrics(textFont);
	const auto nm = QFontMetrics(nameFont);
	const auto tm = QFontMetrics(timeFont);

	struct Box { QRect bubble; QRect text; QRect name; QRect time; int top = 0; int height = 0; };
	auto boxes = std::vector<Box>();
	auto y = pad;
	for (const auto &line : lines) {
		auto b = Box();
		b.top = y + (line.first ? 6 : 0);
		const auto textW = maxBubble - 28;
		auto textRect = fm.boundingRect(
			QRect(0, 0, textW, 100000),
			Qt::TextWordWrap | Qt::AlignLeft,
			line.text);
		auto bubbleW = std::max(textRect.width(), 56);
		auto innerH = textRect.height();
		auto nameW = 0;
		if (line.first && !line.name.isEmpty()) {
			nameW = nm.horizontalAdvance(line.name);
			bubbleW = std::max(bubbleW, std::min(nameW, textW));
			innerH += nm.height() + 4;
		}
		const auto timeW = o.time ? tm.horizontalAdvance(line.time) : 0;
		if (o.time) {
			bubbleW = std::max(bubbleW, std::min(textW, textRect.width() + timeW + 12));
			innerH += tm.height() + 2;
		}
		b.bubble = QRect(0, b.top, bubbleW + 28, innerH + 20);
		boxes.push_back(b);
		y = b.top + b.bubble.height() + gap;
	}
	const auto total = y + pad - gap;

	auto image = QImage(QSize(o.width, total) * k, QImage::Format_ARGB32_Premultiplied);
	image.fill(o.transparent ? Qt::transparent : o.background);
	auto p = QPainter(&image);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.scale(k, k);
	for (auto i = 0; i != int(lines.size()); ++i) {
		const auto &line = lines[i];
		auto &b = boxes[i];
		const auto x0 = pad + avatar + avatarGap;
		const auto bubble = b.bubble.translated(x0, 0);
		if (line.first) {
			auto av = line.avatar;
			if (!av.isNull()) {
				av = Masked(av, o.avatars);
				p.drawImage(QRect(pad, b.top, avatar, avatar), av);
			}
		}
		p.setPen(Qt::NoPen);
		p.setBrush(line.out ? o.bubbleOut : o.bubbleIn);
		p.drawRoundedRect(bubble, 16, 16);
		auto ty = bubble.top() + 10;
		if (line.first && !line.name.isEmpty()) {
			const auto nameRect = QRect(bubble.left() + 14, ty, bubble.width() - 28, nm.height());
			if (o.names == Mask::None) {
				p.setPen(line.nameColor);
				p.setFont(nameFont);
				p.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, nm.elidedText(line.name, Qt::ElideRight, nameRect.width()));
			} else {
				// Имя рисуем в картинку и портим её.
				const auto w = std::min(nm.horizontalAdvance(line.name) + 4, nameRect.width());
				auto strip = QImage(QSize(w, nm.height()) * k, QImage::Format_ARGB32_Premultiplied);
				strip.setDevicePixelRatio(k);
				strip.fill(Qt::transparent);
				{
					auto q = QPainter(&strip);
					q.setRenderHint(QPainter::TextAntialiasing);
					q.setPen(line.nameColor);
					q.setFont(nameFont);
					q.drawText(QRect(0, 0, w, nm.height()), Qt::AlignLeft | Qt::AlignVCenter, line.name);
				}
				p.drawImage(QRect(nameRect.left(), nameRect.top(), w, nm.height()), Masked(strip, o.names));
			}
			ty += nm.height() + 4;
		}
		p.setPen(o.dark ? QColor(255, 255, 255) : QColor(0, 0, 0));
		p.setFont(textFont);
		const auto textRect = QRect(bubble.left() + 14, ty, bubble.width() - 28, 100000);
		p.drawText(textRect, Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, line.text);
		if (o.time) {
			p.setPen(o.dark ? QColor(255, 255, 255, 130) : QColor(0, 0, 0, 110));
			p.setFont(timeFont);
			p.drawText(
				QRect(bubble.left() + 14, bubble.bottom() - 10 - tm.height(), bubble.width() - 28, tm.height()),
				Qt::AlignRight | Qt::AlignVCenter,
				line.time);
		}
	}
	p.end();
	image.setDevicePixelRatio(1.);
	return image;
}

[[nodiscard]] std::vector<Line> Collect(
		not_null<Window::SessionController*> controller,
		const std::vector<FullMsgId> &ids) {
	auto items = std::vector<not_null<HistoryItem*>>();
	for (const auto &id : ids) {
		if (const auto item = controller->session().data().message(id)) {
			items.push_back(item);
		}
	}
	std::sort(items.begin(), items.end(), [](auto a, auto b) {
		return a->date() < b->date()
			|| (a->date() == b->date() && a->id < b->id);
	});
	auto result = std::vector<Line>();
	auto last = PeerId();
	for (const auto item : items) {
		const auto from = item->from();
		const auto who = from ? from.get() : item->history()->peer.get();
		auto line = Line();
		line.name = who->name();
		line.nameColor = NameColor(who->id.value);
		line.out = item->out();
		line.first = (who->id != last);
		last = who->id;
		line.text = item->originalText().text;
		if (line.text.isEmpty()) {
			line.text = U("[вложение]");
		}
		line.time = QDateTime::fromSecsSinceEpoch(item->date()).toString("HH:mm");
		if (line.first) {
			line.avatar = AvatarImage(who, 80);
		}
		result.push_back(std::move(line));
	}
	return result;
}

} // namespace

void RegisterSettings() {
	const auto section = U("Картинка-цитата");
	const auto add = [&](S::Entry entry) {
		entry.section = section;
		entry.category = u"look"_q;
		entry.sectionIcon = u"copy"_q;
		S::Register(std::move(entry));
	};
	const auto masks = QStringList{ U("Показывать"), U("Размыть"), U("Пиксели"), U("Заливка") };
	add({
		.key = u"quote.theme"_q,
		.title = U("Тема"),
		.description = U("Меню «Цитата-картинка» на сообщении собирает картинку."),
		.kind = S::Kind::Choice,
		.defaultValue = 0,
		.choices = { U("Тёмная"), U("Светлая") },
	});
	add({
		.key = u"quote.names"_q,
		.title = U("Имена"),
		.kind = S::Kind::Choice,
		.defaultValue = 0,
		.choices = masks,
	});
	add({
		.key = u"quote.avatars"_q,
		.title = U("Аватарки"),
		.kind = S::Kind::Choice,
		.defaultValue = 0,
		.choices = masks,
	});
	add({
		.key = u"quote.time"_q,
		.title = U("Время под сообщением"),
		.kind = S::Kind::Bool,
		.defaultValue = true,
	});
	add({
		.key = u"quote.transparent"_q,
		.title = U("Прозрачный фон"),
		.kind = S::Kind::Bool,
		.defaultValue = false,
	});
	add({
		.key = u"quote.width"_q,
		.title = U("Ширина картинки, px"),
		.kind = S::Kind::Int,
		.defaultValue = 640,
		.minValue = 360,
		.maxValue = 1000,
	});
	add({
		.key = u"quote.bubble_in"_q,
		.title = U("Цвет чужих пузырей"),
		.kind = S::Kind::Color,
		.defaultValue = QString(),
	});
	add({
		.key = u"quote.bubble_out"_q,
		.title = U("Цвет моих пузырей"),
		.kind = S::Kind::Color,
		.defaultValue = QString(),
	});
	add({
		.key = u"quote.background"_q,
		.title = U("Цвет фона"),
		.kind = S::Kind::Color,
		.defaultValue = QString(),
	});
}

void ShowQuoteBox(
		not_null<Window::SessionController*> controller,
		std::vector<FullMsgId> ids) {
	if (ids.empty()) {
		return;
	}
	auto lines = std::make_shared<std::vector<Line>>(Collect(controller, ids));
	if (lines->empty()) {
		return;
	}
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(U("Картинка-цитата")));
		box->setWidth(520);
		const auto content = box->verticalLayout();
		const auto image = std::make_shared<QImage>();
		const auto preview = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(24, 8, 24, 8));
		const auto refresh = [=] {
			*image = Render(*lines, CurrentOptions());
			const auto w = preview->width() > 0 ? preview->width() : 470;
			const auto h = int(image->height() * (w / double(image->width())));
			preview->resize(w, std::min(h, 520));
			preview->update();
		};
		preview->paintOn([=](QPainter &p) {
			if (image->isNull()) {
				return;
			}
			p.setRenderHint(QPainter::SmoothPixmapTransform);
			const auto w = preview->width();
			const auto h = int(image->height() * (w / double(image->width())));
			p.drawImage(QRect(0, 0, w, h), *image);
		});
		const auto radio = [&](const QString &title, const char *key, const QStringList &names) {
			Ui::AddSubsectionTitle(content, rpl::single(title));
			const auto group = std::make_shared<Ui::RadiobuttonGroup>(S::Int(QString::fromUtf8(key)));
			for (auto i = 0; i != names.size(); ++i) {
				content->add(
					object_ptr<Ui::Radiobutton>(content, group, i, names[i], st::defaultCheckbox),
					QMargins(24, 4, 24, 4));
			}
			group->setChangedCallback([=](int value) {
				S::SetInt(QString::fromUtf8(key), value);
				refresh();
			});
		};
		const auto masks = QStringList{ U("Показывать"), U("Размыть"), U("Пиксели"), U("Заливка") };
		radio(U("Имена"), "quote.names", masks);
		radio(U("Аватарки"), "quote.avatars", masks);
		radio(U("Тема"), "quote.theme", { U("Тёмная"), U("Светлая") });
		refresh();
		box->addButton(rpl::single(U("Копировать")), [=] {
			QGuiApplication::clipboard()->setImage(*image);
			controller->showToast(U("Картинка скопирована"));
		});
		box->addButton(rpl::single(U("Сохранить")), [=] {
			auto dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
			QDir().mkpath(dir);
			const auto path = dir + u"/mokagram_quote_"_q
				+ QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")
				+ u".png"_q;
			if (image->save(path, "PNG")) {
				controller->showToast(U("Сохранено: ") + path);
			} else {
				controller->showToast(U("Не получилось сохранить"));
			}
		});
		box->addLeftButton(rpl::single(U("Закрыть")), [=] { box->closeBox(); });
	}));
}

} // namespace Mokagram::Quote
