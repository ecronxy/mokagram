#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <optional>

namespace Mokagram::Plugins::Sources {

struct Entry {
	QString id;
	QString name;
	QString version;
	QUrl url;
	QByteArray sha256;
	qint64 size = 0;
};

[[nodiscard]] inline bool IsSecure(const QUrl &url) {
	return url.isValid()
		&& url.scheme() == QString::fromLatin1("https")
		&& !url.host().isEmpty()
		&& url.userInfo().isEmpty();
}

[[nodiscard]] inline std::optional<Entry> ParseEntry(
		const QJsonObject &item,
		const QUrl &base,
		qint64 maxBytes) {
	const auto hash = item.value(QString::fromLatin1("sha256")).toString();
	static const auto pattern = QRegularExpression(
		QString::fromLatin1("^[A-Fa-f0-9]{64}$"));
	const auto length = item.value(QString::fromLatin1("size")).toDouble();
	auto entry = Entry{
		.id = item.value(QString::fromLatin1("id")).toString(),
		.name = item.value(QString::fromLatin1("name")).toString(),
		.version = item.value(QString::fromLatin1("version")).toString(),
		.url = base.resolved(QUrl(item.value(QString::fromLatin1("file")).toString())),
		.sha256 = QByteArray::fromHex(hash.toLatin1()),
	};
	if (entry.id.isEmpty() || entry.name.isEmpty() || entry.version.isEmpty()
		|| !IsSecure(entry.url)
		|| !entry.url.path().endsWith(QString::fromLatin1(".plugin"), Qt::CaseInsensitive)
		|| !pattern.match(hash).hasMatch()
		|| !(length > 0 && length <= double(maxBytes))
		|| length != double(qint64(length))) {
		return std::nullopt;
	}
	entry.size = qint64(length);
	return entry;
}

[[nodiscard]] inline bool VerifyDownload(
		const QByteArray &bytes,
		const QByteArray &expectedHash,
		qint64 expectedSize,
		qint64 maxBytes) {
	return !bytes.isEmpty()
		&& bytes.size() <= maxBytes
		&& (expectedHash.isEmpty()
			|| (expectedHash.size() == 32 && bytes.size() == expectedSize
				&& QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) == expectedHash));
}

} // namespace Mokagram::Plugins::Sources
