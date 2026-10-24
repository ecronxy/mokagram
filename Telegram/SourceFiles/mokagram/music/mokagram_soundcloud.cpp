#include "mokagram/music/mokagram_soundcloud.h"

#include "core/application.h"

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
			done({}, u"не удалось получить client_id SoundCloud"_q);
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
						done({}, u"client_id SoundCloud устарел"_q);
						return;
					}
					ApiGet(base, query, done, true);
				}, true);
				return;
			}
			if (error != QNetworkReply::NoError) {
				done({}, u"сеть: %1 (код %2)"_q.arg(
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
		done({}, u"не удалось записать файл"_q);
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
			done({}, u"не загрузился плейлист"_q);
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
			done({}, u"пустой плейлист"_q);
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
					done({}, u"сбой загрузки сегмента"_q);
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
			done({}, u"нет ссылки на поток"_q);
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
				done({}, u"сбой загрузки трека"_q);
				return;
			}
			FinishFile(path, bytes, done);
		});
	});
}

} // namespace Mokagram::Music
