#include "mokagram/music/mokagram_motion.h"

#include "mokagram/lang/mokagram_lang.h"
#include "mokagram/settings/mokagram_settings.h"
#include "ui/toast/toast.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrlQuery>
#include <QtGui/QDesktopServices>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

namespace Mokagram::Music {
namespace {

constexpr auto kMaxVideo = qint64(24 * 1024 * 1024);
constexpr auto kMaxText = qint64(6 * 1024 * 1024);
constexpr auto kRedirect = "http://127.0.0.1:8899/mokagram";

struct Session {
	quint64 generation = 0;
	QString appleToken;
	qint64 appleExpires = 0;
	QString spotifyToken;
	QString spotifyRefresh;
	QString spotifyClient;
	qint64 spotifyExpires = 0;
	QPointer<QTcpServer> login;
};

Session &Tokens() {
	static auto result = Session();
	return result;
}

QNetworkAccessManager &Network() {
	static auto result = new QNetworkAccessManager();
	return *result;
}

QString CacheDir() {
	const auto path = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
		+ u"/mokagram_music/motion"_q;
	QDir().mkpath(path);
	return path;
}

void TrimCache() {
	const auto dir = QDir(CacheDir());
	const auto files = dir.entryInfoList({ u"*.mp4"_q }, QDir::Files, QDir::Time);
	auto retained = qint64(0);
	for (const auto &file : files) {
		retained += file.size();
		if (retained > 256 * 1024 * 1024) {
			QFile::remove(file.absoluteFilePath());
		}
	}
}

void Request(
		QObject *context,
		QUrl url,
		Fn<void(QByteArray, int)> done,
		QByteArray body = {},
		QByteArray authorization = {},
		QByteArray type = {},
		QByteArray range = {},
		qint64 limit = kMaxText) {
	if (url.scheme() != u"https"_q || url.host().isEmpty()
		|| !url.userInfo().isEmpty()) {
		done({}, 0);
		return;
	}
	auto request = QNetworkRequest(url);
	request.setTransferTimeout(20000);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::SameOriginRedirectPolicy);
	request.setRawHeader("User-Agent", "mokaGram/1.1 (Desktop music player)");
	if (!authorization.isEmpty()) request.setRawHeader("Authorization", authorization);
	if (!type.isEmpty()) request.setRawHeader("Content-Type", type);
	if (!range.isEmpty()) request.setRawHeader("Range", range);
	if (url.host().endsWith(u"music.apple.com"_q)) {
		request.setRawHeader("Origin", "https://music.apple.com");
		request.setRawHeader("Referer", "https://music.apple.com/");
	}
	const auto reply = body.isEmpty()
		? Network().get(request)
		: Network().post(request, body);
	QObject::connect(context, &QObject::destroyed, reply, [=] {
		QObject::disconnect(reply, nullptr, context, nullptr);
		reply->abort();
	});
	QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
		[=](qint64 received, qint64 total) {
			if (received > limit || total > limit) reply->abort();
		});
	QObject::connect(reply, &QNetworkReply::finished, context,
		[=, done = std::move(done)]() mutable {
			const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			const auto bytes = reply->error() == QNetworkReply::NoError
				&& reply->bytesAvailable() <= limit ? reply->readAll() : QByteArray();
			done(bytes, status);
		});
	QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

void RefreshSpotify(QObject *context, Fn<void(QString)> done) {
	auto &session = Tokens();
	if (session.spotifyClient != Settings::String(u"player.spotify_client_id"_q).trimmed()) {
		++session.generation;
		session.spotifyToken.clear();
		session.spotifyRefresh.clear();
		session.spotifyClient = Settings::String(u"player.spotify_client_id"_q).trimmed();
	}
	if (session.spotifyExpires > QDateTime::currentSecsSinceEpoch() + 60
		&& !session.spotifyToken.isEmpty()) {
		done(session.spotifyToken);
		return;
	}
	if (session.spotifyRefresh.isEmpty()) {
		done({});
		return;
	}
	auto query = QUrlQuery();
	query.addQueryItem(u"grant_type"_q, u"refresh_token"_q);
	query.addQueryItem(u"refresh_token"_q, session.spotifyRefresh);
	query.addQueryItem(u"client_id"_q, session.spotifyClient);
	const auto generation = session.generation;
	Request(context, QUrl(u"https://accounts.spotify.com/api/token"_q),
		[generation, done = std::move(done)](QByteArray bytes, int status) mutable {
			auto &session = Tokens();
			if (session.generation != generation
				|| session.spotifyClient != Settings::String(u"player.spotify_client_id"_q).trimmed()) {
				done({});
				return;
			}
			const auto json = QJsonDocument::fromJson(bytes).object();
			if (status == 200 && !json.value(u"access_token"_q).toString().isEmpty()) {
				session.spotifyToken = json.value(u"access_token"_q).toString();
				session.spotifyExpires = QDateTime::currentSecsSinceEpoch()
					+ std::clamp(json.value(u"expires_in"_q).toInt(), 60, 86400);
				if (json.contains(u"refresh_token"_q)) {
					session.spotifyRefresh = json.value(u"refresh_token"_q).toString();
				}
			} else {
				session.spotifyToken.clear();
				if (status == 400 || status == 401) session.spotifyRefresh.clear();
			}
			done(session.spotifyToken);
		}, query.toString(QUrl::FullyEncoded).toUtf8(), {},
		"application/x-www-form-urlencoded");
}

void LoginSpotify() {
	const auto toast = [](const char *text) {
		Ui::Toast::Show(QString::fromUtf8(Lang::Tr(text)));
	};
	const auto client = Settings::String(u"player.spotify_client_id"_q).trimmed();
	if (!QRegularExpression(u"^[A-Za-z0-9]{32}$"_q).match(client).hasMatch()) {
		toast("Укажите Client ID своего приложения Spotify в настройках плеера.");
		return;
	}
	if (Tokens().login) {
		toast("Вход в Spotify уже открыт в браузере.");
		return;
	}
	const auto server = new QTcpServer(&Network());
	if (!server->listen(QHostAddress::LocalHost, 8899)) {
		server->deleteLater();
		toast("Не удалось открыть локальное окно входа Spotify: порт 8899 занят.");
		return;
	}
	if (Tokens().spotifyClient != client) {
		Tokens().spotifyToken.clear();
		Tokens().spotifyRefresh.clear();
		Tokens().spotifyExpires = 0;
	}
	Tokens().spotifyClient = client;
	Tokens().login = server;
	const auto generation = ++Tokens().generation;
	const auto claimed = std::make_shared<bool>(false);
	auto random = QByteArray(32, Qt::Uninitialized);
	for (auto &byte : random) byte = char(QRandomGenerator::system()->generate());
	const auto verifier = random.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
	const auto state = QCryptographicHash::hash(random + "state", QCryptographicHash::Sha256).toHex();
	const auto challenge = QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
		.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
	QObject::connect(server, &QTcpServer::newConnection, server, [=] {
		while (server->hasPendingConnections()) {
			const auto socket = server->nextPendingConnection();
			QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
			QTimer::singleShot(10000, socket, [=] { socket->disconnectFromHost(); });
			const auto buffer = std::make_shared<QByteArray>();
			const auto handled = std::make_shared<bool>(false);
			QObject::connect(socket, &QTcpSocket::readyRead, server, [=] {
				if (*handled) return;
				buffer->append(socket->readAll());
				if (buffer->size() > 8192) {
					socket->disconnectFromHost();
					return;
				}
				if (!buffer->contains("\r\n\r\n")) return;
				*handled = true;
				const auto request = buffer->left(buffer->indexOf("\r\n")).split(' ');
				const auto url = request.size() == 3 && request[0] == "GET"
					? QUrl(QString::fromUtf8(request[1])) : QUrl();
				const auto query = QUrlQuery(url);
				const auto code = query.queryItemValue(u"code"_q);
				const auto valid = !*claimed && Tokens().generation == generation
					&& url.path() == u"/mokagram"_q && !code.isEmpty()
					&& query.queryItemValue(u"state"_q).toUtf8() == state;
				socket->write(valid
					? "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nReturn to mokaGram."
					: "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid login response.");
				socket->disconnectFromHost();
				if (!valid) return;
				*claimed = true;
				server->close();
				auto form = QUrlQuery();
				form.addQueryItem(u"grant_type"_q, u"authorization_code"_q);
				form.addQueryItem(u"code"_q, code);
				form.addQueryItem(u"redirect_uri"_q, QString::fromLatin1(kRedirect));
				form.addQueryItem(u"client_id"_q, client);
				form.addQueryItem(u"code_verifier"_q, QString::fromLatin1(verifier));
				Request(server, QUrl(u"https://accounts.spotify.com/api/token"_q),
					[=](QByteArray bytes, int status) {
						if (Tokens().generation != generation
							|| client != Settings::String(u"player.spotify_client_id"_q).trimmed()) {
							server->deleteLater();
							return;
						}
						const auto json = QJsonDocument::fromJson(bytes).object();
						auto &session = Tokens();
						if (status == 200 && !json.value(u"access_token"_q).toString().isEmpty()) {
							session.spotifyClient = client;
							session.spotifyToken = json.value(u"access_token"_q).toString();
							session.spotifyRefresh = json.value(u"refresh_token"_q).toString();
							session.spotifyExpires = QDateTime::currentSecsSinceEpoch()
								+ std::clamp(json.value(u"expires_in"_q).toInt(), 60, 86400);
							toast("Spotify подключён. Переключите трек, чтобы загрузить Canvas.");
						} else {
							toast("Spotify не подтвердил вход. Проверьте Client ID и Redirect URI.");
						}
						server->deleteLater();
					}, form.toString(QUrl::FullyEncoded).toUtf8(), {},
					"application/x-www-form-urlencoded");
			});
		}
	});
	QTimer::singleShot(180000, server, [=] { server->deleteLater(); });
	auto query = QUrlQuery();
	query.addQueryItem(u"client_id"_q, client);
	query.addQueryItem(u"response_type"_q, u"code"_q);
	query.addQueryItem(u"scope"_q, u"streaming user-read-private"_q);
	query.addQueryItem(u"redirect_uri"_q, QString::fromLatin1(kRedirect));
	query.addQueryItem(u"state"_q, QString::fromLatin1(state));
	query.addQueryItem(u"code_challenge_method"_q, u"S256"_q);
	query.addQueryItem(u"code_challenge"_q, QString::fromLatin1(challenge));
	auto url = QUrl(u"https://accounts.spotify.com/authorize"_q);
	url.setQuery(query);
	if (!QDesktopServices::openUrl(url)) {
		server->deleteLater();
		toast("Не удалось открыть браузер для входа в Spotify.");
	}
}

class MotionJob final : public QObject {
public:
	MotionJob(QString key, QString title, QString artist, qint64 duration,
		Fn<void(MotionFiles)> done)
	: QObject(&Network())
	, _key(std::move(key))
	, _title(std::move(title))
	, _artist(std::move(artist))
	, _duration(duration)
	, _done(std::move(done)) {
		QTimer::singleShot(90000, _requests.get(), [=] { finish(); });
	}

	~MotionJob() override {
		_requests.reset();
	}

	void cancel() {
		_done = nullptr;
		_requests.reset();
		deleteLater();
	}

	void start() {
		if (!Settings::Bool(u"player.motion_covers"_q)) {
			spotify();
			return;
		}
		if (QFile::exists(path(u"apple"_q))) {
			_files.artwork = path(u"apple"_q);
			spotify();
		} else if (Tokens().appleExpires > QDateTime::currentSecsSinceEpoch() + 60) {
			appleSearch();
		} else {
			Request(_requests.get(), QUrl(u"https://music.apple.com/us/browse"_q),
				[=](QByteArray bytes, int) {
					const auto pattern = QRegularExpression(
						u"/assets/index(?:-legacy)?[~-][A-Za-z0-9_-]+\\.js"_q);
					const auto match = pattern.match(QString::fromUtf8(bytes));
					if (!match.hasMatch()) { spotify(); return; }
					Request(_requests.get(), QUrl(u"https://music.apple.com"_q + match.captured()),
						[=](QByteArray js, int) {
							const auto jwt = QRegularExpression(
								u"ey[A-Za-z0-9_-]+\\.ey[A-Za-z0-9_-]+\\.[A-Za-z0-9_-]+"_q);
							auto matches = jwt.globalMatch(QString::fromUtf8(js));
							while (matches.hasNext()) {
								const auto token = matches.next().captured();
								const auto payload = QByteArray::fromBase64(token.split('.')[1].toLatin1(),
									QByteArray::Base64UrlEncoding);
								const auto expires = QJsonDocument::fromJson(payload).object()
									.value(u"exp"_q).toInteger();
								if (expires > QDateTime::currentSecsSinceEpoch() + 60) {
									Tokens().appleToken = token;
									Tokens().appleExpires = expires;
									appleSearch();
									return;
								}
							}
							spotify();
						});
				});
		}
	}

private:
	QString path(const QString &source) const {
		return CacheDir() + '/' + QString::fromLatin1(QCryptographicHash::hash(
			(_key + ':' + _title + ':' + _artist + ':' + QString::number(_duration))
			.toUtf8(), QCryptographicHash::Sha256).toHex()) + '-' + source + u".mp4"_q;
	}

	void finish() {
		if (!_done) return;
		const auto done = std::move(_done);
		_requests.reset();
		deleteLater();
		done(std::move(_files));
	}

	void appleSearch() {
		auto url = QUrl(u"https://amp-api.music.apple.com/v1/catalog/us/search"_q);
		auto query = QUrlQuery();
		query.addQueryItem(u"types"_q, u"songs"_q);
		query.addQueryItem(u"limit"_q, u"10"_q);
		query.addQueryItem(u"term"_q, _artist + ' ' + _title);
		url.setQuery(query);
		Request(_requests.get(), url, [=](QByteArray bytes, int status) {
			if (status == 401) Tokens().appleExpires = 0;
			const auto songs = QJsonDocument::fromJson(bytes).object()
				.value(u"results"_q).toObject().value(u"songs"_q).toObject()
				.value(u"data"_q).toArray();
			for (const auto &song : songs) {
				const auto attributes = song.toObject().value(u"attributes"_q).toObject();
				if (!MotionProtocol::MatchesTrack(_title, _artist, _duration,
					attributes.value(u"name"_q).toString(),
					attributes.value(u"artistName"_q).toString(),
					attributes.value(u"durationInMillis"_q).toInteger())) continue;
				const auto isrc = attributes.value(u"isrc"_q).toString();
				if (isrc.isEmpty()) continue;
				appleAlbum(isrc);
				return;
			}
			spotify();
		}, {}, "Bearer " + Tokens().appleToken.toUtf8());
	}

	static QUrl editorialUrl(const QJsonValue &value) {
		if (!value.isObject()) return {};
		const auto object = value.toObject();
		const auto video = QUrl(object.value(u"video"_q).toString());
		if (video.scheme() == u"https"_q && video.path().endsWith(u".m3u8"_q)) return video;
		for (const auto &key : { u"motionSquareVideo1x1"_q, u"motionDetailSquare"_q }) {
			const auto found = editorialUrl(object.value(key));
			if (!found.isEmpty()) return found;
		}
		return {};
	}

	void appleAlbum(const QString &isrc) {
		auto url = QUrl(u"https://amp-api.music.apple.com/v1/catalog/us/songs"_q);
		auto query = QUrlQuery();
		query.addQueryItem(u"include"_q, u"albums"_q);
		query.addQueryItem(u"extend[albums]"_q, u"editorialVideo"_q);
		query.addQueryItem(u"filter[isrc]"_q, isrc);
		url.setQuery(query);
		Request(_requests.get(), url, [=](QByteArray bytes, int) {
			const auto songs = QJsonDocument::fromJson(bytes).object().value(u"data"_q).toArray();
			for (const auto &song : songs) {
				const auto albums = song.toObject().value(u"relationships"_q).toObject()
					.value(u"albums"_q).toObject().value(u"data"_q).toArray();
				for (const auto &album : albums) {
					const auto attributes = album.toObject().value(u"attributes"_q).toObject();
					const auto video = editorialUrl(attributes.value(u"editorialVideo"_q));
					if (video.isEmpty()) continue;
					hls(video);
					return;
				}
			}
			spotify();
		}, {}, "Bearer " + Tokens().appleToken.toUtf8());
	}

	void hls(const QUrl &url) {
		Request(_requests.get(), url, [=](QByteArray bytes, int) {
			const auto variant = MotionProtocol::HlsVariant(url, bytes);
			if (variant.isEmpty()) { spotify(); return; }
			Request(_requests.get(), variant, [=](QByteArray playlist, int) {
				_parts = MotionProtocol::HlsParts(variant, playlist);
				if (_parts.empty()) { spotify(); return; }
				downloadPart();
			});
		});
	}

	void downloadPart() {
		if (_part == _parts.size()) {
			_files.artwork = save(u"apple"_q, _video);
			_video.clear();
			spotify();
			return;
		}
		const auto part = _parts[_part];
		const auto range = part.length > 0 ? "bytes=" + QByteArray::number(part.offset)
			+ '-' + QByteArray::number(part.offset + part.length - 1) : QByteArray();
		Request(_requests.get(), part.url, [=](QByteArray bytes, int status) {
			if (bytes.isEmpty() || (part.length > 0
				&& (status != 206 || bytes.size() != part.length))
				|| _video.size() + bytes.size() > kMaxVideo) {
				_video.clear();
				spotify();
				return;
			}
			_video.append(bytes);
			++_part;
			downloadPart();
		}, {}, {}, {}, range, kMaxVideo - _video.size());
	}

	QString save(const QString &source, const QByteArray &bytes) {
		if (bytes.size() < 12 || bytes.size() > kMaxVideo
			|| bytes.mid(4, 4) != "ftyp") return {};
		auto file = QSaveFile(path(source));
		if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()
			|| !file.commit()) return {};
		TrimCache();
		return file.fileName();
	}

	void spotify() {
		if (!Settings::Bool(u"player.spotify_canvas"_q)) { finish(); return; }
		if (QFile::exists(path(u"spotify"_q))) {
			_files.canvas = path(u"spotify"_q);
			finish();
			return;
		}
		RefreshSpotify(_requests.get(), [=](QString token) {
			if (token.isEmpty()) { finish(); return; }
			auto url = QUrl(u"https://api.spotify.com/v1/search"_q);
			auto query = QUrlQuery();
			query.addQueryItem(u"q"_q, _artist + ' ' + _title);
			query.addQueryItem(u"type"_q, u"track"_q);
			query.addQueryItem(u"limit"_q, u"10"_q);
			url.setQuery(query);
			Request(_requests.get(), url, [=](QByteArray bytes, int status) {
				if (status == 401) Tokens().spotifyExpires = 0;
				const auto tracks = QJsonDocument::fromJson(bytes).object()
					.value(u"tracks"_q).toObject().value(u"items"_q).toArray();
				for (const auto &value : tracks) {
					const auto track = value.toObject();
					auto artists = QStringList();
					for (const auto &artist : track.value(u"artists"_q).toArray()) {
						artists.push_back(artist.toObject().value(u"name"_q).toString());
					}
					if (!MotionProtocol::MatchesTrack(_title, _artist, _duration,
						track.value(u"name"_q).toString(), artists.join(' '),
						track.value(u"duration_ms"_q).toInteger())) continue;
					const auto uri = track.value(u"uri"_q).toString();
					if (!uri.startsWith(u"spotify:track:"_q)) continue;
					canvas(uri, token);
					return;
				}
				finish();
			}, {}, "Bearer " + token.toUtf8());
		});
	}

	void canvas(const QString &uri, const QString &token) {
		Request(_requests.get(), QUrl(u"https://spclient.wg.spotify.com/canvaz-cache/v0/canvases"_q),
			[=](QByteArray bytes, int status) {
				if (status == 401) Tokens().spotifyExpires = 0;
				const auto url = MotionProtocol::CanvasUrl(bytes, uri);
				if (url.isEmpty()) { finish(); return; }
				Request(_requests.get(), url, [=](QByteArray video, int) {
					_files.canvas = save(u"spotify"_q, video);
					finish();
				}, {}, {}, {}, {}, kMaxVideo);
			}, MotionProtocol::CanvasRequest(uri), "Bearer " + token.toUtf8(),
			"application/x-protobuf");
	}

	QString _key;
	QString _title;
	QString _artist;
	qint64 _duration = 0;
	Fn<void(MotionFiles)> _done;
	MotionFiles _files;
	std::vector<MotionPart> _parts;
	qsizetype _part = 0;
	QByteArray _video;
	std::unique_ptr<QObject> _requests = std::make_unique<QObject>();

};

} // namespace

void RegisterMotionSettings() {
	const auto add = [](Settings::Entry entry) {
		entry.section = QString::fromUtf8(Lang::Tr("Плеер"));
		entry.category = u"look"_q;
		entry.sectionIcon = u"hide"_q;
		Settings::Register(std::move(entry));
	};
	add({ .key = u"player.dynamic_background"_q,
		.title = QString::fromUtf8(Lang::Tr("Динамичный фон плеера")),
		.kind = Settings::Kind::Bool, .defaultValue = true });
	add({ .key = u"player.expressive_seek"_q,
		.title = QString::fromUtf8(Lang::Tr("Волнистая полоса воспроизведения")),
		.kind = Settings::Kind::Bool, .defaultValue = true });
	add({ .key = u"player.motion_covers"_q,
		.title = QString::fromUtf8(Lang::Tr("Живые обложки Apple Music")),
		.description = QString::fromUtf8(Lang::Tr("Ищет обложку по названию и исполнителю в Apple Music и кэширует видео.")),
		.kind = Settings::Kind::Bool, .defaultValue = false });
	add({ .key = u"player.public_canvas"_q,
		.title = QString::fromUtf8(Lang::Tr("Canvas из публичного каталога")),
		.description = QString::fromUtf8(Lang::Tr("Автоматически ищет видео по исполнителю и названию через Canvas Downloader; вход в Spotify не требуется.")),
		.kind = Settings::Kind::Bool, .defaultValue = true });
	add({ .key = u"player.spotify_canvas"_q,
		.title = QString::fromUtf8(Lang::Tr("Видео Spotify Canvas")),
		.description = QString::fromUtf8(Lang::Tr("Требует входа в Spotify. При отсутствии видео остаётся обычный фон.")),
		.kind = Settings::Kind::Bool, .defaultValue = false });
	add({ .key = u"player.spotify_client_id"_q,
		.title = QString::fromUtf8(Lang::Tr("Spotify Client ID")),
		.description = QString::fromUtf8(Lang::Tr("Client ID приложения Spotify с Redirect URI http://127.0.0.1:8899/mokagram. Секрет приложения не нужен.")),
		.kind = Settings::Kind::String, .defaultValue = u"a3a01911beb24158bb925ad5ba8dd26c"_q,
		.dependsOn = u"player.spotify_canvas"_q });
	add({ .key = u"player.spotify_login"_q,
		.title = QString::fromUtf8(Lang::Tr("Войти в Spotify")),
		.description = QString::fromUtf8(Lang::Tr("Вход через браузер. Сеанс Spotify хранится в памяти до выхода из mokaGram.")),
		.kind = Settings::Kind::String, .defaultValue = QString(),
		.dependsOn = u"player.spotify_canvas"_q, .action = LoginSpotify });
	add({ .key = u"player.spotify_logout"_q,
		.title = QString::fromUtf8(Lang::Tr("Отключить Spotify")),
		.kind = Settings::Kind::String, .defaultValue = QString(),
		.action = [] {
			auto &session = Tokens();
			++session.generation;
			if (session.login) session.login->deleteLater();
			session.spotifyToken.clear();
			session.spotifyRefresh.clear();
			session.spotifyExpires = 0;
			Ui::Toast::Show(QString::fromUtf8(Lang::Tr("Spotify отключён.")));
		} });
}

Fn<void()> FetchMotion(const QString &key, const QString &title,
		const QString &artist, qint64 duration, Fn<void(MotionFiles)> done) {
	const auto job = new MotionJob(key, title, artist, duration, std::move(done));
	job->start();
	return [weak = QPointer(job)] {
		if (weak) weak->cancel();
	};
}

} // namespace Mokagram::Music
