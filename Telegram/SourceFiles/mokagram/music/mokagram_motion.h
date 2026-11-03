#pragma once

#include "base/basic_types.h"
#include "rpl/producer.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QUrl>
#include <vector>

namespace Mokagram::Music {

struct MotionFiles {
	QString artwork;
	QString canvas;
};

struct MotionPart {
	QUrl url;
	qint64 offset = 0;
	qint64 length = -1;
};

struct MotionMetadata {
	QString title;
	QString artist;
};

namespace MotionProtocol {

[[nodiscard]] MotionMetadata Metadata(
	QString title,
	QString artist,
	bool soundcloud = false);
[[nodiscard]] bool MatchesTitle(const QString &title, const QString &candidate);

[[nodiscard]] QUrl HlsVariant(const QUrl &base, const QByteArray &playlist);
[[nodiscard]] std::vector<MotionPart> HlsParts(
	const QUrl &base,
	const QByteArray &playlist);
[[nodiscard]] QByteArray CanvasRequest(const QString &trackUri);
[[nodiscard]] QUrl CanvasUrl(const QByteArray &protobuf, const QString &trackUri);
[[nodiscard]] bool MatchesTrack(
	const QString &title,
	const QString &artist,
	qint64 duration,
	const QString &candidateTitle,
	const QString &candidateArtist,
	qint64 candidateDuration);

} // namespace MotionProtocol

void RegisterMotionSettings();
[[nodiscard]] rpl::producer<> MotionChanges();
[[nodiscard]] Fn<void()> FetchMotion(
	const QString &key,
	const QString &title,
	const QString &artist,
	qint64 duration,
	Fn<void(MotionFiles)> done);

} // namespace Mokagram::Music
