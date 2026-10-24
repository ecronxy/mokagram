// Mokagram: лайки, история прослушиваний и персональная подборка «Для вас».
#pragma once

#include "mokagram/music/mokagram_soundcloud.h"

#include "rpl/producer.h"

#include <vector>

namespace Mokagram::Music {

// Лайки. Хранятся на диске (JSON), новые сверху.
[[nodiscard]] bool IsLiked(const QString &trackId);
void SetLiked(const Track &track, bool liked);
[[nodiscard]] std::vector<Track> LikedTracks();
[[nodiscard]] rpl::producer<> LikesChanged();

// История: сколько реально прослушано. finished: трек доигран до конца.
// Пропуск (мало прослушано) считается сильным сигналом «не нравится».
void RecordListen(const Track &track, int listenedMs, bool finished);

// Есть ли данные для рекомендаций (лайки или история).
[[nodiscard]] bool HasTaste();

// Собирает подборку. note: пояснение для интерфейса (например, «лайкни пару треков»).
void BuildForYou(
	int count,
	Fn<void(std::vector<Track> tracks, QString note)> done);

} // namespace Mokagram::Music
