// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: c7df01d5
// Mokagram: источник музыки SoundCloud через неофициальный api-v2.
// client_id берётся из публичных JS-файлов сайта, кэшируется и обновляется при 401/403.
#pragma once

#include "base/basic_types.h"
#include "rpl/producer.h"

#include <QtCore/QString>
#include <QtCore/QStringList>
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
	// Для рекомендаций.
	QString artistId;
	QString genre;
	QStringList tags;
	qint64 playbackCount = 0;
	qint64 likesCount = 0;
	qint64 createdAt = 0;    // unix time
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

// Достаёт свежие ссылки на поток по id (нужно для сохранённых лайков: ссылки живут недолго).
void Resolve(
	const Track &track,
	Fn<void(Track, const QString &error)> done);

// Похожие на трек.
void Related(
	const QString &trackId,
	int limit,
	Fn<void(std::vector<Track>)> done);

// Последние треки исполнителя.
void UserTracks(
	const QString &userId,
	int limit,
	Fn<void(std::vector<Track>)> done);

struct Album {
	QString id;
	QString title;
	QString artist;
	QString artworkUrl;
	int trackCount = 0;
};

// Альбомы по запросу.
void SearchAlbums(
	const QString &query,
	int limit,
	Fn<void(std::vector<Album>, QString error)> done);

// Альбомы, в которые входит трек.
void TrackAlbums(
	const QString &trackId,
	Fn<void(std::vector<Album>)> done);

// Треки альбома по порядку: недостающие в ответе подгружаются отдельно по id.
void AlbumTracks(
	const QString &albumId,
	Fn<void(std::vector<Track>, QString error)> done);

// Поиск с произвольным лимитом (для подбора кандидатов).
void SearchLimited(
	const QString &query,
	int limit,
	Fn<void(std::vector<Track>)> done);

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
