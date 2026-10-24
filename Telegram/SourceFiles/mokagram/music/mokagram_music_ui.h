// Mokagram: окно «Музыка» (поиск SoundCloud, очередь, воспроизведение через плеер клиента).
#pragma once

#include <QtCore/QPoint>
#include <QtCore/QString>
#include <QtGui/QColor>

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

// Название и исполнитель текущего трека из очереди (false, если играет чужой трек).
[[nodiscard]] bool CurrentTrackInfo(QString &title, QString &artist);

// Главный цвет обложки играющего трека (невалидный, если выключено в настройках
// или играет не трек из очереди).
[[nodiscard]] QColor CurrentCoverAccent();

// Меню по правой кнопке на панели плеера: то же, что в окне музыки, плюс управление.
void ShowCurrentTrackMenu(
	not_null<Window::SessionController*> controller,
	QPoint globalPosition);

// Полноценный плеер: обложка, перемотка, кнопки, очередь.
void ShowPlayerBox(not_null<Window::SessionController*> controller);

void DebugSaveTrack(int index);
void DebugPlay(int index);
void DebugLike(int index);
void DebugTab(int tab);

} // namespace Mokagram::Music
