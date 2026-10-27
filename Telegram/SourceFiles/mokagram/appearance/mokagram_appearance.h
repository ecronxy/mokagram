// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: fbe4699b
// Mokagram: оформление, настраиваемое в «Кастомизации» (пузыри сообщений, цвета плеера).
#pragma once

#include <QtGui/QColor>

namespace Mokagram::Appearance {

void RegisterSettings();

// Применяет настройки к палитре и следит за их изменением и сменой темы.
void Start();

// Цвет из настройки вида "#rrggbb"; пустой или неверный даёт невалидный QColor.
[[nodiscard]] QColor ColorSetting(const QString &key);

// Сколько сообщений можно выделить за раз (100 или 3000 по настройке).
[[nodiscard]] int SelectionLimit();

} // namespace Mokagram::Appearance
