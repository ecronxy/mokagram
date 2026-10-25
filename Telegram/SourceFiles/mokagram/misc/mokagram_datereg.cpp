#include "mokagram/misc/mokagram_datereg.h"
#include "mokagram/lang/mokagram_lang.h"

#include "mokagram/settings/mokagram_settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <rpl/rpl.h>

#include <map>
#include <set>

namespace Mokagram::Misc {
namespace {


#if __has_include("mokagram/misc/mokagram_datereg_key.h")
#include "mokagram/misc/mokagram_datereg_key.h"
#else
constexpr auto kMokagramDateRegDefaultKey = "";
#endif

QString Key() {
	return QString::fromUtf8(kMokagramDateRegDefaultKey);
}

QNetworkAccessManager &Net() {
	static auto manager = new QNetworkAccessManager();
	return *manager;
}

QString CachePath() {
	return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
		+ u"/mokagram_datereg.json"_q;
}

std::map<quint64, QString> &Cache() {
	static auto result = [] {
		auto map = std::map<quint64, QString>();
		auto file = QFile(CachePath());
		if (file.open(QIODevice::ReadOnly)) {
			const auto object = QJsonDocument::fromJson(file.readAll()).object();
			for (auto i = object.begin(); i != object.end(); ++i) {
				map[i.key().toULongLong()] = i.value().toString();
			}
		}
		return map;
	}();
	return result;
}

void SaveCache() {
	auto object = QJsonObject();
	for (const auto &[id, text] : Cache()) {
		object.insert(QString::number(id), text);
	}
	QDir().mkpath(QFileInfo(CachePath()).absolutePath());
	auto file = QFile(CachePath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
	}
}

// "1.2024" -> "январь 2024".
QString Pretty(const QString &raw) {
	static const auto months = QStringList{
		QString::fromUtf8(Mokagram::Lang::Tr("январь")), QString::fromUtf8(Mokagram::Lang::Tr("февраль")),
		QString::fromUtf8(Mokagram::Lang::Tr("март")), QString::fromUtf8(Mokagram::Lang::Tr("апрель")),
		QString::fromUtf8(Mokagram::Lang::Tr("май")), QString::fromUtf8(Mokagram::Lang::Tr("июнь")),
		QString::fromUtf8(Mokagram::Lang::Tr("июль")), QString::fromUtf8(Mokagram::Lang::Tr("август")),
		QString::fromUtf8(Mokagram::Lang::Tr("сентябрь")), QString::fromUtf8(Mokagram::Lang::Tr("октябрь")),
		QString::fromUtf8(Mokagram::Lang::Tr("ноябрь")), QString::fromUtf8(Mokagram::Lang::Tr("декабрь")),
	};
	const auto parts = raw.split('.');
	if (parts.size() == 2) {
		const auto month = parts[0].toInt();
		if (month >= 1 && month <= 12) {
			return months[month - 1] + ' ' + parts[1];
		}
	}
	return raw;
}

rpl::event_stream<quint64> &Updates() {
	static auto result = rpl::event_stream<quint64>();
	return result;
}

std::map<quint64, QString> &Errors() {
	static auto result = std::map<quint64, QString>();
	return result;
}

std::set<quint64> &Pending() {
	static auto result = std::set<quint64>();
	return result;
}

void Fetch(quint64 userId);
void Fetch(quint64 userId) {
	if (!Pending().insert(userId).second) {
		return;
	}
	auto url = QUrl(u"https://api.datereg.pro/api/v1/users/getCreationDateSmart"_q);
	auto query = QUrlQuery();
	query.addQueryItem(u"token"_q, Key());
	query.addQueryItem(u"user_id"_q, QString::number(userId));
	url.setQuery(query);
	const auto reply = Net().get(QNetworkRequest(url));
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		Pending().erase(userId);
		const auto body = reply->readAll();
		const auto object = QJsonDocument::fromJson(body).object();
		if (reply->error() != QNetworkReply::NoError) {
			const auto detail = object.value("detail").toString();
			static auto retried = std::set<quint64>();
			if (!detail.contains(u"balance"_q, Qt::CaseInsensitive)
				&& retried.insert(userId).second) {
				// Временный сбой сети: пробуем ещё раз, не запоминая ошибку.
				QTimer::singleShot(1500, [=] { Fetch(userId); });
				return;
			}
			retried.erase(userId);
			Errors()[userId] = detail.contains(u"balance"_q, Qt::CaseInsensitive)
				? QString::fromUtf8(Mokagram::Lang::Tr("пополните баланс API"))
				: QString::fromUtf8(Mokagram::Lang::Tr("не получилось узнать"));
			Updates().fire_copy(userId);
			return;
		}
		const auto date = object.value("creation_date").toString();
		if (date.isEmpty()) {
			return;
		}
		Cache()[userId] = Pretty(date);
		SaveCache();
		Updates().fire_copy(userId);
	});
}

} // namespace

bool DateRegEnabled() {
	return !Key().isEmpty();
}

void Prefetch(quint64 userId) {
	if (!DateRegEnabled()
		|| Cache().find(userId) != Cache().end()
		|| Errors().find(userId) != Errors().end()) {
		return;
	}
	Fetch(userId);
}

rpl::producer<QString> RegistrationDate(quint64 userId) {
	return [=](auto consumer) {
		auto lifetime = rpl::lifetime();
		const auto current = [=] {
			const auto i = Cache().find(userId);
			if (i != Cache().end()) {
				return i->second;
			}
			const auto e = Errors().find(userId);
			return (e != Errors().end()) ? e->second : QString();
		};
		const auto found = current();
		consumer.put_next_copy(found.isEmpty()
			? QString::fromUtf8("…")
			: found);
		if (Cache().find(userId) == Cache().end()
			&& Errors().find(userId) == Errors().end()) {
			Fetch(userId);
		}
		Updates().events(
		) | rpl::filter([=](quint64 id) {
			return id == userId;
		}) | rpl::on_next([=](quint64) {
			consumer.put_next_copy(current());
		}, lifetime);
		return lifetime;
	};
}

} // namespace Mokagram::Misc
