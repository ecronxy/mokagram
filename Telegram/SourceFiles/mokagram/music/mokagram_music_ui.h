// Mokagram: окно «Музыка» (поиск SoundCloud, очередь, воспроизведение через плеер клиента).
#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace Mokagram::Music {

void ShowMusicBox(
	not_null<Window::SessionController*> controller,
	const QString &initialQuery = QString());

// Полноценный плеер: обложка, перемотка, кнопки, очередь.
void ShowPlayerBox(not_null<Window::SessionController*> controller);

void DebugSaveTrack(int index);

} // namespace Mokagram::Music
