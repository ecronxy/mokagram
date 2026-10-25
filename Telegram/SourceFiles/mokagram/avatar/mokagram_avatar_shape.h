// Mokagram: формы аватарок (круг, сердце, звезда и т.д.) отдельно для людей,
// ботов, групп, каналов и форумов.
#pragma once

#include <QtGui/QImage>

class PeerData;

namespace Mokagram::Avatar {

void RegisterSettings();

// 0: оставить как в Telegram. Иначе номер формы.
[[nodiscard]] int ShapeFor(not_null<const PeerData*> peer);

// Форма, выбранная для аватарки, которую сейчас строит кеш.
[[nodiscard]] int Current();

struct Scope {
	explicit Scope(int shape);
	~Scope();

private:
	int _old = 0;
};

// Вырезает форму из квадратной картинки (сглаженный край).
[[nodiscard]] QImage ApplyShape(const QImage &square, int shape);

} // namespace Mokagram::Avatar
