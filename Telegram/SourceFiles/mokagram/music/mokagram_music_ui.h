// Mokagram: окно «Музыка» (поиск SoundCloud, очередь, воспроизведение через плеер клиента).
#pragma once

#include "rpl/producer.h"

class AudioMsgId;

namespace Window {
class SessionController;
} // namespace Window

namespace Mokagram::Music {

void ShowMusicBox(
	not_null<Window::SessionController*> controller,
	const QString &initialQuery = QString());

// Мост к плееру клиента: если играет трек из очереди SoundCloud, кнопки
// «вперёд/назад», повтор и перемешивание работают по этой очереди.
[[nodiscard]] bool OwnsTrack(const AudioMsgId &id);
[[nodiscard]] bool NextAvailable();
[[nodiscard]] bool PreviousAvailable();
void Step(int delta);
[[nodiscard]] rpl::producer<> QueueChanges();

// Полноценный плеер: обложка, перемотка, кнопки, очередь.
void ShowPlayerBox(not_null<Window::SessionController*> controller);

void DebugSaveTrack(int index);
void DebugPlay(int index);

} // namespace Mokagram::Music
