// Mokagram: тексты песен через открытый сервис LRCLIB (с временными метками).
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <vector>

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

// Название без хвостов вроде "(Official Video)" и без имени исполнителя в начале.
[[nodiscard]] QString CleanTitle(const QString &title, const QString &artist);

} // namespace Mokagram::Music
