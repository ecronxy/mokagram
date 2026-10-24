// Mokagram: источник музыки SoundCloud через неофициальный api-v2.
// client_id берётся из публичных JS-файлов сайта, кэшируется и обновляется при 401/403.
#pragma once

#include "base/basic_types.h"
#include "rpl/producer.h"

#include <QtCore/QString>
#include <QtGui/QImage>
#include <vector>

namespace Mokagram::Music {

struct Track {
	QString id;
	QString title;
	QString artist;
	QString artworkUrl;      // 100x100, для крупной заменить на t500x500
	QString permalink;
	QString transcodingUrl;  // выбранный вариант потока (progressive mp3 или hls)
	QString authorization;   // track_authorization
	QString extension;       // "mp3" или "m4a"
	int durationMs = 0;
	bool hls = false;
};

struct SearchResult {
	std::vector<Track> tracks;
	QString error;
	int nextOffset = 0;      // 0: больше нет
};

// Все колбэки вызываются в главном потоке. Запросы можно звать только из главного потока.
void Search(
	const QString &query,
	int offset,
	Fn<void(SearchResult)> done);

// Скачивает трек (HLS склеивается) в кэш и отдаёт путь к файлу. Уже скачанный отдаётся сразу.
void Download(
	const Track &track,
	Fn<void(const QString &path, const QString &error)> done);

// Картинка по ссылке (обложки). Результат кэшируется в памяти.
void LoadImage(const QString &url, Fn<void(QImage)> done);

// Обложка трека SoundCloud по id документа клиента (пока не загружена, пустая).
[[nodiscard]] QImage CoverForDocument(uint64 documentId);
void SetCover(uint64 documentId, QImage image);
[[nodiscard]] rpl::producer<> CoversChanged();
[[nodiscard]] uint64 DocumentIdForTrack(const QString &trackId);

// Папка кэша скачанных треков.
[[nodiscard]] QString CacheDir();

} // namespace Mokagram::Music
