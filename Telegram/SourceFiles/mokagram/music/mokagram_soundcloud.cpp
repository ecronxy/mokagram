// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: cfb896c6
#include "mokagram/music/mokagram_soundcloud.h"
#include "mokagram/lang/mokagram_lang.h"

#include "core/application.h"
#include "rpl/event_stream.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <map>
#include <memory>

namespace Mokagram::Music {
namespace {

constexpr auto kApi = "https://api-v2.soundcloud.com";
constexpr auto kHome = "https://soundcloud.com/";
constexpr auto kPageSize = 20;
constexpr auto kUserAgent = "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0";

QNetworkAccessManager &Net() {
	static auto manager = new QNetworkAccessManager();
	return *manager;
}

[[nodiscard]] QString ClientIdFile() {
	return CacheDir() + u"/client_id.txt"_q;
}

QString &ClientId() {
	static auto id = [] {
		auto file = QFile(ClientIdFile());
		return file.open(QIODevice::ReadOnly)
			? QString::fromUtf8(file.readAll()).trimmed()
			: QString();
	}();
	return id;
}

QNetworkReply *Get(const QUrl &url) {
	auto request = QNetworkRequest(url);
	request.setRawHeader("User-Agent", kUserAgent);
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setTransferTimeout(20000);
	return Net().get(request);
}

// Ищет client_id в скриптах главной страницы (в последних скриптах он обычно есть).
void FetchClientId(Fn<void(QString)> done) {
	const auto home = Get(QUrl(QString::fromLatin1(kHome)));
	QObject::connect(home, &QNetworkReply::finished, [=] {
		const auto html = QString::fromUtf8(home->readAll());
		home->deleteLater();
		auto scripts = QStringList();
		auto match = QRegularExpression(
			uR"(https://a-v2\.sndcdn\.com/assets/[^"']+\.js)"_q
		).globalMatch(html);
		while (match.hasNext()) {
			scripts.push_back(match.next().captured());
		}
		if (scripts.isEmpty()) {
			done({});
			return;
		}
		std::reverse(scripts.begin(), scripts.end());
		const auto next = std::make_shared<Fn<void(int)>>();
		*next = [=](int index) {
			if (index >= scripts.size()) {
				done({});
				return;
			}
			const auto reply = Get(QUrl(scripts[index]));
			QObject::connect(reply, &QNetworkReply::finished, [=] {
				const auto text = QString::fromUtf8(reply->readAll());
				reply->deleteLater();
				const auto found = QRegularExpression(
					u"client_id:\"([A-Za-z0-9]{32})\""_q).match(text);
				if (found.hasMatch()) {
					done(found.captured(1));
				} else {
					(*next)(index + 1);
				}
			});
		};
		(*next)(0);
	});
}

void EnsureClientId(Fn<void(bool)> done, bool force = false) {
	if (!force && !ClientId().isEmpty()) {
		done(true);
		return;
	}
	FetchClientId([=](QString id) {
		if (id.isEmpty()) {
			done(false);
			return;
		}
		ClientId() = id;
		QDir().mkpath(CacheDir());
		auto file = QFile(ClientIdFile());
		if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			file.write(id.toUtf8());
		}
		done(true);
	});
}

// GET с автоматическим обновлением client_id при 401/403 (один повтор).
void ApiGet(
		const QString &base,
		QUrlQuery query,
		Fn<void(QByteArray, QString)> done,
		bool retried = false) {
	EnsureClientId([=](bool ok) {
		if (!ok) {
			done({}, QString::fromUtf8(Mokagram::Lang::Tr("не удалось получить client_id SoundCloud")));
			return;
		}
		auto copy = query;
		copy.addQueryItem(u"client_id"_q, ClientId());
		auto url = QUrl(base);
		url.setQuery(copy);
		const auto reply = Get(url);
		QObject::connect(reply, &QNetworkReply::finished, [=] {
			const auto status = reply->attribute(
				QNetworkRequest::HttpStatusCodeAttribute).toInt();
			const auto data = reply->readAll();
			const auto error = reply->error();
			reply->deleteLater();
			if ((status == 401 || status == 403) && !retried) {
				EnsureClientId([=](bool refreshed) {
					if (!refreshed) {
						done({}, QString::fromUtf8(Mokagram::Lang::Tr("client_id SoundCloud устарел")));
						return;
					}
					ApiGet(base, query, done, true);
				}, true);
				return;
			}
			if (error != QNetworkReply::NoError) {
				done({}, QString::fromUtf8(Mokagram::Lang::Tr("сеть: %1 (код %2)")).arg(
					reply->errorString()).arg(status));
				return;
			}
			done(data, QString());
		});
	}, false);
}

[[nodiscard]] Track ParseTrack(const QJsonObject &object) {
	auto result = Track();
	result.id = QString::number(object.value("id").toVariant().toLongLong());
	result.title = object.value("title").toString();
	const auto user = object.value("user").toObject();
	result.artist = user.value("username").toString();
	result.artworkUrl = object.value("artwork_url").toString();
	if (result.artworkUrl.isEmpty()) {
		result.artworkUrl = user.value("avatar_url").toString();
	}
	result.permalink = object.value("permalink_url").toString();
	result.durationMs = object.value("duration").toInt();
	result.authorization = object.value("track_authorization").toString();
	result.artistId = QString::number(user.value("id").toVariant().toLongLong());
	result.genre = object.value("genre").toString().trimmed().toLower();
	result.playbackCount = object.value("playback_count").toVariant().toLongLong();
	result.likesCount = object.value("likes_count").toVariant().toLongLong();
	result.createdAt = QDateTime::fromString(
		object.value("created_at").toString(),
		Qt::ISODate).toSecsSinceEpoch();
	{
		// tag_list: слова через пробел, составные теги в кавычках.
		const auto raw = object.value("tag_list").toString();
		auto match = QRegularExpression(u"\"([^\"]+)\"|(\\S+)"_q).globalMatch(raw);
		while (match.hasNext()) {
			const auto m = match.next();
			const auto tag = (m.captured(1).isEmpty() ? m.captured(2) : m.captured(1)).toLower();
			if (!tag.isEmpty() && result.tags.size() < 12) {
				result.tags.push_back(tag);
			}
		}
	}
	auto progressive = QString();
	auto hlsMp3 = QString();
	auto hlsAac = QString();
	for (const auto &value : object.value("media").toObject()
			.value("transcodings").toArray()) {
		const auto item = value.toObject();
		const auto format = item.value("format").toObject();
		const auto protocol = format.value("protocol").toString();
		const auto mime = format.value("mime_type").toString();
		const auto url = item.value("url").toString();
		if (item.value("snipped").toBool()) {
			continue; // Превью-обрезка.
		}
		if (protocol == u"progressive"_q && mime.contains(u"mpeg"_q)) {
			progressive = url;
		} else if (protocol == u"hls"_q && mime.contains(u"mpeg"_q)
			&& !mime.contains(u"mpegurl"_q)) {
			hlsMp3 = url;
		} else if (protocol == u"hls"_q && mime.contains(u"mp4"_q)) {
			hlsAac = url;
		}
	}
	if (!progressive.isEmpty()) {
		result.transcodingUrl = progressive;
		result.extension = u"mp3"_q;
	} else if (!hlsAac.isEmpty()) {
		result.transcodingUrl = hlsAac;
		result.extension = u"m4a"_q;
		result.hls = true;
	} else if (!hlsMp3.isEmpty()) {
		result.transcodingUrl = hlsMp3;
		result.extension = u"mp3"_q;
		result.hls = true;
	}
	return result;
}

[[nodiscard]] QString CachePath(const Track &track) {
	return CacheDir() + '/' + track.id + '.' + track.extension;
}

void FinishFile(
		const QString &path,
		const QByteArray &bytes,
		Fn<void(const QString &, const QString &)> done) {
	QDir().mkpath(CacheDir());
	auto file = QFile(path + u".part"_q);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
		|| file.write(bytes) != bytes.size()) {
		done({}, QString::fromUtf8(Mokagram::Lang::Tr("не удалось записать файл")));
		return;
	}
	file.close();
	QFile::remove(path);
	file.rename(path);
	done(path, QString());
}

// Скачивает HLS: init-сегмент (EXT-X-MAP) и сегменты по порядку, склеивает в один файл.
void DownloadHls(
		const QString &playlistUrl,
		const QString &path,
		Fn<void(const QString &, const QString &)> done) {
	const auto reply = Get(QUrl(playlistUrl));
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		const auto text = QString::fromUtf8(reply->readAll());
		const auto base = reply->url();
		const auto failed = (reply->error() != QNetworkReply::NoError);
		reply->deleteLater();
		if (failed) {
			done({}, QString::fromUtf8(Mokagram::Lang::Tr("не загрузился плейлист")));
			return;
		}
		auto parts = QStringList();
		for (const auto &raw : text.split('\n')) {
			const auto line = raw.trimmed();
			if (line.startsWith(u"#EXT-X-MAP:"_q)) {
				const auto found = QRegularExpression(
					u"URI=\"([^\"]+)\""_q).match(line);
				if (found.hasMatch()) {
					parts.push_back(base.resolved(
						QUrl(found.captured(1))).toString());
				}
			} else if (!line.isEmpty() && !line.startsWith('#')) {
				parts.push_back(base.resolved(QUrl(line)).toString());
			}
		}
		if (parts.isEmpty()) {
			done({}, QString::fromUtf8(Mokagram::Lang::Tr("пустой плейлист")));
			return;
		}
		const auto buffer = std::make_shared<QByteArray>();
		const auto next = std::make_shared<Fn<void(int)>>();
		*next = [=](int index) {
			if (index >= parts.size()) {
				FinishFile(path, *buffer, done);
				return;
			}
			const auto segment = Get(QUrl(parts[index]));
			QObject::connect(segment, &QNetworkReply::finished, [=] {
				const auto ok = (segment->error() == QNetworkReply::NoError);
				if (ok) {
					buffer->append(segment->readAll());
				}
				segment->deleteLater();
				if (!ok) {
					done({}, QString::fromUtf8(Mokagram::Lang::Tr("сбой загрузки сегмента")));
					return;
				}
				(*next)(index + 1);
			});
		};
		(*next)(0);
	});
}

} // namespace

void LoadImage(const QString &url, Fn<void(QImage)> done) {
	static auto cache = std::map<QString, QImage>();
	const auto i = cache.find(url);
	if (i != cache.end()) {
		done(i->second);
		return;
	}
	if (url.isEmpty()) {
		done(QImage());
		return;
	}
	const auto reply = Get(QUrl(url));
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		auto image = QImage::fromData(reply->readAll());
		reply->deleteLater();
		if (cache.size() > 300) {
			cache.clear();
		}
		cache[url] = image;
		done(image);
	});
}

namespace {
std::map<uint64, QImage> &Covers() {
	static auto result = std::map<uint64, QImage>();
	return result;
}
} // namespace

QImage CoverForDocument(uint64 documentId) {
	const auto &covers = Covers();
	const auto i = covers.find(documentId);
	return (i != covers.end()) ? i->second : QImage();
}

namespace {
rpl::event_stream<> &CoversStream() {
	static auto result = rpl::event_stream<>();
	return result;
}
} // namespace

void SetCover(uint64 documentId, QImage image) {
	Covers()[documentId] = std::move(image);
	CoversStream().fire({});
}

rpl::producer<> CoversChanged() {
	return CoversStream().events();
}

uint64 DocumentIdForTrack(const QString &trackId) {
	return 0x4D4F4B0000000000ULL + trackId.toULongLong();
}

namespace {

void CollectionRequest(
		const QString &path,
		QUrlQuery query,
		Fn<void(std::vector<Track>)> done) {
	ApiGet(
		QString::fromLatin1(kApi) + path,
		std::move(query),
		[=](QByteArray data, QString error) {
			auto result = std::vector<Track>();
			if (error.isEmpty()) {
				const auto root = QJsonDocument::fromJson(data).object();
				for (const auto &value : root.value("collection").toArray()) {
					auto track = ParseTrack(value.toObject());
					if (!track.transcodingUrl.isEmpty() && !track.id.isEmpty()) {
						result.push_back(std::move(track));
					}
				}
			}
			done(std::move(result));
		});
}

} // namespace

void Resolve(
		const Track &track,
		Fn<void(Track, const QString &error)> done) {
	ApiGet(
		QString::fromLatin1(kApi) + u"/tracks/"_q + track.id,
		QUrlQuery(),
		[=](QByteArray data, QString error) {
			if (!error.isEmpty()) {
				done(track, error);
				return;
			}
			auto fresh = ParseTrack(QJsonDocument::fromJson(data).object());
			if (fresh.transcodingUrl.isEmpty()) {
				done(track, QString::fromUtf8(Mokagram::Lang::Tr("трек недоступен (закрыт или удалён)")));
				return;
			}
			done(std::move(fresh), QString());
		});
}

void Related(
		const QString &trackId,
		int limit,
		Fn<void(std::vector<Track>)> done) {
	auto q = QUrlQuery();
	q.addQueryItem(u"limit"_q, QString::number(limit));
	CollectionRequest(u"/tracks/"_q + trackId + u"/related"_q, q, std::move(done));
}

void UserTracks(
		const QString &userId,
		int limit,
		Fn<void(std::vector<Track>)> done) {
	auto q = QUrlQuery();
	q.addQueryItem(u"limit"_q, QString::number(limit));
	CollectionRequest(u"/users/"_q + userId + u"/tracks"_q, q, std::move(done));
}

void SearchLimited(
		const QString &query,
		int limit,
		Fn<void(std::vector<Track>)> done) {
	auto q = QUrlQuery();
	q.addQueryItem(u"q"_q, query);
	q.addQueryItem(u"limit"_q, QString::number(limit));
	CollectionRequest(u"/search/tracks"_q, q, std::move(done));
}

namespace {

[[nodiscard]] Album ParseAlbum(const QJsonObject &object) {
	auto result = Album();
	result.id = QString::number(object.value("id").toVariant().toLongLong());
	result.title = object.value("title").toString();
	const auto user = object.value("user").toObject();
	result.artist = user.value("username").toString();
	result.artworkUrl = object.value("artwork_url").toString();
	if (result.artworkUrl.isEmpty()) {
		const auto tracks = object.value("tracks").toArray();
		if (!tracks.isEmpty()) {
			result.artworkUrl = tracks.first().toObject()
				.value("artwork_url").toString();
		}
	}
	if (result.artworkUrl.isEmpty()) {
		result.artworkUrl = user.value("avatar_url").toString();
	}
	result.trackCount = object.value("track_count").toInt();
	return result;
}

void AlbumsRequest(
		const QString &path,
		QUrlQuery query,
		Fn<void(std::vector<Album>, QString)> done) {
	ApiGet(
		QString::fromLatin1(kApi) + path,
		std::move(query),
		[=](QByteArray data, QString error) {
			auto result = std::vector<Album>();
			if (error.isEmpty()) {
				const auto root = QJsonDocument::fromJson(data).object();
				for (const auto &value : root.value("collection").toArray()) {
					auto album = ParseAlbum(value.toObject());
					if (album.id != u"0"_q && album.trackCount > 0) {
						result.push_back(std::move(album));
					}
				}
			}
			done(std::move(result), error);
		});
}

// Подгружает треки по id пачками по 50 и дописывает в found.
void HydrateTracks(
		std::shared_ptr<std::map<QString, Track>> found,
		QStringList missing,
		Fn<void()> done) {
	if (missing.isEmpty()) {
		done();
		return;
	}
	const auto chunk = missing.mid(0, 50);
	missing = missing.mid(chunk.size());
	auto q = QUrlQuery();
	q.addQueryItem(u"ids"_q, chunk.join(','));
	ApiGet(
		QString::fromLatin1(kApi) + u"/tracks"_q,
		q,
		[=](QByteArray data, QString error) {
			for (const auto &value : QJsonDocument::fromJson(data).array()) {
				auto track = ParseTrack(value.toObject());
				if (!track.transcodingUrl.isEmpty()) {
					(*found)[track.id] = std::move(track);
				}
			}
			HydrateTracks(found, missing, done);
		});
}

} // namespace

void SearchAlbums(
		const QString &query,
		int limit,
		Fn<void(std::vector<Album>, QString error)> done) {
	auto q = QUrlQuery();
	q.addQueryItem(u"q"_q, query);
	q.addQueryItem(u"limit"_q, QString::number(limit));
	AlbumsRequest(u"/search/albums"_q, q, std::move(done));
}

void TrackAlbums(
		const QString &trackId,
		Fn<void(std::vector<Album>)> done) {
	auto q = QUrlQuery();
	q.addQueryItem(u"limit"_q, u"10"_q);
	AlbumsRequest(
		u"/tracks/"_q + trackId + u"/albums"_q,
		q,
		[=](std::vector<Album> albums, QString) { done(std::move(albums)); });
}

void AlbumTracks(
		const QString &albumId,
		Fn<void(std::vector<Track>, QString error)> done) {
	ApiGet(
		QString::fromLatin1(kApi) + u"/playlists/"_q + albumId,
		QUrlQuery(),
		[=](QByteArray data, QString error) {
			if (!error.isEmpty()) {
				done({}, error);
				return;
			}
			const auto root = QJsonDocument::fromJson(data).object();
			auto order = QStringList();
			auto missing = QStringList();
			const auto found = std::make_shared<std::map<QString, Track>>();
			for (const auto &value : root.value("tracks").toArray()) {
				const auto object = value.toObject();
				const auto id = QString::number(
					object.value("id").toVariant().toLongLong());
				order.push_back(id);
				auto track = ParseTrack(object);
				if (track.transcodingUrl.isEmpty()) {
					// В ответе полные данные только у первых треков, остальные приходят одним id.
					missing.push_back(id);
				} else {
					(*found)[id] = std::move(track);
				}
			}
			HydrateTracks(found, missing, [=] {
				auto result = std::vector<Track>();
				for (const auto &id : order) {
					if (const auto i = found->find(id); i != found->end()) {
						result.push_back(i->second);
					}
				}
				done(std::move(result), QString());
			});
		});
}

QString CacheDir() {
	return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
		+ u"/mokagram_music"_q;
}

void Search(
		const QString &query,
		int offset,
		Fn<void(SearchResult)> done) {
	auto q = QUrlQuery();
	q.addQueryItem(u"q"_q, query);
	q.addQueryItem(u"limit"_q, QString::number(kPageSize));
	q.addQueryItem(u"offset"_q, QString::number(offset));
	ApiGet(
		QString::fromLatin1(kApi) + u"/search/tracks"_q,
		q,
		[=](QByteArray data, QString error) {
			auto result = SearchResult();
			if (!error.isEmpty()) {
				result.error = error;
				done(std::move(result));
				return;
			}
			const auto root = QJsonDocument::fromJson(data).object();
			for (const auto &value : root.value("collection").toArray()) {
				auto track = ParseTrack(value.toObject());
				if (!track.transcodingUrl.isEmpty()) {
					result.tracks.push_back(std::move(track));
				}
			}
			if (!root.value("next_href").toString().isEmpty()) {
				result.nextOffset = offset + kPageSize;
			}
			done(std::move(result));
		});
}

void Download(
		const Track &track,
		Fn<void(const QString &path, const QString &error)> done) {
	const auto path = CachePath(track);
	if (QFile::exists(path)) {
		done(path, QString());
		return;
	}
	auto q = QUrlQuery();
	if (!track.authorization.isEmpty()) {
		q.addQueryItem(u"track_authorization"_q, track.authorization);
	}
	ApiGet(track.transcodingUrl, q, [=](QByteArray data, QString error) {
		if (!error.isEmpty()) {
			done({}, error);
			return;
		}
		const auto url = QJsonDocument::fromJson(data).object()
			.value("url").toString();
		if (url.isEmpty()) {
			done({}, QString::fromUtf8(Mokagram::Lang::Tr("нет ссылки на поток")));
			return;
		}
		if (track.hls) {
			DownloadHls(url, path, done);
			return;
		}
		const auto reply = Get(QUrl(url));
		QObject::connect(reply, &QNetworkReply::finished, [=] {
			const auto bytes = reply->readAll();
			const auto failed = (reply->error() != QNetworkReply::NoError);
			reply->deleteLater();
			if (failed) {
				done({}, QString::fromUtf8(Mokagram::Lang::Tr("сбой загрузки трека")));
				return;
			}
			FinishFile(path, bytes, done);
		});
	});
}

} // namespace Mokagram::Music
