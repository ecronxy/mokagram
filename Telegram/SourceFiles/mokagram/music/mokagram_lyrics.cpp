// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 80e3dd12
#include "mokagram/music/mokagram_lyrics.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include "logs.h"

#include <crl/crl.h>
#include <QtGui/QImage>

#include <map>

namespace Mokagram::Music {
namespace {

QNetworkAccessManager &Net() {
	static auto manager = new QNetworkAccessManager();
	return *manager;
}

std::map<QString, Lyrics> &Cache() {
	static auto result = std::map<QString, Lyrics>();
	return result;
}

[[nodiscard]] Lyrics ParseLrc(const QString &text) {
	auto result = Lyrics();
	static const auto stamp = QRegularExpression(
		uR"(\[(\d+):(\d+)(?:[.:](\d+))?\])"_q);
	for (const auto &raw : text.split('\n')) {
		auto times = std::vector<qint64>();
		auto last = 0;
		auto it = stamp.globalMatch(raw);
		while (it.hasNext()) {
			const auto m = it.next();
			auto fraction = m.captured(3);
			const auto ms = fraction.isEmpty()
				? 0
				: (fraction.leftJustified(3, '0').left(3).toInt());
			times.push_back(
				m.captured(1).toLongLong() * 60000
				+ m.captured(2).toLongLong() * 1000
				+ ms);
			last = m.capturedEnd();
		}
		if (times.empty()) {
			continue;
		}
		const auto line = raw.mid(last).trimmed();
		for (const auto time : times) {
			result.lines.push_back({ time, line });
		}
	}
	std::sort(result.lines.begin(), result.lines.end(), [](const auto &a, const auto &b) {
		return a.timeMs < b.timeMs;
	});
	result.synced = !result.lines.empty();
	result.found = result.synced;
	return result;
}

// Версии speed up и slowed идут быстрее или медленнее оригинала: метки времени
// растягиваем во столько же раз, во сколько отличается длина трека.
void ScaleTimes(Lyrics &lyrics, double ratio) {
	if (std::abs(ratio - 1.) < 0.01) {
		return;
	}
	for (auto &line : lyrics.lines) {
		line.timeMs = qint64(line.timeMs * ratio);
	}
}

[[nodiscard]] Lyrics ParsePlain(const QString &text) {
	auto result = Lyrics();
	for (const auto &raw : text.split('\n')) {
		result.lines.push_back({ 0, raw.trimmed() });
	}
	result.found = !result.lines.empty();
	return result;
}

// Название без любых скобок: пометки вроде "(slowed + reverb)" ищут хуже всего.
[[nodiscard]] QString WithoutBrackets(const QString &text) {
	static const auto brackets = QRegularExpression(uR"(\s*[\(\[][^\)\]]*[\)\]])"_q);
	auto result = text;
	result.remove(brackets);
	result = result.trimmed();
	return result.isEmpty() ? text : result;
}

[[nodiscard]] Lyrics Pick(
		const QJsonArray &list,
		qint64 durationMs,
		const QString &matchingTitle = {}) {
	auto best = QJsonObject();
	auto bestScore = -1e9;
	auto bestSynced = false;
	auto bestRatio = 1.;
	for (const auto &value : list) {
		const auto item = value.toObject();
		const auto synced = item.value("syncedLyrics").toString();
		const auto plain = item.value("plainLyrics").toString();
		if (synced.isEmpty() && plain.isEmpty()) {
			continue;
		}
		const auto source = item.value("duration").toDouble() * 1000.;
		const auto ratio = (durationMs > 0 && source > 0.)
			? durationMs / source
			: 1.;
		const auto diff = std::abs(source - durationMs);
		if (!matchingTitle.isEmpty()) {
			const auto title = WithoutBrackets(CleanTitle(
				item.value("trackName").toString(),
				item.value("artistName").toString()));
			if (title.compare(matchingTitle, Qt::CaseInsensitive) != 0
				|| durationMs <= 0
				|| source <= 0.
				|| diff > 3000.) {
				continue;
			}
		}
		auto useSynced = false;
		auto score = 10. - std::min(diff / 1000., 9.);
		if (!synced.isEmpty() && (durationMs <= 0 || diff <= 12000.)) {
			useSynced = true;
			score = 100. - diff / 1000.;
		} else if (!synced.isEmpty() && ratio > 0.5 && ratio < 2.) {
			// Другая скорость: метки подгоним по отношению длин.
			useSynced = true;
			score = 60. - std::abs(std::log(ratio)) * 20.;
		}
		if (score > bestScore) {
			bestScore = score;
			best = item;
			bestSynced = useSynced;
			bestRatio = ratio;
		}
	}
	if (bestScore < -1e8) {
		return {};
	}
	if (bestSynced) {
		auto result = ParseLrc(best.value("syncedLyrics").toString());
		if (result.found) {
			ScaleTimes(result, bestRatio);
			return result;
		}
	}
	const auto plain = best.value("plainLyrics").toString();
	if (!plain.isEmpty()) {
		return ParsePlain(plain);
	}
	return ParsePlain(best.value("syncedLyrics").toString().remove(
		QRegularExpression(uR"(\[[^\]]*\])"_q)));
}

void Request(
		QUrlQuery query,
		qint64 durationMs,
		Fn<void(Lyrics)> done,
		QString matchingTitle = {}) {
	// '+' и скобки кодируем сами: QUrlQuery оставляет '+' как есть, и сервис читает его пробелом.
	auto parts = QStringList();
	for (const auto &[key, value] : query.queryItems()) {
		parts.push_back(key + u"="_q + QString::fromLatin1(QUrl::toPercentEncoding(value)));
	}
	auto url = QUrl(u"https://lrclib.net/api/search?"_q + parts.join('&'));
	auto request = QNetworkRequest(url);
	request.setRawHeader("User-Agent", "mokaGram (https://github.com/ecronx/mokagram)");
	const auto reply = Net().get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		auto result = Lyrics();
		LOG(("[lyrics] %1: error %2").arg(url.toString()).arg(reply->errorString()));
		if (reply->error() == QNetworkReply::NoError) {
			const auto doc = QJsonDocument::fromJson(reply->readAll());
			result = Pick(doc.array(), durationMs, matchingTitle);
		}
		done(std::move(result));
	});
}

void GetJson(
		const QString &address,
		Fn<void(QJsonDocument)> done) {
	auto request = QNetworkRequest(QUrl(address));
	request.setRawHeader("User-Agent", "Mozilla/5.0");
	request.setRawHeader("Referer", "https://music.163.com/");
	const auto reply = Net().get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		done(reply->error() == QNetworkReply::NoError
			? QJsonDocument::fromJson(reply->readAll())
			: QJsonDocument());
	});
}

// Запасной источник: NetEase отдаёт строки с метками времени для многих треков.
void RequestNetease(
		const QString &query,
		const QString &trackName,
		qint64 durationMs,
		Fn<void(Lyrics)> done) {
	const auto search = u"https://music.163.com/api/search/get?type=1&limit=8&s="_q
		+ QString::fromLatin1(QUrl::toPercentEncoding(query));
	GetJson(search, [=](QJsonDocument doc) {
		const auto songs = doc.object().value("result").toObject().value("songs").toArray();
		auto best = qint64(0);
		auto bestSource = 0.;
		auto bestDiff = 1e18;
		for (const auto &value : songs) {
			const auto song = value.toObject();
			const auto name = song.value("name").toString().toLower();
			const auto wanted = trackName.toLower();
			// Берём только песню с тем же названием, иначе слова будут чужие.
			if (wanted.size() >= 2
				&& !name.contains(wanted)
				&& !wanted.contains(name)) {
				continue;
			}
			const auto diff = durationMs > 0
				? std::abs(song.value("duration").toDouble() - durationMs)
				: 0.;
			if (diff < bestDiff) {
				bestDiff = diff;
				bestSource = song.value("duration").toDouble();
				best = qint64(song.value("id").toDouble());
			}
		}
		if (!best) {
			done({});
			return;
		}
		const auto ratio = (durationMs > 0 && bestSource > 0.)
			? durationMs / bestSource
			: 1.;
		if (durationMs > 0 && bestDiff > 12000. && (ratio <= 0.5 || ratio >= 2.)) {
			done({});
			return;
		}
		GetJson(
			u"https://music.163.com/api/song/lyric?lv=1&kv=1&tv=-1&id="_q + QString::number(best),
			[=](QJsonDocument lyric) {
				const auto text = lyric.object().value("lrc").toObject().value("lyric").toString();
				auto result = text.isEmpty() ? Lyrics() : ParseLrc(text);
				// Служебные строки с авторами (作词, 作曲...) не показываем.
				static const auto credits = QRegularExpression(
					uR"(^\s*(作词|作曲|编曲|制作|作詞|编曲|词|曲|Lyrics|Music|Composer|Producer|Arranger)\s*[:：])"_q,
					QRegularExpression::CaseInsensitiveOption);
				auto &lines = result.lines;
				lines.erase(
					std::remove_if(lines.begin(), lines.end(), [&](const LyricLine &l) {
						return credits.match(l.text).hasMatch();
					}),
					lines.end());
				result.found = !lines.empty();
				if (result.found) {
					ScaleTimes(result, ratio);
				}
				done(std::move(result));
			});
	});
}

void RequestFallback(
		const QString &trackName,
		const QString &artistName,
		qint64 durationMs,
		Fn<void(Lyrics)> done) {
	const auto netease = [=] {
		RequestNetease(
			artistName + u" "_q + trackName,
			trackName,
			durationMs,
			done);
	};
	if (durationMs <= 0) {
		netease();
		return;
	}
	auto query = QUrlQuery();
	query.addQueryItem(u"track_name"_q, trackName);
	Request(query, durationMs, [=](Lyrics result) {
		if (result.found) {
			done(std::move(result));
		} else {
			netease();
		}
	}, trackName);
}

} // namespace

QString CleanTitle(const QString &title, const QString &artist) {
	auto result = title.trimmed();
	static const auto extension = QRegularExpression(
		uR"(\.(mp3|m4a|flac|ogg|oga|opus|wav|aac|aiff|aif|wma|alac)$)"_q,
		QRegularExpression::CaseInsensitiveOption);
	result.remove(extension);
	result.replace(QChar('_'), QChar(' '));
	static const auto tail = QRegularExpression(
		uR"(\s*[\(\[][^\)\]]*(official|video|audio|lyrics|clip|прем|клип|visualizer|hd|4k)[^\)\]]*[\)\]])"_q,
		QRegularExpression::CaseInsensitiveOption);
	result.remove(tail);
	const auto prefix = artist.trimmed() + u" - "_q;
	if (!artist.isEmpty() && result.startsWith(prefix, Qt::CaseInsensitive)) {
		result = result.mid(prefix.size());
	}
	return result.simplified();
}

namespace {

struct SplitResult {
	QString clean;
	QString trackName;
	QString artistName;
};

[[nodiscard]] SplitResult SplitName(const QString &title, const QString &artist) {
	auto result = SplitResult{ .clean = CleanTitle(title, QString()) };
	// "Исполнитель - Название" внутри заголовка: у загрузчика чужое имя.
	result.trackName = result.clean;
	result.artistName = artist.trimmed();
	static const auto separator = QRegularExpression(uR"(\s+[-–—]\s+)"_q);
	const auto dash = separator.match(result.clean);
	if (dash.hasMatch() && dash.capturedStart() > 0) {
		result.artistName = result.clean.left(dash.capturedStart()).trimmed();
		result.trackName = CleanTitle(
			result.clean.mid(dash.capturedEnd()),
			result.artistName);
	}
	result.trackName = WithoutBrackets(result.trackName);
	return result;
}

// Тот ли это трек: название и исполнитель совпадают, длительность близка.
[[nodiscard]] bool SameTrack(
		const SplitResult &wanted,
		qint64 durationMs,
		const QString &title,
		const QString &artist,
		qint64 sourceMs) {
	const auto name = WithoutBrackets(CleanTitle(title, artist));
	if (name.compare(wanted.trackName, Qt::CaseInsensitive) != 0) {
		return false;
	}
	const auto performer = artist.toLower();
	const auto expected = wanted.artistName.toLower();
	if (!expected.isEmpty()
		&& !performer.contains(expected)
		&& !expected.contains(performer)) {
		return false;
	}
	return durationMs <= 0
		|| sourceMs <= 0
		|| std::abs(sourceMs - durationMs) <= 4000;
}

std::map<QString, QString> &ArtworkUrls() {
	static auto result = std::map<QString, QString>();
	return result;
}

void LoadArtwork(const QString &url, Fn<void(QImage)> done) {
	if (url.isEmpty()) {
		done(QImage());
		return;
	}
	auto request = QNetworkRequest(QUrl(url));
	request.setRawHeader("User-Agent", "Mozilla/5.0");
	const auto reply = Net().get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			done(QImage());
			return;
		}
		// Большой JPEG декодируется заметное время: не на главном потоке.
		crl::async([=, bytes = reply->readAll()] {
			auto image = QImage::fromData(bytes);
			crl::on_main([=, image = std::move(image)]() mutable {
				done(std::move(image));
			});
		});
	});
}

void SearchDeezer(
		const SplitResult &wanted,
		qint64 durationMs,
		Fn<void(QString)> done) {
	const auto query = u"artist:\"%1\" track:\"%2\""_q
		.arg(wanted.artistName, wanted.trackName);
	auto request = QNetworkRequest(QUrl(u"https://api.deezer.com/search?limit=10&q="_q
		+ QString::fromLatin1(QUrl::toPercentEncoding(query))));
	request.setRawHeader("User-Agent", "Mozilla/5.0");
	const auto reply = Net().get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto doc = QJsonDocument::fromJson(reply->readAll());
		for (const auto &value : doc.object().value("data").toArray()) {
			const auto item = value.toObject();
			const auto cover = item.value("album").toObject()
				.value("cover_xl").toString();
			if (!cover.isEmpty() && SameTrack(
					wanted,
					durationMs,
					item.value("title").toString(),
					item.value("artist").toObject().value("name").toString(),
					qint64(item.value("duration").toDouble() * 1000.))) {
				done(cover);
				return;
			}
		}
		done(QString());
	});
}

void SearchArtwork(
		const SplitResult &wanted,
		qint64 durationMs,
		Fn<void(QString)> done) {
	auto request = QNetworkRequest(QUrl(
		u"https://itunes.apple.com/search?media=music&entity=song&limit=15&term="_q
		+ QString::fromLatin1(QUrl::toPercentEncoding(
			wanted.artistName + u" "_q + wanted.trackName))));
	request.setRawHeader("User-Agent", "Mozilla/5.0");
	const auto reply = Net().get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto doc = QJsonDocument::fromJson(reply->readAll());
		for (const auto &value : doc.object().value("results").toArray()) {
			const auto item = value.toObject();
			auto artwork = item.value("artworkUrl100").toString();
			if (!artwork.isEmpty() && SameTrack(
					wanted,
					durationMs,
					item.value("trackName").toString(),
					item.value("artistName").toString(),
					qint64(item.value("trackTimeMillis").toDouble()))) {
				artwork.replace(u"100x100bb"_q, u"1000x1000bb"_q);
				done(artwork);
				return;
			}
		}
		SearchDeezer(wanted, durationMs, done);
	});
}

} // namespace

void FetchLyrics(
		const QString &cacheKey,
		const QString &title,
		const QString &artist,
		qint64 durationMs,
		Fn<void(Lyrics)> done) {
	auto &cache = Cache();
	if (const auto i = cache.find(cacheKey); i != cache.end()) {
		done(i->second);
		return;
	}
	const auto split = SplitName(title, artist);
	const auto clean = split.clean;
	const auto trackName = split.trackName;
	const auto artistName = split.artistName;
	const auto finish = [=](Lyrics result) {
		// Неудачу не запоминаем: сеть могла моргнуть.
		if (result.found) {
			Cache()[cacheKey] = result;
		}
		done(std::move(result));
	};
	auto first = QUrlQuery();
	first.addQueryItem(u"track_name"_q, trackName);
	first.addQueryItem(u"artist_name"_q, artistName);
	Request(first, durationMs, [=](Lyrics result) {
		if (result.found) {
			finish(std::move(result));
			return;
		}
		auto second = QUrlQuery();
		second.addQueryItem(u"q"_q, artistName + u" "_q + trackName);
		Request(second, durationMs, [=](Lyrics result) {
			if (result.found) {
				finish(std::move(result));
				return;
			}
			if (artistName == artist) {
				RequestFallback(trackName, artistName, durationMs, finish);
				return;
			}
			auto third = QUrlQuery();
			third.addQueryItem(u"q"_q, clean);
			Request(third, durationMs, [=](Lyrics result) {
				if (result.found) {
					finish(std::move(result));
					return;
				}
				RequestFallback(trackName, artistName, durationMs, finish);
			});
		});
	});
}

void FetchArtwork(
		const QString &cacheKey,
		const QString &title,
		const QString &artist,
		qint64 durationMs,
		Fn<void(QImage)> done) {
	auto &urls = ArtworkUrls();
	if (const auto i = urls.find(cacheKey); i != urls.end()) {
		LoadArtwork(i->second, done);
		return;
	}
	const auto wanted = SplitName(title, artist);
	if (wanted.trackName.isEmpty() || wanted.artistName.isEmpty()) {
		done(QImage());
		return;
	}
	SearchArtwork(wanted, durationMs, [=](QString url) {
		// Неудачу не запоминаем: сеть могла моргнуть.
		if (!url.isEmpty()) {
			ArtworkUrls()[cacheKey] = url;
		}
		LoadArtwork(url, done);
	});
}

} // namespace Mokagram::Music
