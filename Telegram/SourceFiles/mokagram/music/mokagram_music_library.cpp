#include "mokagram/music/mokagram_music_library.h"

#include "base/random.h"
#include "rpl/event_stream.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStandardPaths>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>

namespace Mokagram::Music {
namespace {

constexpr auto kHistoryLimit = 3000;
constexpr double kHalfLifeDays = 60.;

struct Liked {
	Track track;
	qint64 at = 0;
};

struct Listen {
	QString id;
	QString artistId;
	QString genre;
	QStringList tags;
	int durationMs = 0;
	int listenedMs = 0;
	bool finished = false;
	qint64 at = 0;
};

struct Library {
	std::vector<Liked> liked;      // новые сверху
	std::vector<Listen> history;   // старые сверху
	bool loaded = false;
	rpl::event_stream<> likesChanged;
};

[[nodiscard]] Library &Lib();

[[nodiscard]] QString FilePath() {
	return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
		+ u"/mokagram_music/library.json"_q;
}

[[nodiscard]] QJsonObject ToJson(const Track &t) {
	auto o = QJsonObject();
	o.insert("id", t.id);
	o.insert("title", t.title);
	o.insert("artist", t.artist);
	o.insert("art", t.artworkUrl);
	o.insert("link", t.permalink);
	o.insert("dur", t.durationMs);
	o.insert("aid", t.artistId);
	o.insert("genre", t.genre);
	o.insert("tags", QJsonArray::fromStringList(t.tags));
	o.insert("plays", double(t.playbackCount));
	o.insert("likes", double(t.likesCount));
	o.insert("created", double(t.createdAt));
	return o;
}

[[nodiscard]] Track FromJson(const QJsonObject &o) {
	auto t = Track();
	t.id = o.value("id").toString();
	t.title = o.value("title").toString();
	t.artist = o.value("artist").toString();
	t.artworkUrl = o.value("art").toString();
	t.permalink = o.value("link").toString();
	t.durationMs = o.value("dur").toInt();
	t.artistId = o.value("aid").toString();
	t.genre = o.value("genre").toString();
	for (const auto &tag : o.value("tags").toArray()) {
		t.tags.push_back(tag.toString());
	}
	t.playbackCount = qint64(o.value("plays").toDouble());
	t.likesCount = qint64(o.value("likes").toDouble());
	t.createdAt = qint64(o.value("created").toDouble());
	return t;
}

void Save() {
	auto &lib = Lib();
	auto liked = QJsonArray();
	for (const auto &item : lib.liked) {
		auto o = ToJson(item.track);
		o.insert("at", double(item.at));
		liked.push_back(o);
	}
	auto history = QJsonArray();
	for (const auto &h : lib.history) {
		auto o = QJsonObject();
		o.insert("id", h.id);
		o.insert("aid", h.artistId);
		o.insert("genre", h.genre);
		o.insert("tags", QJsonArray::fromStringList(h.tags));
		o.insert("dur", h.durationMs);
		o.insert("l", h.listenedMs);
		o.insert("fin", h.finished);
		o.insert("at", double(h.at));
		history.push_back(o);
	}
	auto root = QJsonObject();
	root.insert("liked", liked);
	root.insert("history", history);
	QDir().mkpath(QFileInfo(FilePath()).absolutePath());
	auto file = QFile(FilePath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
	}
}

Library &Lib() {
	static auto library = Library();
	if (!library.loaded) {
		library.loaded = true;
		auto file = QFile(FilePath());
		if (file.open(QIODevice::ReadOnly)) {
			const auto root = QJsonDocument::fromJson(file.readAll()).object();
			for (const auto &v : root.value("liked").toArray()) {
				const auto o = v.toObject();
				library.liked.push_back({ FromJson(o), qint64(o.value("at").toDouble()) });
			}
			for (const auto &v : root.value("history").toArray()) {
				const auto o = v.toObject();
				auto h = Listen();
				h.id = o.value("id").toString();
				h.artistId = o.value("aid").toString();
				h.genre = o.value("genre").toString();
				for (const auto &tag : o.value("tags").toArray()) {
					h.tags.push_back(tag.toString());
				}
				h.durationMs = o.value("dur").toInt();
				h.listenedMs = o.value("l").toInt();
				h.finished = o.value("fin").toBool();
				h.at = qint64(o.value("at").toDouble());
				library.history.push_back(std::move(h));
			}
		}
	}
	return library;
}

// ---- профиль вкуса ----

struct Profile {
	std::map<QString, double> track;
	std::map<QString, double> artist;
	std::map<QString, double> genre;
	std::map<QString, double> tag;
	std::map<QString, qint64> lastFinished;  // когда трек доигран в последний раз
};

[[nodiscard]] double Decay(qint64 at, qint64 now) {
	const auto days = std::max<qint64>(now - at, 0) / 86400.;
	return std::pow(0.5, days / kHalfLifeDays);
}

void AddSignal(
		Profile &p,
		const QString &trackId,
		const QString &artistId,
		const QString &genre,
		const QStringList &tags,
		double weight) {
	if (!trackId.isEmpty()) {
		p.track[trackId] += weight;
	}
	if (!artistId.isEmpty()) {
		p.artist[artistId] += weight;
	}
	if (!genre.isEmpty()) {
		p.genre[genre] += weight;
	}
	const auto norm = tags.isEmpty() ? 1. : 1. / std::sqrt(double(tags.size()));
	for (const auto &tag : tags) {
		p.tag[tag] += weight * norm;
	}
}

[[nodiscard]] Profile BuildProfile() {
	auto p = Profile();
	const auto now = QDateTime::currentSecsSinceEpoch();
	for (const auto &item : Lib().liked) {
		const auto &t = item.track;
		AddSignal(p, t.id, t.artistId, t.genre, t.tags, 3. * Decay(item.at, now));
	}
	for (const auto &h : Lib().history) {
		const auto ratio = (h.durationMs > 0)
			? std::min(1., h.listenedMs / double(h.durationMs))
			: 0.;
		auto w = 0.;
		if (h.finished || ratio >= 0.8) {
			w = 1.;
			p.lastFinished[h.id] = std::max(p.lastFinished[h.id], h.at);
		} else if (ratio >= 0.35) {
			w = 0.25;
		} else if (ratio < 0.25) {
			w = -0.8; // пропустил почти сразу
		}
		AddSignal(p, h.id, h.artistId, h.genre, h.tags, w * Decay(h.at, now));
	}
	return p;
}

[[nodiscard]] double Get(const std::map<QString, double> &m, const QString &key) {
	const auto i = m.find(key);
	return (i != m.end()) ? i->second : 0.;
}

[[nodiscard]] std::vector<QString> TopKeys(
		const std::map<QString, double> &m,
		int count,
		double minimum = 0.) {
	auto items = std::vector<std::pair<QString, double>>(m.begin(), m.end());
	std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
		return a.second > b.second;
	});
	auto result = std::vector<QString>();
	for (const auto &[key, value] : items) {
		if (value <= minimum || int(result.size()) >= count) {
			break;
		}
		result.push_back(key);
	}
	return result;
}

[[nodiscard]] double Random01() {
	return base::RandomIndex(10000) / 10000.;
}

struct Candidate {
	Track track;
	double seedHits = 0.;
	bool exploration = false;
};

struct Collector {
	std::map<QString, Candidate> candidates;
	int pending = 0;
	Fn<void()> finish;
};

void Gather(
		const std::shared_ptr<Collector> &collector,
		std::vector<Track> tracks,
		double seedWeight,
		bool exploration) {
	for (auto &track : tracks) {
		auto &c = collector->candidates[track.id];
		if (c.track.id.isEmpty()) {
			c.track = std::move(track);
			c.exploration = exploration;
		}
		c.seedHits += seedWeight;
	}
	if (--collector->pending == 0) {
		collector->finish();
	}
}

} // namespace

bool IsLiked(const QString &trackId) {
	for (const auto &item : Lib().liked) {
		if (item.track.id == trackId) {
			return true;
		}
	}
	return false;
}

void SetLiked(const Track &track, bool liked) {
	auto &lib = Lib();
	const auto i = std::find_if(lib.liked.begin(), lib.liked.end(), [&](const Liked &l) {
		return l.track.id == track.id;
	});
	if (liked && i == lib.liked.end()) {
		auto copy = track;
		copy.transcodingUrl.clear();
		copy.authorization.clear();
		lib.liked.insert(lib.liked.begin(), { std::move(copy), QDateTime::currentSecsSinceEpoch() });
	} else if (!liked && i != lib.liked.end()) {
		lib.liked.erase(i);
	} else {
		return;
	}
	Save();
	lib.likesChanged.fire({});
}

std::vector<Track> LikedTracks() {
	auto result = std::vector<Track>();
	for (const auto &item : Lib().liked) {
		result.push_back(item.track);
	}
	return result;
}

rpl::producer<> LikesChanged() {
	return Lib().likesChanged.events();
}

void RecordListen(const Track &track, int listenedMs, bool finished) {
	if (track.id.isEmpty() || track.durationMs <= 0) {
		return;
	}
	auto &lib = Lib();
	auto h = Listen();
	h.id = track.id;
	h.artistId = track.artistId;
	h.genre = track.genre;
	h.tags = track.tags;
	h.durationMs = track.durationMs;
	h.listenedMs = std::clamp(listenedMs, 0, track.durationMs);
	h.finished = finished;
	h.at = QDateTime::currentSecsSinceEpoch();
	lib.history.push_back(std::move(h));
	if (int(lib.history.size()) > kHistoryLimit) {
		lib.history.erase(
			lib.history.begin(),
			lib.history.begin() + (lib.history.size() - kHistoryLimit));
	}
	Save();
}

bool HasTaste() {
	return !Lib().liked.empty() || !Lib().history.empty();
}

std::set<QString> &ShownRecently() {
	static auto result = std::set<QString>();
	return result;
}

void BuildForYou(
		int count,
		Fn<void(std::vector<Track>, QString)> done) {
	if (!HasTaste()) {
		done({}, QString::fromUtf8("Пока нечего анализировать. Поставьте сердечко нескольким трекам и послушайте музыку: подборка подстроится под вас."));
		return;
	}
	const auto profile = std::make_shared<Profile>(BuildProfile());
	const auto now = QDateTime::currentSecsSinceEpoch();

	// Семена: лучшие треки по вкусу, часть выбирается случайно из следующих.
	auto ranked = std::vector<std::pair<QString, double>>(
		profile->track.begin(),
		profile->track.end());
	std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
		return a.second > b.second;
	});
	auto seeds = std::vector<std::pair<QString, double>>();
	// Каждое обновление берёт другие семена: один сильный всегда, остальные из верхней части.
	if (!ranked.empty() && ranked[0].second > 0.) {
		seeds.push_back(ranked[0]);
	}
	auto pool = std::vector<std::pair<QString, double>>();
	for (auto i = 1; i < std::min<int>(ranked.size(), 30); ++i) {
		if (ranked[i].second > 0.) {
			pool.push_back(ranked[i]);
		}
	}
	for (auto picked = 0; picked != 5 && !pool.empty(); ++picked) {
		const auto at = base::RandomIndex(int(pool.size()));
		seeds.push_back(pool[at]);
		pool.erase(pool.begin() + at);
	}
	const auto topArtists = TopKeys(profile->artist, 3, 0.5);
	const auto topGenres = TopKeys(profile->genre, 2, 0.5);
	const auto topTags = TopKeys(profile->tag, 2, 0.5);

	const auto collector = std::make_shared<Collector>();
	const auto maxSeed = seeds.empty() ? 1. : std::max(seeds.front().second, 1.);
	collector->finish = [=] {
		const auto &lib = Lib();
		auto liked = std::set<QString>();
		for (const auto &l : lib.liked) {
			liked.insert(l.track.id);
		}
		struct Scored {
			Track track;
			double score = 0.;
			bool known = false;
		};
		auto scored = std::vector<Scored>();
		const auto tagSet = std::set<QString>(topTags.begin(), topTags.end());
		for (const auto &[id, c] : collector->candidates) {
			if (liked.contains(id)) {
				continue;
			}
			const auto &t = c.track;
			auto score = 1.6 * std::min(1., c.seedHits / 1.5);
			const auto artistAffinity = Get(profile->artist, t.artistId);
			score += 1.0 * std::tanh(artistAffinity);
			score += 0.7 * std::tanh(Get(profile->genre, t.genre));
			auto tagAffinity = 0.;
			for (const auto &tag : t.tags) {
				tagAffinity += std::tanh(Get(profile->tag, tag));
			}
			score += 0.5 * std::min(1., tagAffinity / 2.);
			score += 0.25 * std::clamp(std::log10(t.playbackCount + 1.) / 7., 0., 1.);
			if (t.playbackCount > 0) {
				score += 0.2 * std::min(1., (t.likesCount / double(t.playbackCount)) * 25.);
			}
			if (t.createdAt > 0 && (now - t.createdAt) < 365 * 86400) {
				score += 0.15;
			}
			if (artistAffinity < -0.8) {
				score -= 2.;
			}
			if (ShownRecently().contains(id)) {
				score -= 1.6; // уже было в прошлой подборке
			}
			const auto last = profile->lastFinished.find(id);
			if (last != profile->lastFinished.end() && (now - last->second) < 7 * 86400) {
				score -= 1.5;
			}
			if (Get(profile->track, id) < -0.5) {
				score -= 2.; // уже пропускал этот трек
			}
			scored.push_back({ t, score, artistAffinity > 0.3 });
		}
		if (scored.empty()) {
			done({}, QString::fromUtf8("Не нашлось подходящих треков. Попробуйте позже."));
			return;
		}
		// Жадный отбор: штрафы за повтор исполнителя и жанра, доля нового ~30%.
		auto result = std::vector<Track>();
		auto artistCount = std::map<QString, int>();
		auto genreCount = std::map<QString, int>();
		auto titles = std::set<QString>();
		auto unknown = 0;
		while (int(result.size()) < count && !scored.empty()) {
			auto best = -1;
			auto bestValue = -1e9;
			for (auto i = 0; i != int(scored.size()); ++i) {
				const auto &c = scored[i];
				auto value = c.score;
				value -= 0.55 * artistCount[c.track.artistId];
				if (!result.empty()
					&& genreCount[c.track.genre] > int(result.size()) / 2 + 1) {
					value -= 0.3;
				}
				if (!c.known && unknown < int(count * 0.3)) {
					value += 0.3;
				}
				if (!result.empty() && result.back().artistId == c.track.artistId) {
					value -= 0.6;
				}
				value += (Random01() - 0.5) * 0.3;
				if (value > bestValue) {
					bestValue = value;
					best = i;
				}
			}
			auto picked = scored[best];
			scored.erase(scored.begin() + best);
			const auto key = picked.track.title.toLower().trimmed();
			if (!titles.insert(key).second) {
				continue;
			}
			++artistCount[picked.track.artistId];
			++genreCount[picked.track.genre];
			unknown += picked.known ? 0 : 1;
			result.push_back(std::move(picked.track));
		}
		// Немного знакомого: пара любимых треков внутри подборки.
		if (lib.liked.size() >= 5 && result.size() > 6) {
			for (const auto position : { 2, 8, 16 }) {
				if (position < int(result.size())) {
					const auto &pick = lib.liked[base::RandomIndex(int(lib.liked.size()))];
					const auto exists = std::any_of(result.begin(), result.end(), [&](const Track &t) {
						return t.id == pick.track.id;
					});
					if (!exists) {
						result.insert(result.begin() + position, pick.track);
					}
				}
			}
		}
		for (const auto &t : result) {
			ShownRecently().insert(t.id);
		}
		if (ShownRecently().size() > 300) {
			ShownRecently().clear();
		}
		done(std::move(result), QString());
	};

	// Запросы кандидатов: related по семенам, треки любимых исполнителей, поиск по тегам.
	collector->pending = 0;
	struct Job { Fn<void()> run; };
	auto jobs = std::vector<Fn<void()>>();
	for (const auto &[id, weight] : seeds) {
		const auto w = std::clamp(weight / maxSeed, 0.3, 1.);
		jobs.push_back([=] {
			Related(id, 25, [=](std::vector<Track> list) {
				Gather(collector, std::move(list), w, false);
			});
		});
	}
	for (const auto &artist : topArtists) {
		jobs.push_back([=] {
			UserTracks(artist, 12, [=](std::vector<Track> list) {
				Gather(collector, std::move(list), 0.6, false);
			});
		});
	}
	for (const auto &genre : topGenres) {
		jobs.push_back([=] {
			SearchLimited(genre, 20, [=](std::vector<Track> list) {
				Gather(collector, std::move(list), 0.2, true);
			});
		});
	}
	for (const auto &tag : topTags) {
		jobs.push_back([=] {
			SearchLimited(tag, 20, [=](std::vector<Track> list) {
				Gather(collector, std::move(list), 0.2, true);
			});
		});
	}
	if (jobs.empty()) {
		done({}, QString::fromUtf8("Мало данных о вкусе: послушайте и полайкайте ещё несколько треков."));
		return;
	}
	collector->pending = int(jobs.size());
	for (const auto &job : jobs) {
		job();
	}
}

} // namespace Mokagram::Music
