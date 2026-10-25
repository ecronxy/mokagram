#include "mokagram/stars/mokagram_stars_stats.h"

#include "api/api_credits.h"
#include "core/application.h"
#include "data/data_credits.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/effects/credits_graphics.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/vertical_list.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_basic.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDate>
#include <QtCore/QLocale>

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

namespace Mokagram::Stars {
namespace {

constexpr auto kMaxRequests = 60;
constexpr auto kMaxEntries = 6000;
constexpr auto kMonths = 12;

struct Stats {
	std::vector<Data::CreditsHistoryEntry> list;
	std::vector<Data::CreditsHistoryEntry> refunds;
	bool done = false;
	bool failed = false;
};

[[nodiscard]] QString U(const char *text) {
	return QString::fromUtf8(text);
}

[[nodiscard]] QString Category(const Data::CreditsHistoryEntry &e) {
	if (e.gift || e.stargift) {
		return U("Подарки");
	} else if (!e.extended.empty()) {
		return U("Платные медиа");
	} else if (e.reaction) {
		return U("Реакции");
	} else if (e.paidMessagesCount) {
		return U("Платные сообщения");
	} else if (!e.subscriptionUntil.isNull()) {
		return U("Подписки");
	} else if (e.peerType == Data::CreditsHistoryEntry::PeerType::Ads) {
		return U("Реклама");
	}
	return U("Боты и прочее");
}

[[nodiscard]] QString Number(qint64 value) {
	return QLocale(QLocale::Russian).toString(value);
}

[[nodiscard]] QString ShortMonth(int month) {
	static const auto names = std::array<const char*, 12>{
		"янв", "фев", "мар", "апр", "май", "июн",
		"июл", "авг", "сен", "окт", "ноя", "дек" };
	return U(names[std::clamp(month, 1, 12) - 1]);
}

[[nodiscard]] bool Counts(const Data::CreditsHistoryEntry &e) {
	return !e.in && !e.failed && !e.pending && !e.refunded;
}

[[nodiscard]] qint64 Spent(const Data::CreditsHistoryEntry &e) {
	return std::abs(qint64(e.credits.whole()));
}

void LoadDirection(
		not_null<Main::Session*> session,
		bool incoming,
		std::shared_ptr<Stats> stats,
		Fn<void()> changed,
		Fn<void()> finished) {
	const auto api = std::shared_ptr<Api::CreditsHistory>(
		new Api::CreditsHistory(session->user(), incoming, !incoming));
	const auto requests = std::make_shared<int>(0);
	const auto step = std::make_shared<Fn<void(QString)>>();
	*step = [=](QString token) {
		api->request(token, [=](Data::CreditsStatusSlice slice) {
			auto &target = incoming ? stats->refunds : stats->list;
			for (auto &entry : slice.list) {
				if (!incoming || entry.refunded) {
					target.push_back(std::move(entry));
				}
			}
			++*requests;
			if (slice.allLoaded
				|| slice.token.isEmpty()
				|| *requests >= kMaxRequests
				|| int(target.size()) >= kMaxEntries) {
				finished();
				return;
			}
			changed();
			(*step)(slice.token);
		}, 100);
	};
	(*step)(QString());
}

// Сначала траты, потом возвраты: вернувшиеся звёзды (отклонённый оффер,
// проигранная ставка) тратой не считаем.
void Load(
		not_null<Main::Session*> session,
		std::shared_ptr<Stats> stats,
		Fn<void()> changed) {
	LoadDirection(session, false, stats, changed, [=] {
		LoadDirection(session, true, stats, changed, [=] {
			stats->done = true;
			changed();
		});
	});
}

class Chart final : public Ui::RpWidget {
public:
	Chart(
		QWidget *parent,
		std::vector<std::pair<QString, qint64>> bars)
	: RpWidget(parent)
	, _bars(std::move(bars)) {
		resize(width(), 150);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto n = int(_bars.size());
		if (!n) {
			return;
		}
		auto top = qint64(1);
		for (const auto &b : _bars) {
			top = std::max(top, b.second);
		}
		const auto labelH = st::normalFont->height + 4;
		const auto area = QRect(0, 18, width(), height() - labelH - 18);
		const auto slot = width() / double(n);
		const auto barW = std::max(4., slot * 0.56);
		p.setFont(st::normalFont);
		for (auto i = 0; i != n; ++i) {
			const auto h = int(area.height() * (_bars[i].second / double(top)));
			const auto x = slot * i + (slot - barW) / 2;
			p.setPen(Qt::NoPen);
			p.setBrush(_bars[i].second ? st::windowBgActive : st::windowBgOver);
			const auto rect = QRectF(x, area.bottom() - std::max(h, 3), barW, std::max(h, 3));
			p.drawRoundedRect(rect, 5, 5);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				QRectF(slot * i, height() - labelH, slot, labelH),
				Qt::AlignCenter,
				_bars[i].first);
			if (_bars[i].second == top) {
				p.setPen(st::windowFg);
				p.drawText(
					QRectF(slot * i - 20, 0, slot + 40, 16),
					Qt::AlignCenter,
					Number(top));
			}
		}
	}

private:
	std::vector<std::pair<QString, qint64>> _bars;

};

void Fill(
		not_null<Ui::VerticalLayout*> content,
		not_null<Main::Session*> session,
		const Stats &stats) {
	auto total = qint64(0);
	auto lastMonth = qint64(0);
	auto toSelf = qint64(0);
	auto byCategory = std::map<QString, qint64>();
	auto byPeer = std::map<uint64, qint64>();
	auto byMonth = std::map<int, qint64>();
	const auto now = QDate::currentDate();
	const auto key = [](QDate d) { return d.year() * 12 + d.month() - 1; };
	const auto nowKey = key(now);
	auto refundCount = std::map<std::pair<uint64, qint64>, int>();
	for (const auto &r : stats.refunds) {
		++refundCount[{ r.barePeerId, Spent(r) }];
	}
	auto ordered = std::vector<const Data::CreditsHistoryEntry*>();
	for (const auto &e : stats.list) {
		ordered.push_back(&e);
	}
	// Возврат гасит ближайшую по времени трату той же суммы тому же получателю.
	std::sort(ordered.begin(), ordered.end(), [](auto a, auto b) {
		return a->date > b->date;
	});
	for (const auto entry : ordered) {
		const auto &e = *entry;
		if (!Counts(e)) {
			continue;
		}
		const auto amount = Spent(e);
		auto i = refundCount.find({ e.barePeerId, amount });
		if (i != refundCount.end() && i->second > 0) {
			--i->second;
			continue;
		}
		total += amount;
		byCategory[Category(e)] += amount;
		if (e.barePeerId == session->userId().bare) {
			toSelf += amount;
		}
		if (e.barePeerId && e.peerType == Data::CreditsHistoryEntry::PeerType::Peer) {
			byPeer[e.barePeerId] += amount;
		}
		const auto k = key(e.date.date());
		byMonth[k] += amount;
		if (e.date.date().daysTo(now) <= 30) {
			lastMonth += amount;
		}
	}

	const auto head = [&](const QString &text) {
		Ui::AddSkip(content);
		Ui::AddSubsectionTitle(content, rpl::single(text));
	};
	const auto line = [&](const QString &left, const QString &right, bool star = false) {
		const auto row = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(24, 3, 24, 3));
		row->resize(row->width(), st::boxLabel.style.font->height + 4);
		const auto l = Ui::CreateChild<Ui::FlatLabel>(row, left, st::boxLabel);
		const auto r = Ui::CreateChild<Ui::FlatLabel>(row, right, st::boxLabel);
		l->show();
		r->show();
		const auto icon = star ? Ui::CreateSingleStarWidget(row, st::boxLabel.style.font->height).get() : static_cast<Ui::RpWidget*>(nullptr);
		if (icon) {
			icon->show();
		}
		row->widthValue() | rpl::on_next([=](int w) {
			const auto extra = icon ? icon->width() + 4 : 0;
			l->resizeToNaturalWidth(w - r->naturalWidth() - extra - 12);
			l->moveToLeft(0, 0);
			r->moveToRight(0, 0);
			if (icon) {
				icon->moveToRight(r->naturalWidth() + 4, 0);
			}
		}, row->lifetime());
	};

	head(U("Итого потрачено"));
	line(U("За всё время"), Number(total), true);
	line(U("За 30 дней"), Number(lastMonth), true);
	line(U("Из них себе (подарки и покупки)"), Number(toSelf), true);
	if (!stats.done) {
		line(U("Загружено операций"), Number(int(stats.list.size())) + U("…"));
	}

	head(U("По месяцам"));
	auto bars = std::vector<std::pair<QString, qint64>>();
	for (auto i = kMonths - 1; i >= 0; --i) {
		const auto k = nowKey - i;
		bars.emplace_back(ShortMonth(k % 12 + 1), byMonth[k]);
	}
	content->add(
		object_ptr<Chart>(content, std::move(bars)),
		QMargins(24, 6, 24, 6));

	head(U("По категориям"));
	auto cats = std::vector<std::pair<QString, qint64>>(
		byCategory.begin(), byCategory.end());
	std::sort(cats.begin(), cats.end(), [](auto &a, auto &b) {
		return a.second > b.second;
	});
	for (const auto &c : cats) {
		line(c.first, Number(c.second), true);
	}
	if (cats.empty()) {
		line(U("Трат пока нет"), QString());
	}

	head(U("Кому больше всего"));
	auto peers = std::vector<std::pair<uint64, qint64>>(
		byPeer.begin(), byPeer.end());
	std::sort(peers.begin(), peers.end(), [](auto &a, auto &b) {
		return a.second > b.second;
	});
	if (peers.size() > 7) {
		peers.resize(7);
	}
	for (const auto &p : peers) {
		auto name = QString();
		for (const auto id : {
			peerFromUser(UserId(p.first)),
			peerFromChannel(ChannelId(p.first)) }) {
			if (const auto peer = session->data().peerLoaded(id)) {
				name = peer->name();
				if (name.isEmpty()) {
					name = U("Удалённый аккаунт");
				}
				break;
			}
		}
		if (name.isEmpty()) {
			name = U("ID ") + QString::number(p.first);
		}
		line(name, Number(p.second), true);
	}
	Ui::AddSkip(content);
}

} // namespace

void ShowSpendingStatsBox() {
	const auto window = Core::App().activeWindow();
	const auto controller = window ? window->sessionController() : nullptr;
	if (!controller) {
		return;
	}
	const auto session = &controller->session();
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(U("Траты звёзд")));
		box->setWidth(440);
		const auto stats = std::make_shared<Stats>();
		const auto holder = box->verticalLayout();
		const auto render = [=] {
			holder->clear();
			Fill(holder, session, *stats);
			holder->resizeToWidth(holder->width());
		};
		const auto refresh = crl::guard(box, render);
		Load(session, stats, refresh);
		box->addButton(
			rpl::single(U("Закрыть")),
			[=] { box->closeBox(); });
		render();
	}));
}

} // namespace Mokagram::Stars
