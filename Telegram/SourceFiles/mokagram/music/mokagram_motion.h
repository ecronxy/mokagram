#pragma once

#include "base/basic_types.h"

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

namespace MotionProtocol {

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
[[nodiscard]] Fn<void()> FetchMotion(
	const QString &key,
	const QString &title,
	const QString &artist,
	qint64 duration,
	Fn<void(MotionFiles)> done);

} // namespace Mokagram::Music
