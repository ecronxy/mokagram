// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: f2f6d32e
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
