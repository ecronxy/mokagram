// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 7640bdb1
// Mokagram: магазин плагинов (подписанный каталог в репозитории mokagram-plugins).
#pragma once

#include "base/basic_types.h"

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Mokagram::Plugins {

// Окно «Магазин»: каталог грузится с GitHub, подпись проверяется ключом mokaGram,
// файл плагина сверяется с хешем из каталога и отдаётся обычному окну установки.
void StoreBox(
	not_null<Ui::GenericBox*> box,
	Window::SessionController *controller,
	Fn<void()> changed);

// Раз в 12 часов (первая проверка через 90 секунд после запуска) сверяет версии из магазина
// и тихо обновляет плагины, если права не выросли; иначе подсказывает открыть магазин.
void StartPluginAutoUpdate();

} // namespace Mokagram::Plugins
