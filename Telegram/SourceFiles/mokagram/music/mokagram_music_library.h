// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 80ddf4c8
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

// Треки Telegram в очереди живут под id «telegram:...», в подборках не участвуют.
[[nodiscard]] bool IsTelegramId(const QString &trackId);

// Явная обратная связь из плеера. «Больше такого» усиливает вкус сильнее лайка,
// блокировка автора навсегда убирает его из подборок и потока.
void BoostTrack(const Track &track);
void BlockArtist(const Track &track);
void UnblockArtist(const QString &artistId);
[[nodiscard]] bool IsArtistBlocked(const Track &track);

// Контекст бесконечного потока: что играет сейчас и что недавно было или уже в очереди.
struct StreamContext {
	Track current;
	std::vector<Track> avoid;
};

// Пополнение потока: подборка с учётом сессии (последние дослушанные), плавных
// переходов, дедупликации и кулдаунов по истории на диске.
void BuildStream(
	StreamContext context,
	int count,
	Fn<void(std::vector<Track> tracks, QString note)> done);

// Собирает подборку. note: пояснение для интерфейса (например, «лайкни пару треков»).
void BuildForYou(
	int count,
	Fn<void(std::vector<Track> tracks, QString note)> done);

} // namespace Mokagram::Music
