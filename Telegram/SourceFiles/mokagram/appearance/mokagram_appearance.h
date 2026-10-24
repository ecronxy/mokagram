// Mokagram: оформление, настраиваемое в «Кастомизации» (пузыри сообщений, цвета плеера).
#pragma once

#include <QtGui/QColor>

namespace Mokagram::Appearance {

void RegisterSettings();

// Применяет настройки к палитре и следит за их изменением и сменой темы.
void Start();

// Цвет из настройки вида "#rrggbb"; пустой или неверный даёт невалидный QColor.
[[nodiscard]] QColor ColorSetting(const QString &key);

} // namespace Mokagram::Appearance
