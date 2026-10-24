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

[[nodiscard]] Lyrics ParsePlain(const QString &text) {
	auto result = Lyrics();
	for (const auto &raw : text.split('\n')) {
		result.lines.push_back({ 0, raw.trimmed() });
	}
	result.found = !result.lines.empty();
	return result;
}

[[nodiscard]] Lyrics Pick(
		const QJsonArray &list,
		qint64 durationMs) {
	auto best = QJsonObject();
	auto bestScore = -1e9;
	auto bestSynced = false;
	for (const auto &value : list) {
		const auto item = value.toObject();
		const auto synced = item.value("syncedLyrics").toString();
		const auto plain = item.value("plainLyrics").toString();
		if (synced.isEmpty() && plain.isEmpty()) {
			continue;
		}
		const auto diff = durationMs > 0
			? std::abs(item.value("duration").toDouble() * 1000. - durationMs)
			: 0.;
		// Метки времени годятся только у версии той же длины.
		const auto useSynced = !synced.isEmpty() && diff <= 12000.;
		const auto score = (useSynced ? 100. : 10.) - diff / 1000.;
		if (score > bestScore) {
			bestScore = score;
			best = item;
			bestSynced = useSynced;
		}
	}
	if (bestScore < -1e8) {
		return {};
	}
	if (bestSynced) {
		auto result = ParseLrc(best.value("syncedLyrics").toString());
		if (result.found) {
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
		Fn<void(Lyrics)> done) {
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
			result = Pick(doc.array(), durationMs);
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
				best = qint64(song.value("id").toDouble());
			}
		}
		if (!best || (durationMs > 0 && bestDiff > 12000.)) {
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
				done(std::move(result));
			});
	});
}

} // namespace

QString CleanTitle(const QString &title, const QString &artist) {
	auto result = title;
	static const auto tail = QRegularExpression(
		uR"(\s*[\(\[][^\)\]]*(official|video|audio|lyrics|clip|прем|клип|visualizer|hd|4k)[^\)\]]*[\)\]])"_q,
		QRegularExpression::CaseInsensitiveOption);
	result.remove(tail);
	const auto prefix = artist.trimmed() + u" - "_q;
	if (!artist.isEmpty() && result.startsWith(prefix, Qt::CaseInsensitive)) {
		result = result.mid(prefix.size());
	}
	return result.trimmed();
}

namespace {

// Название без любых скобок: пометки вроде "(slowed + reverb)" ищут хуже всего.
[[nodiscard]] QString WithoutBrackets(const QString &text) {
	static const auto brackets = QRegularExpression(uR"(\s*[\(\[][^\)\]]*[\)\]])"_q);
	auto result = text;
	result.remove(brackets);
	result = result.trimmed();
	return result.isEmpty() ? text : result;
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
	const auto clean = CleanTitle(title, artist);
	// "Исполнитель - Название" внутри заголовка: у загрузчика чужое имя.
	auto trackName = clean;
	auto artistName = artist;
	const auto dash = title.indexOf(u" - "_q);
	if (dash > 0) {
		artistName = title.left(dash).trimmed();
		trackName = CleanTitle(title.mid(dash + 3), artistName);
	}
	trackName = WithoutBrackets(trackName);
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
				RequestNetease(artistName + u" "_q + trackName, trackName, durationMs, finish);
				return;
			}
			auto third = QUrlQuery();
			third.addQueryItem(u"q"_q, clean);
			Request(third, durationMs, [=](Lyrics result) {
				if (result.found) {
					finish(std::move(result));
					return;
				}
				RequestNetease(artistName + u" "_q + trackName, trackName, durationMs, finish);
			});
		});
	});
}

} // namespace Mokagram::Music
