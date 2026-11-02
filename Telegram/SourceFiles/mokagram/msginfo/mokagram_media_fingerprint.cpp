#include "mokagram/msginfo/mokagram_media_fingerprint.h"

#include <QtCore/QIODevice>
#include <QtCore/QRegularExpression>
#include <QtCore/QtEndian>
#include <QtCore/QStringList>
#include <algorithm>

namespace Mokagram::MessageInfo {
namespace {

constexpr auto kMaxOggPrefix = 256 * 1024;
constexpr auto kMaxPacket = 65536;

quint32 Little32(const QByteArray &data, qsizetype at) {
	return qFromLittleEndian<quint32>(data.constData() + at);
}

quint32 Big32(const QByteArray &data, qsizetype at) {
	return qFromBigEndian<quint32>(data.constData() + at);
}

QString VoiceVendor(const QByteArray &bytes) {
	auto packet = QByteArray();
	auto offset = qsizetype(0);
	auto serial = quint32(0);
	auto sequence = quint32(0);
	auto head = false;
	auto continued = false;
	const auto limit = std::min(qsizetype(kMaxOggPrefix), bytes.size());
	while (offset + 27 <= limit) {
		if (bytes.mid(offset, 4) != "OggS" || bytes[offset + 4] != 0) return {};
		const auto flags = quint8(bytes[offset + 5]);
		const auto pageSerial = Little32(bytes, offset + 14);
		const auto pageSequence = Little32(bytes, offset + 18);
		if ((flags & ~7) || bool(flags & 1) != continued) return {};
		if (!offset) {
			if (flags != 2 || pageSequence != 0) return {};
			serial = pageSerial;
		} else if ((flags & 2) || pageSerial != serial || pageSequence != sequence) {
			return {};
		}
		sequence = pageSequence + 1;
		const auto count = quint8(bytes[offset + 26]);
		const auto table = offset + 27;
		offset = table + count;
		if (offset > limit) return {};
		for (auto i = 0; i < count; ++i) {
			const auto length = quint8(bytes[table + i]);
			if (offset + length > limit || packet.size() + length > kMaxPacket) return {};
			packet.append(bytes.constData() + offset, length);
			offset += length;
			continued = length == 255;
			if (continued) continue;
			if (!head) {
				if (packet.size() < 19 || !packet.startsWith("OpusHead")
					|| quint8(packet[8]) > 15 || !packet[9]) return {};
				head = true;
				packet.clear();
				continue;
			}
			if (packet.size() < 16 || !packet.startsWith("OpusTags")) return {};
			const auto vendorSize = Little32(packet, 8);
			if (vendorSize > 512 || vendorSize > quint32(packet.size() - 16)) return {};
			auto position = qsizetype(12 + vendorSize);
			const auto comments = Little32(packet, position);
			position += 4;
			if (comments > quint32((packet.size() - position) / 4)) return {};
			for (auto c = quint32(0); c < comments; ++c) {
				if (position + 4 > packet.size()) return {};
				const auto size = Little32(packet, position);
				position += 4;
				if (size > quint32(packet.size() - position)) return {};
				position += size;
			}
			return QString::fromUtf8(packet.mid(12, vendorSize));
		}
		if (flags & 4) return {};
	}
	return {};
}

struct Track {
	QByteArray type;
	QString name;
	QString language;
	bool media = false;
};

struct Mp4 {
	QByteArray major;
	quint32 minor = 0;
	QList<QByteArray> brands;
	Track video;
	Track audio;
	QString encoder;
	bool hasEncoder = false;
	bool movie = false;
	bool fragmented = false;
	int boxes = 0;
};

bool ReadBoxes(QIODevice &file, qint64 end, const QByteArray &parent,
		Track *track, Mp4 &result, int depth) {
	if (depth > 12) return false;
	while (file.pos() < end) {
		const auto start = file.pos();
		if (end - start < 8 || ++result.boxes > 4096) return false;
		const auto header = file.read(8);
		if (header.size() != 8) return false;
		auto size = quint64(Big32(header, 0));
		const auto type = header.mid(4, 4);
		auto headerSize = qint64(8);
		if (size == 1) {
			const auto extended = file.read(8);
			if (end - start < 16 || extended.size() != 8) return false;
			size = qFromBigEndian<quint64>(extended.constData());
			headerSize = 16;
		} else if (!size) size = end - start;
		if (size < quint64(headerSize) || size > quint64(end - start)) return false;
		const auto till = start + qint64(size);
		const auto payload = qint64(size) - headerSize;
		if (type == "ftyp" && parent.isEmpty()) {
			if (!result.major.isEmpty() || payload < 8 || payload > 256 || payload % 4) return false;
			const auto bytes = file.read(payload);
			if (bytes.size() != payload) return false;
			result.major = bytes.left(4);
			result.minor = Big32(bytes, 4);
			for (auto at = 8; at < bytes.size(); at += 4) result.brands.push_back(bytes.mid(at, 4));
		} else if (type == "moov" && parent.isEmpty()) {
			if (result.movie) return false;
			result.movie = true;
			if (!ReadBoxes(file, till, type, nullptr, result, depth + 1)) return false;
		} else if (type == "trak" && parent == "moov") {
			auto parsed = Track();
			if (!ReadBoxes(file, till, type, &parsed, result, depth + 1)) return false;
			if (parsed.type == "vide") {
				if (!result.video.type.isEmpty()) return false;
				result.video = std::move(parsed);
			} else if (parsed.type == "soun") {
				if (!result.audio.type.isEmpty()) return false;
				result.audio = std::move(parsed);
			}
		} else if (type == "mdia" && parent == "trak" && track) {
			if (track->media) return false;
			track->media = true;
			if (!ReadBoxes(file, till, type, track, result, depth + 1)) return false;
		} else if (type == "hdlr" && parent == "mdia" && track) {
			if (!track->type.isEmpty() || payload < 24 || payload > 256) return false;
			const auto bytes = file.read(payload);
			if (bytes.size() != payload || Big32(bytes, 0)) return false;
			track->type = bytes.mid(8, 4);
			auto name = bytes.mid(24);
			const auto zero = name.indexOf('\0');
			if (zero >= 0) name.truncate(zero);
			track->name = QString::fromUtf8(name);
		} else if (type == "mdhd" && parent == "mdia" && track) {
			if (!track->language.isNull() || payload < 24) return false;
			const auto version = file.read(4);
			if (version.size() != 4 || (Big32(version, 0) != 0 && Big32(version, 0) != 0x01000000)) return false;
			const auto offset = version[0] ? 32 : 20;
			if (payload < offset + 4 || !file.seek(start + headerSize + offset)) return false;
			const auto bytes = file.read(2);
			if (bytes.size() != 2) return false;
			const auto packed = qFromBigEndian<quint16>(bytes.constData());
			track->language = QString(QChar(((packed >> 10) & 31) + 96))
				+ QChar(((packed >> 5) & 31) + 96) + QChar((packed & 31) + 96);
		} else if ((type == "udta" && parent == "moov")
			|| (type == "ilst" && parent == "meta")
			|| (type == QByteArray::fromHex("a9746f6f") && parent == "ilst")) {
			if (!ReadBoxes(file, till, type, nullptr, result, depth + 1)) return false;
		} else if (type == "meta" && (parent == "udta" || parent == "moov")) {
			const auto fullbox = file.read(4);
			if (payload < 4 || fullbox.size() != 4 || Big32(fullbox, 0)
				|| !ReadBoxes(file, till, type, nullptr, result, depth + 1)) return false;
		} else if (type == "data" && parent == QByteArray::fromHex("a9746f6f")) {
			if (result.hasEncoder || payload < 8 || payload > 1024) return false;
			const auto bytes = file.read(payload);
			if (bytes.size() != payload || Big32(bytes, 0) != 1) return false;
			result.encoder = QString::fromUtf8(bytes.mid(8));
			result.hasEncoder = true;
		} else if (type == "mvex" || type == "moof") result.fragmented = true;
		if (!file.seek(till)) return false;
	}
	return file.pos() == end;
}

bool Brands(const QList<QByteArray> &actual, const QList<QByteArray> &expected) {
	return actual.size() == expected.size() && std::all_of(expected.begin(), expected.end(),
		[&](const QByteArray &brand) { return actual.contains(brand); });
}

} // namespace

QString VoicePlatform(const QByteArray &prefix) {
	const auto vendor = VoiceVendor(prefix);
	if (vendor == u"libopus unknown"_q) return u"Telegram X"_q;
	if (vendor == u"libopus 1.3.1-fixed"_q) return u"macOS"_q;
	if (vendor == u"telegram-web-a"_q) return u"Web A"_q;
	if (vendor == u"tweb"_q) return u"Web K"_q;
	if (vendor == u"libopus 1.5.1"_q) return u"iOS"_q;
	if (vendor == u"libopus unknown-fixed"_q) return u"Android"_q;
	return QRegularExpression(u"^Lavf[0-9]+\\.[0-9]+\\.[0-9]+$"_q).match(vendor).hasMatch()
		? u"Desktop"_q : QString();
}

QString RoundVideoPlatform(QIODevice &file) {
	if (!file.isOpen() || file.isSequential() || !file.seek(0) || file.size() < 8) return {};
	auto result = Mp4();
	if (!ReadBoxes(file, file.size(), {}, nullptr, result, 0)
		|| !result.movie || result.fragmented || result.video.type.isEmpty()
		|| result.audio.type.isEmpty()) return {};
	if (result.major == "isom" && result.minor == 512
		&& Brands(result.brands, { "isom", "iso2", "avc1", "mp41" })) {
		if (!result.hasEncoder && result.video.name == u"VideoHandle"_q
			&& result.audio.name == u"SoundHandle"_q && result.video.language == u"eng"_q
			&& result.audio.language == u"eng"_q) return u"Android"_q;
		if (QRegularExpression(u"^Lavf[0-9]+\\.[0-9]+\\.[0-9]+$"_q).match(result.encoder).hasMatch()
			&& result.video.name == u"VideoHandler"_q && result.audio.name == u"SoundHandler"_q
			&& result.video.language == u"und"_q && result.audio.language == u"und"_q) return u"Desktop"_q;
	}
	if (!result.hasEncoder && result.major == "mp42" && result.minor == 1
		&& Brands(result.brands, { "isom", "mp41", "mp42" })
		&& result.video.name == u"Core Media Video"_q && result.audio.name == u"Core Media Audio"_q
		&& result.video.language == u"und"_q && result.audio.language == u"und"_q) return u"iOS"_q;
	return {};
}

} // namespace Mokagram::MessageInfo
