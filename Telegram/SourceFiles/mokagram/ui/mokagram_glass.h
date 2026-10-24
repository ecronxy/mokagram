// Mokagram: подложка liquid glass по рецепту стекла таб-бара кита TIER:
// заливка section-bg 70%, мягкая тень 0 0 10px 10% и кромка 1px (градиент 8deg,
// 32% белого в середине). Размытие подложки Qt не умеет, его здесь нет.
#pragma once

#include <QtGui/QPainter>
#include <QtGui/QLinearGradient>

namespace Mokagram::Glass {

inline void PaintGlass(QPainter &p, QRectF rect, qreal radius) {
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	for (auto i = 5; i >= 1; --i) {
		p.setBrush(QColor(0, 0, 0, 4));
		p.drawRoundedRect(
			rect.adjusted(-i, -i, i, i),
			radius + i,
			radius + i);
	}
	p.setBrush(QColor(44, 44, 46, 179));
	p.drawRoundedRect(rect, radius, radius);
	p.setBrush(QColor(120, 120, 128, 3));
	p.drawRoundedRect(rect, radius, radius);
	auto edge = QLinearGradient(rect.bottomLeft(), rect.topRight());
	edge.setColorAt(0., QColor(255, 255, 255, 0));
	edge.setColorAt(0.35, QColor(255, 255, 255, 82));
	edge.setColorAt(0.65, QColor(255, 255, 255, 82));
	edge.setColorAt(1., QColor(255, 255, 255, 0));
	p.setPen(QPen(QBrush(edge), 1.));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(
		rect.adjusted(0.5, 0.5, -0.5, -0.5),
		radius - 0.5,
		radius - 0.5);
	p.restore();
}

} // namespace Mokagram::Glass
