// Mokagram: окно «Музыка» (поиск SoundCloud, очередь, воспроизведение через плеер клиента).
#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace Mokagram::Music {

void ShowMusicBox(
	not_null<Window::SessionController*> controller,
	const QString &initialQuery = QString());

} // namespace Mokagram::Music
