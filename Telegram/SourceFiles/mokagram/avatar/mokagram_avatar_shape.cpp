// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: b4e8cd12
#include "mokagram/avatar/mokagram_avatar_shape.h"
#include "mokagram/lang/mokagram_lang.h"

#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "mokagram/settings/mokagram_settings.h"

#include <QtGui/QBrush>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QTransform>

#include <cmath>

namespace Mokagram::Avatar {
namespace {

namespace S = Mokagram::Settings;

int CurrentShape = 0;

[[nodiscard]] QString U(const char *text) {
	return QString::fromUtf8(text);
}

[[nodiscard]] const std::vector<const char*> &Names() {
	static const auto result = std::vector<const char*>{
		Mokagram::Lang::Tr("Как в Telegram"),
		Mokagram::Lang::Tr("Круг"),
		Mokagram::Lang::Tr("Скруглённый квадрат"),
		Mokagram::Lang::Tr("Суперэллипс"),
		Mokagram::Lang::Tr("Квадрат"),
		Mokagram::Lang::Tr("Шестиугольник"),
		Mokagram::Lang::Tr("Восьмиугольник"),
		Mokagram::Lang::Tr("Ромб"),
		Mokagram::Lang::Tr("Щит"),
		Mokagram::Lang::Tr("Сердце"),
		Mokagram::Lang::Tr("Звезда"),
		Mokagram::Lang::Tr("Волнистый круг"),
		Mokagram::Lang::Tr("Цветок"),
		Mokagram::Lang::Tr("Капля"),
		Mokagram::Lang::Tr("Треугольник"),
		Mokagram::Lang::Tr("Пятиугольник"),
		Mokagram::Lang::Tr("Овал вертикальный"),
		Mokagram::Lang::Tr("Овал горизонтальный"),
		Mokagram::Lang::Tr("Лист"),
		Mokagram::Lang::Tr("Скошенный квадрат"),
		Mokagram::Lang::Tr("Лепесток"),
	};
	return result;
}

// Многоугольник со скруглёнными углами в единичном квадрате.
[[nodiscard]] QPainterPath RoundedPoly(
		const std::vector<QPointF> &points,
		double radius) {
	auto path = QPainterPath();
	const auto count = int(points.size());
	for (auto i = 0; i != count; ++i) {
		const auto prev = points[(i + count - 1) % count];
		const auto cur = points[i];
		const auto next = points[(i + 1) % count];
		const auto toPrev = prev - cur;
		const auto toNext = next - cur;
		const auto lenPrev = std::hypot(toPrev.x(), toPrev.y());
		const auto lenNext = std::hypot(toNext.x(), toNext.y());
		const auto r = std::min({ radius, lenPrev / 2., lenNext / 2. });
		const auto start = cur + toPrev * (r / lenPrev);
		const auto end = cur + toNext * (r / lenNext);
		if (i == 0) {
			path.moveTo(start);
		} else {
			path.lineTo(start);
		}
		path.quadTo(cur, end);
	}
	path.closeSubpath();
	return path;
}

[[nodiscard]] std::vector<QPointF> RegularPolygon(
		int sides,
		double radius,
		double startAngle) {
	auto result = std::vector<QPointF>();
	for (auto i = 0; i != sides; ++i) {
		const auto a = startAngle + i * 2 * M_PI / sides;
		result.emplace_back(0.5 + radius * std::cos(a), 0.5 + radius * std::sin(a));
	}
	return result;
}

// Кривая r(θ) вокруг центра квадрата.
template <typename Radius>
[[nodiscard]] QPainterPath Polar(Radius &&radius) {
	auto path = QPainterPath();
	constexpr auto kSteps = 240;
	for (auto i = 0; i != kSteps; ++i) {
		const auto a = i * 2 * M_PI / kSteps;
		const auto r = radius(a);
		const auto point = QPointF(0.5 + r * std::cos(a), 0.5 + r * std::sin(a));
		if (i == 0) {
			path.moveTo(point);
		} else {
			path.lineTo(point);
		}
	}
	path.closeSubpath();
	return path;
}

[[nodiscard]] QPainterPath UnitPath(int shape) {
	auto path = QPainterPath();
	switch (shape) {
	case 1:
		path.addEllipse(QRectF(0, 0, 1, 1));
		break;
	case 2:
		path.addRoundedRect(QRectF(0, 0, 1, 1), 0.3, 0.3);
		break;
	case 3:
		path = Polar([](double a) {
			const auto c = std::abs(std::cos(a));
			const auto s = std::abs(std::sin(a));
			return 0.5 / std::pow(std::pow(c, 4) + std::pow(s, 4), 0.25);
		});
		break;
	case 4:
		path.addRoundedRect(QRectF(0, 0, 1, 1), 0.1, 0.1);
		break;
	case 5:
		path = RoundedPoly(RegularPolygon(6, 0.5, -M_PI / 2), 0.14);
		break;
	case 6:
		path = RoundedPoly(RegularPolygon(8, 0.5, -M_PI / 8), 0.1);
		break;
	case 7:
		path = RoundedPoly({
			{ 0.5, 0.0 }, { 1.0, 0.5 }, { 0.5, 1.0 }, { 0.0, 0.5 },
		}, 0.16);
		break;
	case 8:
		path = RoundedPoly({
			{ 0.08, 0.04 }, { 0.92, 0.04 }, { 0.92, 0.55 },
			{ 0.5, 0.98 }, { 0.08, 0.55 },
		}, 0.14);
		break;
	case 9:
		path.moveTo(0.5, 0.95);
		path.cubicTo(0.15, 0.7, 0.02, 0.5, 0.02, 0.3);
		path.cubicTo(0.02, 0.12, 0.17, 0.03, 0.3, 0.03);
		path.cubicTo(0.4, 0.03, 0.47, 0.09, 0.5, 0.16);
		path.cubicTo(0.53, 0.09, 0.6, 0.03, 0.7, 0.03);
		path.cubicTo(0.83, 0.03, 0.98, 0.12, 0.98, 0.3);
		path.cubicTo(0.98, 0.5, 0.85, 0.7, 0.5, 0.95);
		path.closeSubpath();
		break;
	case 10: {
		auto points = std::vector<QPointF>();
		for (auto i = 0; i != 10; ++i) {
			const auto a = -M_PI / 2 + i * M_PI / 5;
			const auto r = (i % 2) ? 0.23 : 0.5;
			points.emplace_back(0.5 + r * std::cos(a), 0.53 + r * std::sin(a));
		}
		path = RoundedPoly(points, 0.07);
	} break;
	case 11:
		path = Polar([](double a) { return 0.43 + 0.07 * std::cos(10 * a); });
		break;
	case 12:
		path = Polar([](double a) { return 0.37 + 0.13 * std::cos(6 * a); });
		break;
	case 13:
		path.moveTo(0.5, 0.02);
		path.cubicTo(0.65, 0.25, 0.92, 0.38, 0.92, 0.62);
		path.cubicTo(0.92, 0.84, 0.74, 0.98, 0.5, 0.98);
		path.cubicTo(0.26, 0.98, 0.08, 0.84, 0.08, 0.62);
		path.cubicTo(0.08, 0.38, 0.35, 0.25, 0.5, 0.02);
		path.closeSubpath();
		break;
	case 14:
		path = RoundedPoly({
			{ 0.5, 0.06 }, { 0.97, 0.9 }, { 0.03, 0.9 },
		}, 0.2);
		break;
	case 15:
		path = RoundedPoly(RegularPolygon(5, 0.52, -M_PI / 2), 0.14);
		break;
	case 16:
		path.addEllipse(QRectF(0.14, 0, 0.72, 1));
		break;
	case 17:
		path.addEllipse(QRectF(0, 0.14, 1, 0.72));
		break;
	case 18: {
		const auto big = 0.5;
		const auto small = 0.08;
		path.moveTo(big, 0);
		path.lineTo(1 - small, 0);
		path.arcTo(QRectF(1 - 2 * small, 0, 2 * small, 2 * small), 90, -90);
		path.lineTo(1, 1 - big);
		path.arcTo(QRectF(1 - 2 * big, 1 - 2 * big, 2 * big, 2 * big), 0, -90);
		path.lineTo(small, 1);
		path.arcTo(QRectF(0, 1 - 2 * small, 2 * small, 2 * small), 270, -90);
		path.lineTo(0, big);
		path.arcTo(QRectF(0, 0, 2 * big, 2 * big), 180, -90);
		path.closeSubpath();
	} break;
	case 19:
		path = RoundedPoly({
			{ 0.28, 0.0 }, { 0.72, 0.0 }, { 1.0, 0.28 }, { 1.0, 0.72 },
			{ 0.72, 1.0 }, { 0.28, 1.0 }, { 0.0, 0.72 }, { 0.0, 0.28 },
		}, 0.03);
		break;
	case 20: {
		const auto big = 0.5;
		path.moveTo(big, 0);
		path.arcTo(QRectF(1 - 2 * big, 0, 2 * big, 2 * big), 90, -90);
		path.lineTo(1, 1);
		path.lineTo(big, 1);
		path.arcTo(QRectF(0, 1 - 2 * big, 2 * big, 2 * big), 270, -90);
		path.lineTo(0, big);
		path.arcTo(QRectF(0, 0, 2 * big, 2 * big), 180, -90);
		path.closeSubpath();
	} break;
	default:
		path.addEllipse(QRectF(0, 0, 1, 1));
		break;
	}
	return path;
}

[[nodiscard]] QString KeyFor(int type) {
	switch (type) {
	case 0: return u"avatar.shape.user"_q;
	case 1: return u"avatar.shape.bot"_q;
	case 2: return u"avatar.shape.group"_q;
	case 3: return u"avatar.shape.channel"_q;
	default: return u"avatar.shape.forum"_q;
	}
}

} // namespace

void RegisterSettings() {
	auto choices = QStringList();
	for (const auto name : Names()) {
		choices.push_back(U(name));
	}
	const auto add = [&](int type, const char *title) {
		S::Register({
			.key = KeyFor(type),
			.section = U(Mokagram::Lang::Tr("Формы аватарок")),
			.title = U(title),
			.kind = S::Kind::Choice,
			.defaultValue = 0,
			.choices = choices,
			.category = u"look"_q,
			.sectionIcon = u"person"_q,
		});
	};
	add(0, Mokagram::Lang::Tr("Люди"));
	add(1, Mokagram::Lang::Tr("Боты"));
	add(2, Mokagram::Lang::Tr("Группы"));
	add(3, Mokagram::Lang::Tr("Каналы"));
	add(4, Mokagram::Lang::Tr("Форумы"));
}

int ShapeFor(not_null<const PeerData*> peer) {
	const auto type = peer->isUser()
		? (peer->asUser()->isBot() ? 1 : 0)
		: peer->isForum()
		? 4
		: peer->isChat()
		? 2
		: peer->isMegagroup()
		? 2
		: peer->isChannel()
		? 3
		: 0;
	return std::clamp(S::Int(KeyFor(type)), 0, int(Names().size()) - 1);
}

int Current() {
	return CurrentShape;
}

Scope::Scope(int shape) : _old(CurrentShape) {
	CurrentShape = shape;
}

Scope::~Scope() {
	CurrentShape = _old;
}

QImage ApplyShape(const QImage &square, int shape) {
	auto result = QImage(square.size(), QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	auto p = QPainter(&result);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.setPen(Qt::NoPen);
	p.setBrush(QBrush(square));
	p.drawPath(QTransform::fromScale(square.width(), square.height())
		.map(UnitPath(shape)));
	return result;
}

} // namespace Mokagram::Avatar
