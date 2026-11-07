// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 9ee9cde3
#include "mokagram/misc/mokagram_datereg.h"
#include "mokagram/lang/mokagram_lang.h"

#include "mokagram/settings/mokagram_settings.h"

#include "base/unixtime.h"
#include "data/data_changes.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "main/main_session.h"

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

#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace Mokagram::Misc {
namespace {


// Ключ DateReg в исходниках больше не хранится: он передаётся при сборке
// через макрос MOKAGRAM_DATEREG_KEY (см. Telegram/CMakeLists.txt — файл
// build/keys/datereg.key или переменная окружения MOKAGRAM_DATEREG_KEY).
// Пустой ключ отключает функцию: DateRegEnabled() вернёт false.
#ifndef MOKAGRAM_DATEREG_KEY
#define MOKAGRAM_DATEREG_KEY ""
#endif

QString Key() {
	return QString::fromUtf8(MOKAGRAM_DATEREG_KEY);
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
	// Без ключа API бесполезен: не ходим в сеть с пустым токеном.
	if (Key().isEmpty() || !Pending().insert(userId).second) {
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

struct Anchor {
	quint64 id = 0;
	TimeId date = 0;
};

// Опорные точки «айди → время регистрации», отсортированы по айди.
constexpr auto kAnchors = std::array<Anchor, 150>{{
	{ 217509ULL, 1382662080 }, { 2768400ULL, 1383242400 }, { 2768409ULL, 1383264000 }, { 6256671ULL, 1390013486 },
	{ 11538510ULL, 1391104800 }, { 11538514ULL, 1391212000 }, { 15835240ULL, 1392832800 }, { 15835244ULL, 1392940000 },
	{ 23646070ULL, 1393351200 }, { 23646077ULL, 1393459000 }, { 38015510ULL, 1393632000 }, { 44634660ULL, 1396720800 },
	{ 45641754ULL, 1398167867 }, { 46145305ULL, 1400198000 }, { 54845230ULL, 1403287200 }, { 60457419ULL, 1408075000 },
	{ 63263518ULL, 1414454000 }, { 72776425ULL, 1427835600 }, { 95308423ULL, 1428238000 }, { 103204956ULL, 1433032767 },
	{ 110306836ULL, 1434314267 }, { 115831565ULL, 1438524943 }, { 124872440ULL, 1439838000 }, { 124872445ULL, 1439856000 },
	{ 127929226ULL, 1442623033 }, { 133909600ULL, 1444158000 }, { 133909606ULL, 1444176000 }, { 139646974ULL, 1444273680 },
	{ 143445120ULL, 1448910000 }, { 143445125ULL, 1448928000 }, { 155983984ULL, 1449370973 }, { 172144707ULL, 1454045640 },
	{ 188459175ULL, 1457850160 }, { 222021230ULL, 1465326000 }, { 222021233ULL, 1465344000 }, { 225034350ULL, 1466190000 },
	{ 225034354ULL, 1466208000 }, { 267103082ULL, 1472677200 }, { 279333717ULL, 1473142920 }, { 281973858ULL, 1475301600 },
	{ 285253070ULL, 1476817200 }, { 285253072ULL, 1476835000 }, { 294851030ULL, 1479582000 }, { 294851037ULL, 1479600000 },
	{ 297621220ULL, 1481828400 }, { 297621225ULL, 1481846000 }, { 310042805ULL, 1487106000 }, { 328122828ULL, 1487205960 },
	{ 339965214ULL, 1489772244 }, { 369669040ULL, 1490814000 }, { 369669043ULL, 1490918000 }, { 399597034ULL, 1496257200 },
	{ 400169470ULL, 1501441200 }, { 400169472ULL, 1501459000 }, { 409518943ULL, 1506848400 }, { 484976017ULL, 1516084200 },
	{ 501535268ULL, 1518638400 }, { 554990133ULL, 1521187200 }, { 573604341ULL, 1523822400 }, { 601341900ULL, 1531829829 },
	{ 655730151ULL, 1537472400 }, { 675879664ULL, 1539510300 }, { 695531082ULL, 1541019600 }, { 707751101ULL, 1542304800 },
	{ 717887953ULL, 1545429600 }, { 774723749ULL, 1550821640 }, { 820050353ULL, 1557302400 }, { 871177218ULL, 1561951200 },
	{ 920778234ULL, 1568803164 }, { 970269006ULL, 1574693313 }, { 1020178246ULL, 1576957200 }, { 1040151903ULL, 1577817000 },
	{ 1059845356ULL, 1578848886 }, { 1081554131ULL, 1580504400 }, { 1130489378ULL, 1583002800 }, { 1145856008ULL, 1586342040 },
	{ 1170070589ULL, 1588284000 }, { 1202891775ULL, 1588310640 }, { 1232810985ULL, 1590890220 }, { 1279382482ULL, 1590934320 },
	{ 1334421204ULL, 1597360114 }, { 1382531194ULL, 1600188120 }, { 1383895297ULL, 1601492400 }, { 1415571066ULL, 1604817900 },
	{ 1439824385ULL, 1606762800 }, { 1471194210ULL, 1606813200 }, { 1485680298ULL, 1609437600 }, { 1514538817ULL, 1609441200 },
	{ 1578805533ULL, 1612119600 }, { 1649902679ULL, 1612670965 }, { 1692464211ULL, 1615402500 }, { 1735271901ULL, 1618786994 },
	{ 1788462219ULL, 1622458800 }, { 1824157837ULL, 1622576984 }, { 1881430862ULL, 1623440640 }, { 1938921569ULL, 1626933294 },
	{ 1991158335ULL, 1630556060 }, { 2030606431ULL, 1631992680 }, { 2250645697ULL, 1633515812 }, { 5017595749ULL, 1638309600 },
	{ 5035057026ULL, 1640352847 }, { 5065900297ULL, 1640977200 }, { 5124099423ULL, 1643662800 }, { 5199399786ULL, 1645002097 },
	{ 5249255238ULL, 1646383782 }, { 5290311052ULL, 1647451401 }, { 5297354132ULL, 1648760400 }, { 5318972588ULL, 1651937341 },
	{ 5362593868ULL, 1652024520 }, { 5396575599ULL, 1654553525 }, { 5402540458ULL, 1656615600 }, { 5502182459ULL, 1658314837 },
	{ 5562600534ULL, 1659301200 }, { 5584365163ULL, 1659366750 }, { 5608562550ULL, 1664012820 }, { 5644144970ULL, 1664703130 },
	{ 5731896020ULL, 1667242122 }, { 5778063231ULL, 1667477640 }, { 5821105646ULL, 1671677863 }, { 5844821188ULL, 1672506000 },
	{ 5856660622ULL, 1673991930 }, { 5903848700ULL, 1675280531 }, { 5983654523ULL, 1681223063 }, { 6025903944ULL, 1681680600 },
	{ 6115110768ULL, 1683830609 }, { 6297860552ULL, 1688115560 }, { 6373693377ULL, 1692301478 }, { 6433264186ULL, 1692763697 },
	{ 6507968649ULL, 1695301080 }, { 6609923725ULL, 1696100400 }, { 6639644540ULL, 1698760800 }, { 6845043276ULL, 1704907301 },
	{ 7043862629ULL, 1711876800 }, { 7129689557ULL, 1715623200 }, { 7191651807ULL, 1721136600 }, { 7255466649ULL, 1724214000 },
	{ 7347120414ULL, 1724655600 }, { 7424559477ULL, 1727733600 }, { 7490725809ULL, 1729065600 }, { 7509628470ULL, 1732991400 },
	{ 7519637470ULL, 1734202800 }, { 7537361651ULL, 1735686000 }, { 7552093513ULL, 1738224000 }, { 7716779743ULL, 1739042100 },
	{ 7967849270ULL, 1743318900 }, { 8096580061ULL, 1745982867 }, { 8218310808ULL, 1758394800 }, { 8379482677ULL, 1759605750 },
	{ 8541347308ULL, 1763236800 }, { 8587904527ULL, 1764543600 }
}};

enum class Bound {
	Approx,
	NotBefore,
	NotAfter,
};

struct Estimate {
	TimeId date = 0;
	Bound bound = Bound::Approx;
};

[[nodiscard]] Estimate EstimateByAnchors(quint64 id) {
	const auto first = kAnchors.front();
	const auto last = kAnchors.back();
	if (id < first.id) {
		return { first.date, Bound::NotAfter };
	} else if (id > last.id) {
		return { last.date, Bound::NotBefore };
	}
	const auto after = std::lower_bound(
		kAnchors.begin(),
		kAnchors.end(),
		id,
		[](const Anchor &anchor, quint64 value) {
			return anchor.id < value;
		});
	if (after->id == id || after == kAnchors.begin()) {
		return { after->date, Bound::Approx };
	}
	const auto before = after - 1;
	const auto part = double(id - before->id) / double(after->id - before->id);
	return {
		TimeId(before->date + part * (after->date - before->date)),
		Bound::Approx,
	};
}

[[nodiscard]] QString MonthYear(TimeId date, bool genitive) {
	static const auto nominative = QStringList{
		QString::fromUtf8(Mokagram::Lang::Tr("январь")), QString::fromUtf8(Mokagram::Lang::Tr("февраль")),
		QString::fromUtf8(Mokagram::Lang::Tr("март")), QString::fromUtf8(Mokagram::Lang::Tr("апрель")),
		QString::fromUtf8(Mokagram::Lang::Tr("май")), QString::fromUtf8(Mokagram::Lang::Tr("июнь")),
		QString::fromUtf8(Mokagram::Lang::Tr("июль")), QString::fromUtf8(Mokagram::Lang::Tr("август")),
		QString::fromUtf8(Mokagram::Lang::Tr("сентябрь")), QString::fromUtf8(Mokagram::Lang::Tr("октябрь")),
		QString::fromUtf8(Mokagram::Lang::Tr("ноябрь")), QString::fromUtf8(Mokagram::Lang::Tr("декабрь")),
	};
	static const auto genitiveNames = QStringList{
		QString::fromUtf8(Mokagram::Lang::Tr("января")), QString::fromUtf8(Mokagram::Lang::Tr("февраля")),
		QString::fromUtf8(Mokagram::Lang::Tr("марта")), QString::fromUtf8(Mokagram::Lang::Tr("апреля")),
		QString::fromUtf8(Mokagram::Lang::Tr("мая")), QString::fromUtf8(Mokagram::Lang::Tr("июня")),
		QString::fromUtf8(Mokagram::Lang::Tr("июля")), QString::fromUtf8(Mokagram::Lang::Tr("августа")),
		QString::fromUtf8(Mokagram::Lang::Tr("сентября")), QString::fromUtf8(Mokagram::Lang::Tr("октября")),
		QString::fromUtf8(Mokagram::Lang::Tr("ноября")), QString::fromUtf8(Mokagram::Lang::Tr("декабря")),
	};
	const auto when = base::unixtime::parse(date).date();
	const auto &names = genitive ? genitiveNames : nominative;
	return names[when.month() - 1] + ' ' + QString::number(when.year());
}

[[nodiscard]] TimeId EarliestKnownPhoto(not_null<PeerData*> peer) {
	const auto id = peer->userpicPhotoId();
	return id ? peer->owner().photo(id)->date() : TimeId();
}

} // namespace

QString RegdateText(not_null<PeerData*> peer) {
	if (!Settings::Bool(u"regdate.show"_q)) {
		return QString();
	}
	const auto isUser = peer->isUser();
	const auto id = isUser
		? peerToUser(peer->id).bare
		: peer->isChannel()
		? peerToChannel(peer->id).bare
		: peerToChat(peer->id).bare;
	auto estimate = EstimateByAnchors(uint64(id));
	if (isUser) {
		const auto photo = EarliestKnownPhoto(peer);
		if (photo > 0 && photo < estimate.date) {
			estimate = { photo, Bound::NotAfter };
		}
	} else {
		const auto next = std::upper_bound(
			kAnchors.begin(),
			kAnchors.end(),
			uint64(id),
			[](quint64 value, const Anchor &anchor) {
				return value < anchor.id;
			});
		estimate = {
			(next == kAnchors.end() ? kAnchors.back() : *next).date,
			Bound::Approx,
		};
	}
	switch (estimate.bound) {
	case Bound::NotBefore:
		return QString::fromUtf8(Mokagram::Lang::Tr("не раньше "))
			+ MonthYear(estimate.date, true);
	case Bound::NotAfter:
		return QString::fromUtf8(Mokagram::Lang::Tr("не позднее "))
			+ MonthYear(estimate.date, true);
	case Bound::Approx:
		break;
	}
	return QString::fromUtf8(Mokagram::Lang::Tr("примерно "))
		+ MonthYear(estimate.date, false);
}

rpl::producer<QString> RegdateValue(not_null<PeerData*> peer) {
	return peer->session().changes().peerFlagsValue(
		peer,
		Data::PeerUpdate::Flag::Photo
	) | rpl::map([=] {
		return RegdateText(peer);
	});
}

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
