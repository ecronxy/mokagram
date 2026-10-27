// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 81cc2956
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
