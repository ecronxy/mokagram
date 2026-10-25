// Mokagram: эффекты удаления сообщений и частицы при наборе текста.
#pragma once

#include "base/basic_types.h"

class QWidget;

namespace Ui {
class InputField;
} // namespace Ui

namespace Mokagram::Effects {

void RegisterSettings();

// Сообщение в rectInSource (координаты source) рассыпается поверх host.
void PlayDelete(
	not_null<QWidget*> host,
	not_null<QWidget*> source,
	QRect rectInSource,
	bool outgoing);

} // namespace Mokagram::Effects
