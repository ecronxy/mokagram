// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 39e5d90b
// Mokagram: тексты песен через открытый сервис LRCLIB (с временными метками).
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <vector>

class QImage;

namespace Mokagram::Music {

struct LyricLine {
	qint64 timeMs = 0;
	QString text;
};

struct Lyrics {
	std::vector<LyricLine> lines;
	bool synced = false;
	bool found = false;
};

// done вызывается в главном потоке; результат кэшируется по ключу трека.
void FetchLyrics(
	const QString &cacheKey,
	const QString &title,
	const QString &artist,
	qint64 durationMs,
	Fn<void(Lyrics)> done);

// Обложка в высоком качестве из открытых каталогов (iTunes, затем Deezer),
// только если совпали название, исполнитель и длительность. Пустая, если нет.
void FetchArtwork(
	const QString &cacheKey,
	const QString &title,
	const QString &artist,
	qint64 durationMs,
	Fn<void(QImage)> done);

// Название без хвостов вроде "(Official Video)" и без имени исполнителя в начале.
[[nodiscard]] QString CleanTitle(const QString &title, const QString &artist);

} // namespace Mokagram::Music
