#include "mokagram/music/mokagram_motion.h"

#include <QtCore/QRegularExpression>
#include <cmath>
#include <limits>

namespace Mokagram::Music::MotionProtocol {
namespace {

[[nodiscard]] bool Https(const QUrl &url) {
	return url.isValid() && url.scheme() == u"https"_q
		&& !url.host().isEmpty() && url.userInfo().isEmpty();
}

[[nodiscard]] QString Normalize(QString value) {
	value = value.normalized(QString::NormalizationForm_KD).toCaseFolded();
	value.remove(QRegularExpression(u"[^\\p{L}\\p{N}]"_q));
	return value;
}

void Varint(QByteArray &data, quint64 value) {
	while (value >= 128) {
		data.append(char((value & 127) | 128));
		value >>= 7;
	}
	data.append(char(value));
}

[[nodiscard]] bool ReadVarint(
		const QByteArray &data,
		qsizetype &position,
		quint64 &value) {
	value = 0;
	for (auto shift = 0; shift < 64 && position < data.size(); shift += 7) {
		const auto byte = quint8(data[position++]);
		if (shift == 63 && byte > 1) {
			return false;
		}
		value |= quint64(byte & 127) << shift;
		if (!(byte & 128)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool ReadField(
		const QByteArray &data,
		qsizetype &position,
		quint64 &field,
		QByteArray &bytes) {
	auto tag = quint64(0);
	if (!ReadVarint(data, position, tag) || !(tag >> 3)) {
		return false;
	}
	field = tag >> 3;
	bytes.clear();
	auto length = quint64(0);
	switch (tag & 7) {
	case 0: return ReadVarint(data, position, length);
	case 1: length = 8; break;
	case 5: length = 4; break;
	case 2:
		if (!ReadVarint(data, position, length)) {
			return false;
		}
		break;
	default: return false;
	}
	if (length > quint64(data.size() - position)) {
		return false;
	}
	if ((tag & 7) == 2) {
		bytes = data.mid(position, qsizetype(length));
	}
	position += qsizetype(length);
	return true;
}

} // namespace

MotionMetadata Metadata(QString title, QString artist, bool soundcloud) {
	static const auto decoration = QRegularExpression(
		uR"re(\s*[\(\[]\s*(?:official\s+(?:audio|video|music\s+video)|lyrics?(?:\s+video)?|visuali[sz]er|audio|hd|hq)\s*[\)\]]\s*)re"_q,
		QRegularExpression::CaseInsensitiveOption);
	title.replace(decoration, u" "_q);
	static const auto separator = QRegularExpression(u"\\s+[-–—]\\s+"_q);
	static const auto version = QRegularExpression(
		uR"re(^(?:live|remix|remaster(?:ed)?|slowed|sped\s*up|instrumental|acoustic|radio\s*edit)(?:\b|$))re"_q,
		QRegularExpression::CaseInsensitiveOption);
	const auto split = separator.match(title);
	if (split.hasMatch()) {
		const auto prefix = title.left(split.capturedStart()).trimmed();
		const auto suffix = title.mid(split.capturedEnd()).trimmed();
		if (!prefix.isEmpty() && !suffix.isEmpty()
			&& ((soundcloud && !version.match(suffix).hasMatch())
				|| Normalize(prefix) == Normalize(artist))) {
			title = suffix;
			artist = prefix;
		}
	}
	return { title.simplified(), artist.simplified() };
}

bool MatchesTitle(const QString &title, const QString &candidate) {
	static const auto featured = QRegularExpression(
		uR"re(\s*(?:[\(\[]\s*(?:feat\.?|ft\.?|featuring)\s+[^\)\]]*[\)\]]|(?:feat\.?|ft\.?|featuring)\s+.+)$)re"_q,
		QRegularExpression::CaseInsensitiveOption);
	const auto comparable = [&](const QString &value) {
		auto result = Metadata(value, {}).title;
		result.remove(featured);
		return Normalize(result);
	};
	const auto a = comparable(title);
	const auto b = comparable(candidate);
	return !a.isEmpty() && a == b;
}

QUrl HlsVariant(const QUrl &base, const QByteArray &playlist) {
	const auto lines = QString::fromUtf8(playlist).split('\n');
	auto result = QUrl();
	auto best = std::numeric_limits<int>::max();
	const auto resolution = QRegularExpression(u"RESOLUTION=(\\d+)x(\\d+)"_q);
	for (auto i = 0; i + 1 < lines.size(); ++i) {
		const auto line = lines[i].trimmed();
		if (!line.startsWith(u"#EXT-X-STREAM-INF:"_q)
			|| !line.contains(u"avc1"_q)
			|| line.contains(u"VIDEO-RANGE=PQ"_q)
			|| line.contains(u"VIDEO-RANGE=HLG"_q)) {
			continue;
		}
		const auto match = resolution.match(line);
		const auto size = std::max(match.captured(1).toInt(), match.captured(2).toInt());
		const auto uri = lines[i + 1].trimmed();
		const auto url = base.resolved(QUrl(uri));
		if (size <= 0 || size > 1920 || uri.startsWith('#') || !Https(url)) {
			continue;
		}
		const auto score = size >= 640 ? size - 640 : 10000 - size;
		if (score < best) {
			best = score;
			result = url;
		}
	}
	return result;
}

std::vector<MotionPart> HlsParts(const QUrl &base, const QByteArray &playlist) {
	auto result = std::vector<MotionPart>();
	auto previous = QUrl();
	auto nextOffset = qint64(0);
	auto rangeOffset = qint64(-1);
	auto rangeLength = qint64(-1);
	auto total = qint64(0);
	const auto uri = QRegularExpression(u"URI=\"([^\"]+)\""_q);
	const auto mapRange = QRegularExpression(u"BYTERANGE=\"(\\d+)(?:@(\\d+))?\""_q);
	for (const auto &raw : QString::fromUtf8(playlist).split('\n')) {
		const auto line = raw.trimmed();
		if (line.startsWith(u"#EXT-X-KEY:"_q)
			&& !line.contains(u"METHOD=NONE"_q)) {
			return {};
		}
		auto part = MotionPart();
		if (line.startsWith(u"#EXT-X-MAP:"_q)) {
			const auto match = uri.match(line);
			if (!match.hasMatch()) {
				return {};
			}
			part.url = base.resolved(QUrl(match.captured(1)));
			const auto range = mapRange.match(line);
			if (range.hasMatch()) {
				auto valid = false;
				part.length = range.captured(1).toLongLong(&valid);
				if (!valid) return {};
				if (!range.captured(2).isEmpty()) {
					part.offset = range.captured(2).toLongLong(&valid);
					if (!valid) return {};
				}
			}
		} else if (line.startsWith(u"#EXT-X-BYTERANGE:"_q)) {
			const auto range = line.mid(17).split('@');
			auto ok = false;
			rangeLength = range[0].toLongLong(&ok);
			if (!ok || rangeLength <= 0) {
				return {};
			}
			rangeOffset = range.size() > 1 ? range[1].toLongLong(&ok) : -1;
			if (!ok || rangeOffset < -1) {
				return {};
			}
			continue;
		} else if (!line.isEmpty() && !line.startsWith('#')) {
			part.url = base.resolved(QUrl(line));
			part.length = rangeLength;
			part.offset = rangeOffset >= 0 ? rangeOffset
				: part.url == previous ? nextOffset : 0;
			rangeLength = rangeOffset = -1;
		} else {
			continue;
		}
		if (!Https(part.url) || part.offset < 0 || part.offset > 64 * 1024 * 1024
			|| part.length == 0 || part.length > 24 * 1024 * 1024
			|| result.size() >= 128) {
			return {};
		}
		total += std::max(qint64(0), part.length);
		if (total > 24 * 1024 * 1024) {
			return {};
		}
		previous = part.url;
		nextOffset = part.offset + std::max(qint64(0), part.length);
		result.push_back(std::move(part));
	}
	return result;
}

QByteArray CanvasRequest(const QString &trackUri) {
	const auto uri = trackUri.toUtf8();
	auto inner = QByteArray(1, char(10));
	Varint(inner, uri.size());
	inner.append(uri);
	auto outer = QByteArray(1, char(10));
	Varint(outer, inner.size());
	return outer + inner;
}

QUrl CanvasUrl(const QByteArray &protobuf, const QString &trackUri) {
	auto position = qsizetype(0);
	while (position < protobuf.size()) {
		auto field = quint64(0);
		auto bytes = QByteArray();
		if (!ReadField(protobuf, position, field, bytes)) {
			return {};
		}
		if (field != 1 || bytes.isEmpty()) {
			continue;
		}
		auto inner = qsizetype(0);
		auto uri = QString();
		auto url = QUrl();
		while (inner < bytes.size()) {
			auto value = QByteArray();
			if (!ReadField(bytes, inner, field, value)) {
				return {};
			}
			if (field == 2) url = QUrl(QString::fromUtf8(value));
			if (field == 5) uri = QString::fromUtf8(value);
		}
		if (uri == trackUri && Https(url)
			&& url.path().endsWith(u".mp4"_q, Qt::CaseInsensitive)) {
			return url;
		}
	}
	return {};
}

bool MatchesTrack(
		const QString &title,
		const QString &artist,
		qint64 duration,
		const QString &candidateTitle,
		const QString &candidateArtist,
		qint64 candidateDuration) {
	const auto a = Normalize(artist);
	const auto b = Normalize(candidateArtist);
	return !a.isEmpty() && !b.isEmpty()
		&& (a == b || (a.size() >= 4 && b.contains(a)))
		&& MatchesTitle(title, candidateTitle)
		&& duration > 0 && candidateDuration > 0
		&& std::abs(duration - candidateDuration) <= 5000;
}

} // namespace Mokagram::Music::MotionProtocol
