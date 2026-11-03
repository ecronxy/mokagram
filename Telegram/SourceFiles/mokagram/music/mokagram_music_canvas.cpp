#include "mokagram/music/mokagram_music_canvas.h"

#include "media/streaming/media_streaming_loader_local.h"
#include "media/streaming/media_streaming_reader.h"
#include "ui/effects/animations.h"
#include "ui/power_saving.h"

#include <QtCore/QCache>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QUrlQuery>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <QtNetwork/QNetworkReply>

namespace Mokagram::Music {
namespace {

constexpr auto kMetadataLimit = 1024 * 1024;
constexpr auto kVideoLimit = 8 * 1024 * 1024;
constexpr auto kCandidateLimit = 30;

QString Normalize(const QString &text) {
	auto result = QString();
	for (const auto ch : text.normalized(QString::NormalizationForm_KC).toCaseFolded()) {
		if (ch.isLetterOrNumber()) {
			result += ch;
		}
	}
	return result;
}

QCache<QString, QString> &Titles() {
	static auto result = QCache<QString, QString>(512);
	return result;
}

QCache<QString, QByteArray> &Videos() {
	static auto result = QCache<QString, QByteArray>(24 * 1024 * 1024);
	return result;
}

bool IsVideoUrl(const QUrl &url) {
	return url.scheme() == u"https"_q
		&& url.host() == u"canvaz.scdn.co"_q
		&& url.path().endsWith(u".mp4"_q)
		&& url.userInfo().isEmpty()
		&& url.port(-1) == -1;
}

} // namespace

CanvasPlayer::CanvasPlayer(QObject *parent, Fn<void()> repaint)
: QObject(parent)
, _repaint(std::move(repaint))
, _loadingAnimation([=](crl::time) {
	if (!_loading
		|| anim::Disabled()
		|| PowerSaving::On(PowerSaving::kAnimations)
		|| QGuiApplication::applicationState() != Qt::ApplicationActive) {
		return false;
	}
	_repaint();
	return true;
}) {
	connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [=] {
		updatePlayback();
		_repaint();
	});
}

CanvasPlayer::~CanvasPlayer() {
	_requests.reset();
	_playbackLifetime.destroy();
	_player.reset();
}

void CanvasPlayer::refresh(
		QString id,
		QString title,
		QString artist,
		bool playing) {
	if (_playing != playing) {
		_playing = playing;
		updatePlayback();
		_repaint();
	}
	if (_id == id) {
		return;
	}
	setLoading(false);
	_loadingProgress = -1.;
	_requests = std::make_unique<QObject>();
	_playbackLifetime.destroy();
	_player.reset();
	_candidates.clear();
	_candidate = 0;
	_pages = 0;
	_nextPage = QUrl();
	_id = std::move(id);
	_title = Normalize(title);
	_repaint();
	if (_id.isEmpty() || _title.isEmpty() || artist.trimmed().isEmpty()) {
		return;
	}
	setLoading(true);
	if (const auto cached = Videos().object(_id)) {
		startVideo(*cached);
		return;
	}
	findArtist(artist);
}

void CanvasPlayer::get(
		QUrl url,
		int limit,
		Fn<void(QByteArray)> done) {
	auto request = QNetworkRequest(url);
	request.setRawHeader("User-Agent", "mokaGram/1.1 (Desktop music player)");
	request.setTransferTimeout(10000);
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::SameOriginRedirectPolicy);
	const auto reply = _network.get(request);
	reply->setParent(_requests.get());
	reply->setReadBufferSize(limit + 1);
	connect(reply, &QNetworkReply::readyRead, reply, [=] {
		if (reply->bytesAvailable() > limit) {
			reply->abort();
		}
	});
	connect(reply, &QNetworkReply::downloadProgress, reply, [=](qint64 received, qint64 total) {
		if (total > limit) {
			reply->abort();
			return;
		}
		if (limit == kVideoLimit && total > 0) {
			_loadingProgress = std::clamp(float64(received) / total, 0., 1.);
			_repaint();
		}
	});
	QTimer::singleShot(20000, reply, [=] {
		if (!reply->isFinished()) {
			reply->abort();
		}
	});
	connect(reply, &QNetworkReply::finished, _requests.get(), [=] {
		const auto ok = reply->error() == QNetworkReply::NoError
			&& reply->bytesAvailable() <= limit;
		const auto data = ok ? reply->readAll() : QByteArray();
		reply->deleteLater();
		if (data.isEmpty()) {
			setLoading(false);
		}
		done(data);
	});
}

void CanvasPlayer::findArtist(const QString &artist) {
	auto url = QUrl(u"https://www.canvasdownloader.com/api/search"_q);
	auto query = QUrlQuery();
	query.addQueryItem(u"q"_q, artist);
	url.setQuery(query);
	get(url, kMetadataLimit, [=](QByteArray data) {
		readArtist(data, artist);
	});
}

void CanvasPlayer::readArtist(
		const QByteArray &data,
		const QString &artist) {
	const auto artists = QJsonDocument::fromJson(data).object()
		.value(u"artists"_q).toArray();
	for (const auto &entry : artists) {
		const auto object = entry.toObject();
		if (Normalize(object.value(u"name"_q).toString()) != Normalize(artist)) {
			continue;
		}
		const auto slug = object.value(u"slug"_q).toString();
		if (slug.isEmpty() || slug.contains('/') || slug.contains('.')) {
			setLoading(false);
			return;
		}
		auto url = QUrl(u"https://www.canvasdownloader.com"_q);
		url.setPath(u"/artists/"_q + slug);
		get(url, kMetadataLimit, [=](QByteArray page) {
			readCandidates(page);
		});
		return;
	}
	setLoading(false);
}

void CanvasPlayer::readCandidates(const QByteArray &data) {
	static const auto pattern = QRegularExpression(
		uR"re(artist-page__canvas-container[^>]*>(?:(?!artist-page__canvas-container).)*?<source\s+src="([^"]+)"(?:(?!artist-page__canvas-container).)*?href="spotify:track:([A-Za-z0-9]{22})")re"_q,
		QRegularExpression::DotMatchesEverythingOption);
	const auto html = QString::fromUtf8(data);
	auto matches = pattern.globalMatch(html);
	while (matches.hasNext() && _candidates.size() < kCandidateLimit) {
		const auto match = matches.next();
		auto url = QUrl(match.captured(1));
		url.setFragment(QString());
		if (IsVideoUrl(url)) {
			_candidates.push_back({ match.captured(2), url });
		}
	}
	static const auto next = QRegularExpression(
		uR"re(href="([^"]+)"\s+aria-label="Next")re"_q);
	const auto match = next.match(html);
	_nextPage = QUrl();
	if (++_pages < 3 && match.hasMatch()) {
		const auto url = QUrl(u"https://www.canvasdownloader.com"_q)
			.resolved(QUrl(match.captured(1)));
		if (url.scheme() == u"https"_q
			&& url.host() == u"www.canvasdownloader.com"_q
			&& url.path().startsWith(u"/artists/"_q)) {
			_nextPage = url;
		}
	}
	findTitle();
}

void CanvasPlayer::findTitle() {
	while (_candidate < int(_candidates.size())) {
		const auto candidate = _candidates[_candidate++];
		if (const auto title = Titles().object(candidate.trackId)) {
			if (*title == _title) {
				loadVideo(candidate.video);
				return;
			}
			continue;
		}
		auto url = QUrl(u"https://open.spotify.com/oembed"_q);
		auto query = QUrlQuery();
		query.addQueryItem(
			u"url"_q,
			u"https://open.spotify.com/track/"_q + candidate.trackId);
		url.setQuery(query);
		get(url, kMetadataLimit, [=](QByteArray data) {
			const auto title = Normalize(QJsonDocument::fromJson(data).object()
				.value(u"title"_q).toString());
			if (!title.isEmpty()) {
				Titles().insert(candidate.trackId, new QString(title));
			}
			if (!title.isEmpty() && title == _title) {
				loadVideo(candidate.video);
			} else if (!data.isEmpty()) {
				findTitle();
			}
		});
		return;
	}
	if (!_nextPage.isEmpty() && _candidates.size() < kCandidateLimit) {
		const auto next = base::take(_nextPage);
		get(next, kMetadataLimit, [=](QByteArray data) {
			readCandidates(data);
		});
	} else {
		setLoading(false);
	}
}

void CanvasPlayer::loadVideo(QUrl url) {
	_loadingProgress = -1.;
	get(url, kVideoLimit, [=](QByteArray data) {
		if (!data.isEmpty()) {
			startVideo(std::move(data));
		}
	});
}

void CanvasPlayer::startVideo(QByteArray data) {
	const auto cost = int(data.size());
	Videos().insert(_id, new QByteArray(data), cost);
	_playbackLifetime.destroy();
	_player = std::make_unique<Media::Streaming::Player>(
		std::make_shared<Media::Streaming::Reader>(
			Media::Streaming::MakeBytesLoader(data)));
	_player->updates() | rpl::on_next_error([=](const Media::Streaming::Update &) {
		if (_player->ready()) {
			setLoading(false);
		}
		_repaint();
	}, [=](Media::Streaming::Error) {
		Videos().remove(_id);
		setLoading(false);
		_repaint();
	}, _playbackLifetime);
	_player->play({
		.mode = Media::Streaming::Mode::Video,
		.volume = 0.,
		.waitForMarkAsShown = true,
		.loop = true,
	});
	updatePlayback();
}

void CanvasPlayer::setLoading(bool loading) {
	if (_loading == loading) {
		return;
	}
	_loading = loading;
	if (!loading) {
		_loadingAnimation.stop();
	}
	_repaint();
}

void CanvasPlayer::updatePlayback() {
	if (!_player || _player->failed()) {
		return;
	}
	const auto animate = _playing
		&& !anim::Disabled()
		&& !PowerSaving::On(PowerSaving::kAnimations)
		&& QGuiApplication::applicationState() == Qt::ApplicationActive;
	if (animate && _player->paused()) {
		_player->resume();
	} else if (!animate && !_player->paused()) {
		_player->pause();
	}
}

bool CanvasPlayer::paint(QPainter &p, QRect area) {
	if (!_player || !_player->ready() || _player->failed()) {
		return false;
	}
	updatePlayback();
	_frameSize = _player->videoSize().scaled(480, 720, Qt::KeepAspectRatio);
	const auto frame = _player->frame({
		.resize = _frameSize,
		.outer = _frameSize,
	});
	_player->markFrameShown();
	if (frame.isNull()) {
		return false;
	}
	const auto scaled = frame.size().scaled(area.size(), Qt::KeepAspectRatioByExpanding);
	const auto target = QRect(area.center() - QPoint(scaled.width(), scaled.height()) / 2, scaled);
	p.save();
	p.setClipRect(area, Qt::IntersectClip);
	p.drawImage(target, frame);
	p.restore();
	return true;
}

void CanvasPlayer::paintLoading(QPainter &p, QRect area, QColor color) {
	if (!_loading || area.isEmpty()) {
		return;
	}
	const auto animate = _loadingProgress < 0.
		&& !anim::Disabled()
		&& !PowerSaving::On(PowerSaving::kAnimations)
		&& QGuiApplication::applicationState() == Qt::ApplicationActive;
	if (animate && !_loadingAnimation.animating()) {
		_loadingAnimation.start();
	} else if (!animate) {
		_loadingAnimation.stop();
	}
	const auto line = QRectF(area);
	const auto radius = line.height() / 2.;
	auto fill = line;
	if (_loadingProgress >= 0.) {
		fill.setWidth(std::max(line.width() * _loadingProgress, line.height()));
	} else {
		const auto phase = animate
			? ((crl::now() - _loadingAnimation.started()) % 1400) / 1400.
			: 0.5;
		const auto width = line.width() * 0.28;
		fill.setLeft(line.left() + (line.width() + width) * phase - width);
		fill.setWidth(width);
	}
	p.save();
	p.setClipRect(line, Qt::IntersectClip);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(255, 255, 255, 46));
	p.drawRoundedRect(line, radius, radius);
	p.setBrush(color);
	p.drawRoundedRect(fill, radius, radius);
	p.restore();
}

} // namespace Mokagram::Music
