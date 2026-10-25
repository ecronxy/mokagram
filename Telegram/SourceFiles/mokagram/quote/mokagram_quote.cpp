#include "mokagram/quote/mokagram_quote.h"

#include "base/call_delayed.h"
#include "core/application.h"
#include "window/window_controller.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/view/history_view_element.h"
#include "dialogs/ui/dialogs_video_userpic.h"
#include "ui/chat/chat_theme.h"
#include "ui/text/custom_emoji_instance.h"
#include "ui/effects/path_shift_gradient.h"
#include "ui/style/style_core_scale.h"
#include "data/data_forum.h"
#include "main/main_session.h"
#include "mokagram/settings/mokagram_settings.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"
#include "ui/chat/chat_style.h"
#include "ui/empty_userpic.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QStandardPaths>
#include <QtGui/QClipboard>
#include <QtGui/QLinearGradient>
#include <QtGui/QMouseEvent>
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

struct Gradient {
	const char *name;
	QColor a, b;
};

const std::vector<Gradient> &Gradients() {
	static const auto result = std::vector<Gradient>{
		{ "Закат", QColor(0xf6, 0xd3, 0x65), QColor(0xfd, 0xa0, 0x85) },
		{ "Океан", QColor(0x4f, 0xac, 0xfe), QColor(0x00, 0xc6, 0xfb) },
		{ "Ночь", QColor(0x0f, 0x20, 0x27), QColor(0x2c, 0x53, 0x64) },
		{ "Фиолет", QColor(0x8e, 0x2d, 0xe2), QColor(0x4a, 0x00, 0xe0) },
		{ "Мята", QColor(0x11, 0x99, 0x8e), QColor(0x38, 0xef, 0x7d) },
		{ "Графит", QColor(0x23, 0x25, 0x26), QColor(0x41, 0x43, 0x45) },
		{ "Белый", QColor(0xf5, 0xf7, 0xfa), QColor(0xc3, 0xcf, 0xe2) },
	};
	return result;
}

constexpr auto kCustomBg = 100;

[[nodiscard]] bool BgColors(int index, QColor &a, QColor &b) {
	if (index >= 0 && index < int(Gradients().size())) {
		a = Gradients()[index].a;
		b = Gradients()[index].b;
		return true;
	} else if (index == kCustomBg) {
		a = QColor(S::String(u"quote.custom1"_q));
		b = QColor(S::String(u"quote.custom2"_q));
		if (!a.isValid()) {
			a = QColor(0xff, 0x9a, 0x8b);
		}
		if (!b.isValid()) {
			b = QColor(0x6a, 0x11, 0xcb);
		}
		return true;
	}
	return false;
}

struct MaskRect {
	QRectF norm; // относительно картинки сообщений, 0..1
	Mask mask = Mask::Blur;
};

// Углы пузырей Telegram рисует из мелких картинок: при увеличении они дают
// ступеньки. Сглаживаем только край (там, где размытие меняет прозрачность),
// текст внутри пузыря не трогаем.
[[nodiscard]] QImage BoxBlur(const QImage &src, int r) {
	const auto w = src.width(), h = src.height();
	auto tmp = QImage(src.size(), QImage::Format_ARGB32_Premultiplied);
	auto out = QImage(src.size(), QImage::Format_ARGB32_Premultiplied);
	const auto n = 2 * r + 1;
	for (auto y = 0; y != h; ++y) {
		const auto in = reinterpret_cast<const QRgb*>(src.constScanLine(y));
		auto row = reinterpret_cast<QRgb*>(tmp.scanLine(y));
		auto a = 0, rr = 0, g = 0, b = 0;
		const auto add = [&](int x, int sign) {
			const auto c = in[std::clamp(x, 0, w - 1)];
			a += sign * qAlpha(c); rr += sign * qRed(c);
			g += sign * qGreen(c); b += sign * qBlue(c);
		};
		for (auto x = -r; x <= r; ++x) add(x, 1);
		for (auto x = 0; x != w; ++x) {
			row[x] = qRgba(rr / n, g / n, b / n, a / n);
			add(x + r + 1, 1);
			add(x - r, -1);
		}
	}
	for (auto x = 0; x != w; ++x) {
		auto a = 0, rr = 0, g = 0, b = 0;
		const auto add = [&](int y, int sign) {
			const auto c = reinterpret_cast<const QRgb*>(tmp.constScanLine(std::clamp(y, 0, h - 1)))[x];
			a += sign * qAlpha(c); rr += sign * qRed(c);
			g += sign * qGreen(c); b += sign * qBlue(c);
		};
		for (auto y = -r; y <= r; ++y) add(y, 1);
		for (auto y = 0; y != h; ++y) {
			reinterpret_cast<QRgb*>(out.scanLine(y))[x] = qRgba(rr / n, g / n, b / n, a / n);
			add(y + r + 1, 1);
			add(y - r, -1);
		}
	}
	return out;
}

void SmoothEdges(QImage &image, int radius) {
	if (image.isNull() || radius < 1) {
		return;
	}
	const auto blurred = BoxBlur(image, radius);
	for (auto y = 0; y != image.height(); ++y) {
		auto line = reinterpret_cast<QRgb*>(image.scanLine(y));
		const auto soft = reinterpret_cast<const QRgb*>(blurred.constScanLine(y));
		for (auto x = 0; x != image.width(); ++x) {
			if (std::abs(qAlpha(soft[x]) - qAlpha(line[x])) > 3) {
				line[x] = soft[x];
			}
		}
	}
}

[[nodiscard]] QImage StackRows(const std::vector<QImage> &rows) {
	if (rows.empty()) {
		return QImage();
	}
	const auto dpr = rows.front().devicePixelRatio();
	auto left = INT_MAX, right = 0, height = 0;
	auto heights = std::vector<int>();
	for (const auto &row : rows) {
		height += row.height();
		for (auto y = 0; y != row.height(); ++y) {
			const auto line = reinterpret_cast<const QRgb*>(row.constScanLine(y));
			for (auto x = 0; x != row.width(); ++x) {
				if (qAlpha(line[x]) > 6) {
					left = std::min(left, x);
					right = std::max(right, x);
				}
			}
		}
	}
	if (right <= left) {
		return QImage();
	}
	const auto margin = int(8 * dpr);
	left = std::max(0, left - margin);
	right = std::min(rows.front().width() - 1, right + margin);
	auto result = QImage(
		QSize(right - left + 1, height),
		QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(dpr);
	result.fill(Qt::transparent);
	auto p = QPainter(&result);
	auto y = 0;
	for (const auto &row : rows) {
		p.drawImage(QPoint(0, y), row, QRect(left, 0, right - left + 1, row.height()));
		y += row.height();
	}
	p.end();
	SmoothEdges(result, std::max(1, int(std::round(dpr * 0.8))));
	return result;
}

[[nodiscard]] QImage ApplyMasks(QImage content, const std::vector<MaskRect> &masks) {
	if (content.isNull()) {
		return content;
	}
	// Рисуем в пикселях, поэтому масштаб экрана на время снимаем.
	const auto dpr = content.devicePixelRatio();
	content.setDevicePixelRatio(1.);
	auto p = QPainter(&content);
	p.setRenderHint(QPainter::Antialiasing);
	for (const auto &m : masks) {
		const auto r = QRect(
			int(m.norm.x() * content.width()),
			int(m.norm.y() * content.height()),
			std::max(2, int(m.norm.width() * content.width())),
			std::max(2, int(m.norm.height() * content.height())))
			.intersected(content.rect());
		if (r.isEmpty()) {
			continue;
		}
		if (m.mask == Mask::Fill) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(110, 110, 120));
			p.drawRoundedRect(r, 6, 6);
			continue;
		}
		const auto part = content.copy(r);
		p.setCompositionMode(QPainter::CompositionMode_Source);
		p.drawImage(r.topLeft(), Masked(part, m.mask));
		p.setCompositionMode(QPainter::CompositionMode_SourceOver);
	}
	p.end();
	content.setDevicePixelRatio(dpr);
	return content;
}

struct Composed {
	QImage image;
	QRectF content; // где в итоге лежат сообщения (пиксели итога)
};

[[nodiscard]] Composed Compose(const QImage &content, int gradient, int cardMode, int outer, bool shadow) {
	const auto dpr = std::max(1., content.devicePixelRatio());
	const auto innerPad = int(22 * dpr);
	const auto dotsH = 0;
	const auto pad = int(outer * dpr);
	const auto cardW = content.width() + innerPad * 2;
	const auto cardH = content.height() + innerPad * 2 + dotsH;
	const auto hasCard = (cardMode != 2);
	auto bgA = QColor();
	auto bgB = QColor();
	const auto hasBg = BgColors(gradient, bgA, bgB);
	const auto fullPad = (hasBg || hasCard) ? pad : 0;
	auto result = QImage(
		QSize(cardW + fullPad * 2, cardH + fullPad * 2),
		QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	auto p = QPainter(&result);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	const auto all = QRectF(result.rect());
	if (hasBg) {
		auto lg = QLinearGradient(all.topLeft(), all.bottomRight());
		lg.setColorAt(0, bgA);
		lg.setColorAt(1, bgB);
		p.setPen(Qt::NoPen);
		p.setBrush(lg);
		p.drawRect(all);
	}
	const auto card = QRectF(fullPad, fullPad, cardW, cardH);
	const auto radius = 20 * dpr;
	if (hasCard) {
		if (shadow && hasBg) {
			// Тень: размытый тёмный прямоугольник под карточкой.
			auto sh = QImage(result.size() / 8 + QSize(2, 2), QImage::Format_ARGB32_Premultiplied);
			sh.fill(Qt::transparent);
			{
				auto q = QPainter(&sh);
				q.setRenderHint(QPainter::Antialiasing);
				q.setPen(Qt::NoPen);
				q.setBrush(QColor(0, 0, 0, 150));
				q.drawRoundedRect(QRectF(card.x() / 8, (card.y() + 14 * dpr) / 8, card.width() / 8, card.height() / 8), radius / 8, radius / 8);
			}
			auto big = sh.scaled(result.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
			p.drawImage(QPoint(0, 0), big);
		}
		p.setPen(Qt::NoPen);
		p.setBrush(cardMode == 0 ? QColor(0x0e, 0x16, 0x21) : QColor(0xe9, 0xee, 0xf2));
		p.drawRoundedRect(card, radius, radius);
	}
	const auto at = QPointF(card.x() + innerPad, card.y() + innerPad + dotsH);
	p.drawImage(QRectF(at, QSizeF(content.size())), content);
	p.end();
	result.setDevicePixelRatio(1.);
	return { result, QRectF(at, QSizeF(content.size())) };
}

class Pills final : public Ui::RpWidget {
public:
	Pills(QWidget *parent, QStringList names, int current, Fn<void(int)> changed)
	: RpWidget(parent)
	, _names(std::move(names))
	, _current(current)
	, _changed(std::move(changed)) {
		setMouseTracking(true);
		const auto &font = st::normalFont;
		auto x = 0;
		for (const auto &name : _names) {
			const auto w = font->width(name) + 28;
			_rects.push_back(QRect(x, 0, w, 32));
			x += w + 8;
		}
		resize(x, 32);
	}

	void setCurrent(int current) {
		_current = current;
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		p.setFont(st::normalFont);
		for (auto i = 0; i != int(_rects.size()); ++i) {
			const auto active = (i == _current);
			p.setPen(Qt::NoPen);
			p.setBrush(active ? st::windowBgActive : st::windowBgOver);
			p.drawRoundedRect(_rects[i], 16, 16);
			p.setPen(active ? st::windowFgActive : st::windowFg);
			p.drawText(_rects[i], Qt::AlignCenter, _names[i]);
		}
	}
	void mousePressEvent(QMouseEvent *e) override {
		for (auto i = 0; i != int(_rects.size()); ++i) {
			if (_rects[i].contains(e->pos())) {
				_current = i;
				update();
				_changed(i);
				return;
			}
		}
	}

private:
	QStringList _names;
	std::vector<QRect> _rects;
	int _current = 0;
	Fn<void(int)> _changed;

};

class Swatches final : public Ui::RpWidget {
public:
	Swatches(QWidget *parent, int current, Fn<void(int)> changed)
	: RpWidget(parent)
	, _current(current)
	, _changed(std::move(changed)) {
		resize(int(Gradients().size() + 1) * 38 + 2, 32);
	}

	void refreshCustom() {
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto count = int(Gradients().size());
		for (auto i = 0; i != count + 1; ++i) {
			const auto r = QRectF(i * 38 + 5, 5, 22, 22);
			const auto index = (i < count) ? i : kCustomBg;
			auto a = QColor();
			auto b = QColor();
			BgColors(index, a, b);
			auto lg = QLinearGradient(r.topLeft(), r.bottomRight());
			lg.setColorAt(0, a);
			lg.setColorAt(1, b);
			p.setPen(Qt::NoPen);
			p.setBrush(lg);
			p.drawEllipse(r);
			if (i == count) {
				// Свой цвет: значок карандаша заменяем плюсом.
				p.setPen(QPen(Qt::white, 1.6, Qt::SolidLine, Qt::RoundCap));
				const auto c = r.center();
				p.drawLine(QPointF(c.x() - 5, c.y()), QPointF(c.x() + 5, c.y()));
				p.drawLine(QPointF(c.x(), c.y() - 5), QPointF(c.x(), c.y() + 5));
			}
			if (index == _current) {
				p.setPen(QPen(st::windowFg, 2));
				p.setBrush(Qt::NoBrush);
				p.drawEllipse(r.adjusted(-3, -3, 3, 3));
			}
		}
	}
	void mousePressEvent(QMouseEvent *e) override {
		const auto i = e->pos().x() / 38;
		const auto count = int(Gradients().size());
		if (i >= 0 && i <= count) {
			_current = (i < count) ? i : kCustomBg;
			update();
			_changed(_current);
		}
	}

private:
	int _current = 0;
	Fn<void(int)> _changed;

};

class PreviewWidget final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

	Fn<void(QPainter&)> paint;
	Fn<void(QPointF)> pressed;
	Fn<void(QPointF)> moved;
	Fn<void(QPointF)> released;

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		if (paint) {
			paint(p);
		}
	}
	void mousePressEvent(QMouseEvent *e) override {
		if (pressed) {
			pressed(e->position());
		}
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (moved) {
			moved(e->position());
		}
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		if (released) {
			released(e->position());
		}
	}

};

} // namespace

void RegisterSettings() {
	const auto section = U("Картинка-цитата");
	const auto add = [&](S::Entry entry) {
		entry.section = section;
		entry.category = u"look"_q;
		entry.sectionIcon = u"copy"_q;
		S::Register(std::move(entry));
	};
	QStringList bgs;
	for (const auto &g : Gradients()) {
		bgs.push_back(U(g.name));
	}
	add({
		.key = u"quote.bg"_q,
		.title = U("Фон картинки"),
		.description = U("Меню «Скриншот сообщений» собирает картинку из сообщений как в чате."),
		.kind = S::Kind::Choice,
		.defaultValue = 5,
		.choices = bgs,
	});
	add({
		.key = u"quote.custom1"_q,
		.title = U("Свой фон: цвет 1"),
		.kind = S::Kind::Color,
		.defaultValue = QString(u"#ff9a8b"_q),
	});
	add({
		.key = u"quote.custom2"_q,
		.title = U("Свой фон: цвет 2"),
		.kind = S::Kind::Color,
		.defaultValue = QString(u"#6a11cb"_q),
	});
	add({
		.key = u"quote.theme"_q,
		.title = U("Карточка"),
		.kind = S::Kind::Choice,
		.defaultValue = 0,
		.choices = { U("Тёмная"), U("Светлая"), U("Без карточки") },
	});
	add({
		.key = u"quote.quality"_q,
		.title = U("Качество картинки"),
		.description = U("Во сколько раз больше, чем на экране. Выше: чётче и тяжелее."),
		.kind = S::Kind::Choice,
		.defaultValue = 1,
		.choices = { U("2×"), U("3×"), U("4×") },
	});
	add({
		.key = u"quote.padding"_q,
		.title = U("Отступ от края, px"),
		.kind = S::Kind::Int,
		.defaultValue = 20,
		.minValue = 0,
		.maxValue = 120,
	});
	add({
		.key = u"quote.shadow"_q,
		.title = U("Тень под карточкой"),
		.kind = S::Kind::Bool,
		.defaultValue = false,
	});
	add({
		.key = u"quote.width"_q,
		.title = U("Ширина текстовой цитаты, px"),
		.description = U("Только если сообщения не на экране и рисуются как текст."),
		.kind = S::Kind::Int,
		.defaultValue = 640,
		.minValue = 360,
		.maxValue = 1000,
	});
}


// Идея и схема отрисовки (отдельные Element со своим делегатом, рендер вне
// окна) взяты из функции Message Shot в AyuGram Desktop (GPL, Radolyn).
class ShotDelegate final : public HistoryView::DefaultElementDelegate {
public:
	ShotDelegate(not_null<const Ui::ChatStyle*> st, not_null<History*> history)
	: _pathGradient(HistoryView::MakePathShiftGradient(st, [] {}))
	, _history(history) {
	}

	bool elementAnimationsPaused() override {
		return true;
	}
	not_null<Ui::PathShiftGradient*> elementPathShiftGradient() override {
		return _pathGradient.get();
	}
	HistoryView::Context elementContext() override {
		return HistoryView::Context::AdminLog;
	}
	bool elementHideReply(not_null<const HistoryView::Element*> view) override {
		if (const auto reply = view->data()->Get<HistoryMessageReply>()) {
			const auto replyToPeerId = reply->externalPeerId()
				? reply->externalPeerId()
				: _history->peer->id;
			if (reply->fields().manualQuote) {
				return false;
			} else if (replyToPeerId == _history->peer->id) {
				return _history->asForum()
					&& _history->asForum()->topicFor(reply->messageId());
			}
		}
		return false;
	}
	HistoryView::ElementChatMode elementChatMode() override {
		return HistoryView::ElementChatMode::Wide;
	}

private:
	const std::unique_ptr<Ui::PathShiftGradient> _pathGradient;
	const not_null<History*> _history;

};

struct Shot {
	not_null<Window::SessionController*> controller;
	std::vector<not_null<HistoryItem*>> items;
	std::unique_ptr<ShotDelegate> delegate;
	std::vector<std::unique_ptr<HistoryView::Element>> views;
};

// На время снимка поднимаем style::DevicePixelRatio: эмодзи, углы и
// аватарки строятся сразу в большом разрешении. Создание Element тоже
// должно идти под этим же значением.
struct EmojiScale {
	explicit EmojiScale(int scale) : old(Ui::CustomEmoji::ShotScale()) {
		Ui::CustomEmoji::ShotScale() = std::max(1, scale);
	}
	~EmojiScale() {
		Ui::CustomEmoji::ShotScale() = old;
	}
	int old = 1;
};

struct RatioScope {
	explicit RatioScope(int ratio) : old(style::DevicePixelRatio()) {
		style::SetDevicePixelRatio(std::max(ratio, old));
	}
	~RatioScope() {
		style::SetDevicePixelRatio(old);
	}
	int old = 1;
};

[[nodiscard]] std::shared_ptr<Shot> MakeShot(
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
	if (items.empty()) {
		return nullptr;
	}
	auto result = std::make_shared<Shot>(Shot{ controller, items });
	result->delegate = std::make_unique<ShotDelegate>(
		controller->chatStyle(),
		items.front()->history());
	for (const auto item : items) {
		result->views.push_back(item->createView(result->delegate.get()));
	}
	const auto &v = result->views;
	for (auto i = 1; i < int(v.size()); ++i) {
		const auto prev = v[i - 1].get();
		const auto next = v[i].get();
		if (next->isHidden()) {
			next->setDisplayDate(false);
			continue;
		}
		next->setDisplayDate(
			next->dateTime().date() != prev->dateTime().date());
		const auto attached = next->computeIsAttachToPrevious(prev);
		next->setAttachToPrevious(attached, prev);
		prev->setAttachToNext(attached, next);
	}
	v.front()->setAttachToPrevious(false);
	v.back()->setAttachToNext(false);
	return result;
}

// Рисуем с поднятым style::DevicePixelRatio: эмодзи, углы и аватарки
// строятся сразу в большом разрешении.
[[nodiscard]] QImage RenderShot(Shot &shot, int ratio) {
	// style::DevicePixelRatio не трогаем: размеры эмодзи в Telegram жёстко
	// привязаны к нему. Чёткость даёт масштаб самой картинки.
	const auto dpr = double(std::max(ratio, style::DevicePixelRatio()));
	const auto emojiScale = EmojiScale(ratio);

	const auto width = st::msgMaxWidth + st::boxPadding.left() + st::boxPadding.right();
	auto height = 0;
	for (const auto &view : shot.views) {
		view->itemDataChanged();
		height += view->resizeGetHeight(width);
	}
	auto image = QImage(QSize(int(width * dpr), int(height * dpr)), QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(dpr);
	image.fill(Qt::transparent);
	Painter p(&image);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.setRenderHint(QPainter::TextAntialiasing);
	const auto st = shot.controller->chatStyle();
	const auto viewport = QRect(0, 0, width, height);
	base::flat_map<not_null<PeerData*>, Ui::PeerUserpicView> userpics;
	auto y = 0;
	for (auto i = 0; i != int(shot.items.size()); ++i) {
		const auto item = shot.items[i];
		const auto &view = shot.views[i];
		const auto rect = QRect(0, 0, width, view->height());
		auto context = shot.controller->defaultChatTheme()->preparePaintContext(
			st,
			viewport,
			rect,
			rect,
			true);
		p.translate(0, y);
		view->draw(p, context);
		p.translate(0, -y);
		if (view->displayFromPhoto() || item->isPost()) {
			if (const auto from = item->displayFrom()) {
				const auto size = st::msgPhotoSize;
				const auto x = st::msgMargin.left();
				const auto ay = y + view->height() - size;
				auto &userpic = userpics[from];
				const auto cloud = from->userpicCloudImage(userpic);
				if (cloud && !cloud->isNull()) {
					// Исходная картинка аватарки, а не кеш размером с экран.
					auto path = QPainterPath();
					path.addEllipse(QRectF(x, ay, size, size));
					p.setClipPath(path);
					p.drawImage(QRectF(x, ay, size, size), *cloud);
					p.setClipping(false);
				} else {
					Dialogs::Ui::PaintUserpic(
						p, from, nullptr, userpic, x, ay, width, size, true);
				}
			}
		}
		y += view->height();
	}
	p.end();
	return image;
}

void ShowQuoteBoxNow(
		not_null<Window::SessionController*> controller,
		std::shared_ptr<Shot> shot,
		int ratio);

void ShowQuoteBox(
		not_null<Window::SessionController*> controller,
		std::vector<FullMsgId> ids) {
	const auto ratio = 2 + std::clamp(S::Int(u"quote.quality"_q), 0, 2);
	const auto shot = [&] {
		const auto scale = EmojiScale(ratio);
		return MakeShot(controller, ids);
	}();
	if (!shot) {
		return;
	}
	// Первый проход запускает загрузку эмодзи и медиа; окно показываем,
	// когда они готовы, чтобы ничего не прыгало на глазах.
	auto warm = RenderShot(*shot, ratio);
	Q_UNUSED(warm);
	base::call_delayed(900, controller, [=] {
		ShowQuoteBoxNow(controller, shot, ratio);
	});
}

void ShowCustomBg(Fn<void()> refresh) {
	const auto window = Core::App().activeWindow();
	if (!window) {
		return;
	}
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(U("Свой фон")));
		box->setWidth(360);
		const auto content = box->verticalLayout();
		const auto add = [&](const QString &key, const QString &label) {
			const auto field = content->add(
				object_ptr<Ui::InputField>(
					content,
					st::defaultInputField,
					rpl::single(label),
					S::String(key)),
				QMargins(22, 8, 22, 4));
			field->changes() | rpl::on_next([=] {
				const auto text = field->getLastText().trimmed();
				if (QColor(text).isValid()) {
					S::SetString(key, text);
					refresh();
				}
			}, field->lifetime());
		};
		add(u"quote.custom1"_q, U("Цвет 1, например #ff9a8b"));
		add(u"quote.custom2"_q, U("Цвет 2, например #6a11cb"));
		box->addButton(rpl::single(U("Готово")), [=] { box->closeBox(); });
	}));
}

void ShowQuoteBoxNow(
		not_null<Window::SessionController*> controller,
		std::shared_ptr<Shot> shot,
		int ratio) {
	auto content = std::make_shared<QImage>(StackRows({ RenderShot(*shot, ratio) }));
	if (content->isNull()) {
		return;
	}
	const auto onUpdate = std::make_shared<Fn<void()>>();
	// Что догрузилось позже, подхватываем перерисовкой.
	for (const auto delay : { 1200, 3000 }) {
		base::call_delayed(delay, controller, [=] {
			auto fresh = StackRows({ RenderShot(*shot, ratio) });
			if (!fresh.isNull()) {
				if (qEnvironmentVariableIsSet("MOKAGRAM_DEBUG_CMD")) {
					fresh.save(QString(u"/tmp/moka_quote_%1.png"_q).arg(delay), "PNG");
				}
				*content = std::move(fresh);
				if (*onUpdate) {
					(*onUpdate)();
				}
			}
		});
	}
	if (qEnvironmentVariableIsSet("MOKAGRAM_DEBUG_CMD")) {
		content->save(u"/tmp/moka_quote_content.png"_q, "PNG");
	}
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(U("Скриншот сообщений")));
		box->setWidth(620);
		const auto layout = box->verticalLayout();
		const auto masks = std::make_shared<std::vector<MaskRect>>();
		const auto maskKind = std::make_shared<int>(0);
		const auto result = std::make_shared<Composed>();
		const auto shown = std::make_shared<QImage>();
		const auto shownFor = std::make_shared<QImage>();
		const auto preview = layout->add(
			object_ptr<PreviewWidget>(layout),
			QMargins(24, 8, 24, 8));
		preview->setCursor(style::cur_cross);
		const auto drag = std::make_shared<std::pair<bool, QPointF>>(false, QPointF());
		const auto dragRect = std::make_shared<QRectF>();
		const auto composedFor = [=](bool fresh = false) {
			Q_UNUSED(fresh);
			const auto masked = ApplyMasks(*content, *masks);
			return Compose(
				masked,
				S::Int(u"quote.bg"_q),
				S::Int(u"quote.theme"_q),
				S::Int(u"quote.padding"_q),
				S::Bool(u"quote.shadow"_q));
		};
		const auto refresh = [=] {
			*result = composedFor();
			const auto w = std::max(100, preview->width());
			const auto h = int(result->image.height() * (w / double(result->image.width())));
			if (preview->height() != h) {
				preview->resize(w, h);
			}
			preview->update();
		};
		preview->paint = [=](QPainter &p) {
			if (result->image.isNull()) {
				return;
			}
			const auto w = preview->width();
			const auto h = int(result->image.height() * (w / double(result->image.width())));
			// Заранее уменьшаем до физического размера превью, иначе Qt
			// жмёт огромную картинку грубо и всё выглядит мыльным.
			const auto ratio = style::DevicePixelRatio();
			const auto target = QSize(w * ratio, h * ratio);
			if (shown->size() != target || shownFor->cacheKey() != result->image.cacheKey()) {
				*shown = result->image.scaled(
					target,
					Qt::IgnoreAspectRatio,
					Qt::SmoothTransformation);
				shown->setDevicePixelRatio(ratio);
				*shownFor = result->image;
			}
			p.drawImage(QRect(0, 0, w, h), *shown);
			if (drag->first) {
				p.setPen(QPen(Qt::white, 1, Qt::DashLine));
				p.setBrush(QColor(255, 255, 255, 40));
				p.drawRect(*dragRect);
			}
		};
		const auto toNorm = [=](QRectF r) {
			const auto k = result->image.width() / double(preview->width());
			const auto px = QRectF(r.x() * k, r.y() * k, r.width() * k, r.height() * k)
				.translated(-result->content.topLeft());
			return QRectF(
				px.x() / result->content.width(),
				px.y() / result->content.height(),
				px.width() / result->content.width(),
				px.height() / result->content.height());
		};
		preview->pressed = [=](QPointF pos) {
			drag->first = true;
			drag->second = pos;
			*dragRect = QRectF(pos, pos);
		};
		preview->moved = [=](QPointF pos) {
			if (drag->first) {
				*dragRect = QRectF(drag->second, pos).normalized();
				preview->update();
			}
		};
		preview->released = [=](QPointF pos) {
			if (!drag->first) {
				return;
			}
			drag->first = false;
			*dragRect = QRectF(drag->second, pos).normalized();
			if (dragRect->width() > 6 && dragRect->height() > 6) {
				masks->push_back({ toNorm(*dragRect), Mask(*maskKind + 1) });
			}
			refresh();
		};

		const auto hint = layout->add(
			object_ptr<Ui::FlatLabel>(
				layout,
				U("Проведи мышью по картинке, чтобы скрыть имя или аватарку."),
				st::boxLabel),
			QMargins(24, 0, 24, 8));
		Q_UNUSED(hint);

		Ui::AddSubsectionTitle(layout, rpl::single(U("Фон")));
		layout->add(
			object_ptr<Swatches>(layout, S::Int(u"quote.bg"_q), [=](int i) {
				S::SetInt(u"quote.bg"_q, i);
				refresh();
				if (i == kCustomBg) {
					ShowCustomBg(refresh);
				}
			}),
			QMargins(24, 4, 24, 8));
		Ui::AddSubsectionTitle(layout, rpl::single(U("Карточка")));
		layout->add(
			object_ptr<Pills>(layout, QStringList{ U("Тёмная"), U("Светлая"), U("Без карточки") }, S::Int(u"quote.theme"_q), [=](int i) {
				S::SetInt(u"quote.theme"_q, i);
				refresh();
			}),
			QMargins(24, 4, 24, 8));
		Ui::AddSubsectionTitle(layout, rpl::single(U("Чем скрывать")));
		layout->add(
			object_ptr<Pills>(layout, QStringList{ U("Размытие"), U("Пиксели"), U("Заливка") }, 0, [=](int i) {
				*maskKind = i;
			}),
			QMargins(24, 4, 24, 8));
		const auto shadow = layout->add(
			object_ptr<Ui::Checkbox>(layout, U("Тень под карточкой"), S::Bool(u"quote.shadow"_q), st::defaultCheckbox),
			QMargins(24, 2, 24, 8));
		shadow->checkedChanges() | rpl::on_next([=](bool on) {
			S::SetBool(u"quote.shadow"_q, on);
			refresh();
		}, shadow->lifetime());
		*onUpdate = crl::guard(box, refresh);
		preview->widthValue() | rpl::on_next([=](int) {
			refresh();
		}, preview->lifetime());
		refresh();

		box->addButton(rpl::single(U("Копировать")), [=] {
			QGuiApplication::clipboard()->setImage(result->image);
			controller->showToast(U("Картинка скопирована"));
		});
		box->addButton(rpl::single(U("Сохранить")), [=] {
			auto dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
			QDir().mkpath(dir);
			const auto path = dir + u"/mokagram_"_q
				+ QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")
				+ u".png"_q;
			controller->showToast(result->image.save(path, "PNG")
				? U("Сохранено: ") + path
				: U("Не получилось сохранить"));
		});
		box->addLeftButton(rpl::single(U("Отменить скрытие")), [=] {
			if (!masks->empty()) {
				masks->pop_back();
				refresh();
			}
		});
	}));
}

} // namespace Mokagram::Quote
