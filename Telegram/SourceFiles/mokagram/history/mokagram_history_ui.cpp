// Mokagram: экраны истории (удалённые, правки, список чатов) и редактор фильтров.
#include "mokagram/history/mokagram_history.h"
#include <map>
#include <set>
#include "ui/boxes/confirm_box.h"
#include "ui/painter.h"
#include "mokagram/plugins/mokagram_plugin_install.h"
#include "base/unixtime.h"
#include "lang/lang_keys.h"

#include "mokagram/settings/mokagram_settings.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/platform/base_platform_info.h"
#include "data/data_thread.h"
#include "data/data_peer.h"
#include "data/data_channel.h"
#include "data/data_user.h"
#include "data/data_chat.h"
#include "history/history.h"
#include "history/history_item.h"
#include "ui/widgets/popup_menu.h"
#include "ui/text/text_entity.h"
#include "window/window_peer_menu.h"
#include "styles/style_menu_icons.h"
#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>
#include "data/data_session.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_dialogs.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtCore/QPointer>

#include <algorithm>

namespace Mokagram::History {
namespace {

constexpr auto kPageSize = 50;

[[nodiscard]] QString U(const char *text) {
	return QString::fromUtf8(text);
}

[[nodiscard]] QString PeerTitle(
		not_null<Main::Session*> session,
		uint64 id) {
	if (const auto peer = session->data().peerLoaded(PeerId(id))) {
		return peer->name();
	}
	return U("Чат ") + QString::number(id);
}

[[nodiscard]] QString FormatTime(int32 time) {
	return QDateTime::fromSecsSinceEpoch(time).toString(
		u"dd.MM.yyyy HH:mm"_q);
}

struct PageState {
	int32 cursorEvent = 0;
	int64 cursorId = 0;
	bool loading = false;
	bool finished = false;
};

struct HistoryFilter {
	int period = 0; // 0 все, 1 сутки, 2 неделя, 3 месяц.
	int type = 0;   // 0 все, 1 фото, 2 видео, 3 голосовые, 4 кружки, 5 файлы.
};

using Key = std::pair<uint64, int64>;

struct Selection {
	std::set<Key> picked;
	std::map<Key, Fn<void(bool)>> setters;
	std::map<Key, Fn<void()>> removers;

	void remove(const Key &key) {
		const auto i = removers.find(key);
		if (i != removers.end() && i->second) {
			i->second();
		}
	}
	rpl::event_stream<> changed;

	void set(const Key &key, bool on) {
		if (on) {
			picked.insert(key);
		} else {
			picked.erase(key);
		}
		changed.fire({});
	}
};

// Карточка в стиле окна «Плагины»: скруглённая подложка, поля, анимация скрытия.
struct CardParts {
	not_null<Ui::VerticalLayout*> card;
	Fn<void()> collapse;
};

[[nodiscard]] CardParts AddCard(not_null<Ui::VerticalLayout*> parent) {
	const auto slot = parent->add(
		object_ptr<Ui::VerticalLayout>(parent),
		QMargins(14, 4, 14, 4),
		style::al_justify);
	const auto card = slot->add(
		object_ptr<Ui::VerticalLayout>(slot),
		QMargins(),
		style::al_justify);
	card->paintOn([=](QPainter &p) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBg);
		p.drawRoundedRect(card->rect(), 22, 22);
	});
	slot->show();
	card->show();
	// Ряды добавляются позже общей раскладки: задаём ширину сами.
	const auto available = parent->width() - 28;
	if (available > 0) {
		slot->resizeToWidth(available);
	}
	return {
		.card = card,
		.collapse = [=] { slot->clear(); },
	};
}

// Круглый переключатель выбора в правом верхнем углу карточки.
[[nodiscard]] not_null<Ui::AbstractButton*> AddCheck(
		not_null<Ui::RpWidget*> card,
		bool initial,
		Fn<void(bool)> changed,
		Fn<void(Fn<void(bool)>)> exposeSetter) {
	const auto button = Ui::CreateChild<Ui::AbstractButton>(card.get());
	button->resize(30, 30);
	button->setPointerCursor(true);
	const auto on = std::make_shared<bool>(initial);
	button->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(button);
		auto hq = PainterHighQualityEnabler(p);
		const auto rect = QRectF(button->rect()).adjusted(3, 3, -3, -3);
		if (*on) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowActiveTextFg);
			p.drawEllipse(rect);
			auto pen = QPen(QColor(255, 255, 255), 2.2);
			pen.setCapStyle(Qt::RoundCap);
			pen.setJoinStyle(Qt::RoundJoin);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			p.drawLine(QPointF(9.5, 15.5), QPointF(13.5, 19.5));
			p.drawLine(QPointF(13.5, 19.5), QPointF(21, 11));
		} else {
			p.setPen(QPen(QColor(255, 255, 255, 90), 1.6));
			p.setBrush(Qt::NoBrush);
			p.drawEllipse(rect);
		}
	}, button->lifetime());
	const auto apply = [=](bool value) {
		*on = value;
		button->update();
	};
	button->setClickedCallback([=] {
		const auto value = !*on;
		apply(value);
		changed(value);
	});
	exposeSetter(apply);
	card->widthValue(
	) | rpl::on_next([=](int width) {
		button->move(width - 14 - button->width(), 12);
	}, button->lifetime());
	return button;
}

// Ряд кнопок равной ширины.
[[nodiscard]] not_null<Ui::RpWidget*> AddButtonsRow(
		not_null<Ui::VerticalLayout*> parent,
		std::vector<not_null<Ui::RoundButton*>> buttons) {
	const auto row = parent->add(
		object_ptr<Ui::RpWidget>(parent),
		QMargins(14, 4, 14, 4));
	for (const auto button : buttons) {
		button->setParent(row);
		button->show();
	}
	row->resize(row->width(), buttons.front()->height());
	row->widthValue(
	) | rpl::on_next([=](int width) {
		const auto count = int(buttons.size());
		const auto skip = 8;
		const auto each = std::max((width - skip * (count - 1)) / count, 40);
		auto left = 0;
		for (const auto button : buttons) {
			button->setFullWidth(each);
			button->moveToLeft(left, 0);
			left += each + skip;
		}
	}, row->lifetime());
	return row;
}

// Шапка окна: закрыть слева, заголовок по центру.
void AddHeader(
		not_null<Ui::GenericBox*> box,
		not_null<Ui::VerticalLayout*> content,
		const QString &title) {
	const auto header = content->add(
		object_ptr<Ui::RpWidget>(content),
		QMargins(10, 10, 10, 6));
	header->resize(header->width(), 40);
	const auto close = Plugins::MakeHeaderButton(
		header,
		true,
		[=] { box->closeBox(); });
	const auto label = Ui::CreateChild<Ui::FlatLabel>(
		header,
		title,
		st::boxTitle);
	header->widthValue(
	) | rpl::on_next([=](int width) {
		close->move(0, (header->height() - close->height()) / 2);
		label->resizeToNaturalWidth(width - 2 * (close->width() + 8));
		label->move(
			(width - label->width()) / 2,
			(header->height() - label->height()) / 2);
	}, header->lifetime());
}

void ConfirmAndRun(
		not_null<Window::SessionController*> controller,
		const QString &text,
		const QString &button,
		Fn<void()> run) {
	controller->show(Box([=](not_null<Ui::GenericBox*> confirm) {
		Ui::ConfirmBox(confirm, {
			.text = text,
			.confirmed = [=](Fn<void()> close) {
				run();
				close();
			},
			.confirmText = button,
		});
	}));
}

void HistoryBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		uint64 peerId,
		Kind kind,
		int64 messageId,
		HistoryFilter filter = {}) {
	const auto session = &controller->session();
	const auto store = &StoreFor(session);
	const auto deleted = (kind == Kind::Deleted);
	Plugins::RoundedWindow(box);
	const auto content = box->verticalLayout();
	AddHeader(
		box,
		content,
		messageId
			? U("Прежние версии")
			: deleted
			? (peerId ? U("Удалённые") : U("Удалённые везде"))
			: U("История правок"));
	const auto selection = std::make_shared<Selection>();

	if (!messageId && deleted) {
		const auto periods = std::vector<QString>{
			U("За всё время"), U("За сутки"), U("За неделю"), U("За месяц") };
		const auto types = std::vector<QString>{
			U("Все типы"), U("Фото"), U("Видео"), U("Голосовые"),
			U("Кружки"), U("Файлы") };
		const auto periodButton = Ui::CreateChild<Ui::RoundButton>(
			content.get(),
			rpl::single(periods[filter.period] + U(" ▾")),
			st::dialogsPickCancel);
		const auto typeButton = Ui::CreateChild<Ui::RoundButton>(
			content.get(),
			rpl::single(types[filter.type] + U(" ▾")),
			st::dialogsPickCancel);
		AddButtonsRow(content, { periodButton, typeButton });
		const auto reopen = [=](HistoryFilter next) {
			box->closeBox();
			controller->show(Box(
				HistoryBox,
				controller,
				peerId,
				kind,
				messageId,
				next));
		};
		const auto pick = [=](
				not_null<Ui::RoundButton*> button,
				const std::vector<QString> &names,
				Fn<HistoryFilter(int)> make) {
			button->setClickedCallback([=] {
				const auto menu = Ui::CreateChild<Ui::PopupMenu>(
					button.get(),
					st::popupMenuWithIcons);
				for (auto i = 0; i != int(names.size()); ++i) {
					menu->addAction(names[i], [=] { reopen(make(i)); });
				}
				menu->popup(QCursor::pos());
			});
		};
		pick(periodButton, periods, [=](int i) {
			auto next = filter;
			next.period = i;
			return next;
		});
		pick(typeButton, types, [=](int i) {
			auto next = filter;
			next.type = i;
			return next;
		});

		const auto selectAll = Ui::CreateChild<Ui::RoundButton>(
			content.get(),
			rpl::single(U("Выбрать всё")),
			st::dialogsPickButton);
		const auto removePicked = Ui::CreateChild<Ui::RoundButton>(
			content.get(),
			rpl::single(U("Удалить (0)")),
			st::dialogsPickDanger);
		const auto clearAll = Ui::CreateChild<Ui::RoundButton>(
			content.get(),
			rpl::single(U("Очистить")),
			st::dialogsPickCancel);
		AddButtonsRow(content, { selectAll, removePicked, clearAll });
		selectAll->setClickedCallback([=] {
			for (const auto &[key, setter] : selection->setters) {
				setter(true);
				selection->picked.insert(key);
			}
			selection->changed.fire({});
		});
		selection->changed.events(
		) | rpl::on_next([=] {
			removePicked->setText(rpl::single(
				U("Удалить (%1)").arg(selection->picked.size())));
		}, box->lifetime());
		removePicked->setClickedCallback([=] {
			if (selection->picked.empty()) {
				return;
			}
			const auto count = selection->picked.size();
			ConfirmAndRun(
				controller,
				U("Удалить выбранные сообщения из истории (%1)?").arg(count),
				U("Удалить"),
				[=] {
					for (const auto &key : selection->picked) {
						store->removeOne(key.first, key.second);
						selection->remove(key);
						selection->setters.erase(key);
					}
					selection->picked.clear();
					selection->changed.fire({});
				});
		});
		clearAll->setClickedCallback([=] {
			ConfirmAndRun(
				controller,
				peerId
					? U("Очистить все удалённые сообщения этого чата?")
					: U("Очистить все удалённые сообщения из всех чатов?"),
				U("Очистить"),
				[=] {
					if (peerId) {
						store->clearPeer(peerId);
					} else {
						store->clearAll();
					}
					box->closeBox();
				});
		});
	}

	const auto empty = content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			U("Загрузка..."),
			st::boxLabel),
		QMargins(24, 12, 24, 12));
	const auto list = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		QMargins(),
		style::al_justify);
	const auto state = std::make_shared<PageState>();
	const auto mark = Settings::String(u"history.mark"_q);
	const auto weakList = QPointer<Ui::VerticalLayout>(list);
	const auto weakEmpty = QPointer<Ui::FlatLabel>(empty);
	const auto sinceEvent = !filter.period
		? 0
		: int32(base::unixtime::now()
			- ((filter.period == 1)
				? 86400
				: (filter.period == 2)
				? 7 * 86400
				: 30 * 86400));
	const auto mediaLike = [&] {
		switch (filter.type) {
		case 1: return tr::lng_in_dlg_photo(tr::now) + u"%"_q;
		case 2: return tr::lng_in_dlg_video(tr::now) + u"%"_q;
		case 3: return tr::lng_in_dlg_audio(tr::now) + u"%"_q;
		case 4: return tr::lng_in_dlg_video_message(tr::now) + u"%"_q;
		case 5: return tr::lng_in_dlg_file(tr::now) + u"%"_q;
		}
		return QString();
	}();

	const auto loadMore = [=] {
		if (state->loading || state->finished) {
			return;
		}
		state->loading = true;
		store->loadPage(PageQuery{
			.peer = peerId,
			.kind = kind,
			.id = messageId,
			.beforeEvent = state->cursorEvent,
			.beforeId = state->cursorId,
			.since = sinceEvent,
			.mediaLike = mediaLike,
			.limit = kPageSize,
		}, [=](std::vector<Snapshot> rows) {
			state->loading = false;
			if (rows.size() < kPageSize) {
				state->finished = true;
			}
			if (!weakList || !weakEmpty) {
				return;
			}
			for (const auto &row : rows) {
				auto head = FormatTime(row.event)
					+ U(" · ") + PeerTitle(session, row.from);
				if (!peerId) {
					head += U(" · ") + PeerTitle(session, row.peer);
				}
				if (deleted) {
					head = mark + U(" ") + head;
				}
				auto body = row.text;
				if (!row.media.isEmpty() && row.media != row.text) {
					body = body.isEmpty()
						? row.media
						: (row.media + U("\n") + body);
				}
				if (body.isEmpty()) {
					body = U("(пусто)");
				}
				const auto parts = AddCard(weakList);
				const auto card = parts.card;
				const auto key = Key(row.peer, row.id);
				card->add(
					object_ptr<Ui::FlatLabel>(
						card,
						head,
						st::defaultSubsectionTitle),
					QMargins(18, 14, 56, 0));
				card->add(
					object_ptr<Ui::FlatLabel>(
						card,
						body,
						st::boxLabel),
					QMargins(18, 6, 56, 14));
				if (deleted) {
					selection->removers[key] = parts.collapse;
					AddCheck(card, false, [=](bool on) {
						selection->set(key, on);
					}, [=](Fn<void(bool)> setter) {
						selection->setters[key] = std::move(setter);
					});
				}
			}
			if (!rows.empty()) {
				state->cursorEvent = rows.back().event;
				state->cursorId = rows.back().id;
			}
			weakEmpty->setText(list->count()
				? QString()
				: (deleted
					? U("Здесь пока нет удалённых сообщений.")
					: U("Здесь пока нет сохранённых правок.")));
			weakEmpty->setVisible(!list->count());
		});
	};
	loadMore();

	const auto more = Ui::CreateChild<Ui::RoundButton>(
		content.get(),
		rpl::single(U("Показать ещё")),
		st::dialogsPickCancel);
	more->setClickedCallback(loadMore);
	if (messageId) {
		AddButtonsRow(content, { more });
		return;
	}
	const auto switchKind = Ui::CreateChild<Ui::RoundButton>(
		content.get(),
		rpl::single(deleted ? U("Правки") : U("Удалённые")),
		st::dialogsPickCancel);
	switchKind->setClickedCallback([=] {
		box->closeBox();
		ShowHistoryBox(
			controller,
			peerId,
			deleted ? Kind::Edited : Kind::Deleted);
	});
	AddButtonsRow(content, { more, switchKind });
	Ui::AddSkip(content, 10);
}

void ChatsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	const auto store = &StoreFor(session);
	Plugins::RoundedWindow(box);
	const auto content = box->verticalLayout();
	AddHeader(box, content, U("История по чатам"));
	const auto selection = std::make_shared<Selection>();
	const auto allDeleted = Ui::CreateChild<Ui::RoundButton>(
		content.get(),
		rpl::single(U("Все удалённые")),
		st::dialogsPickButton);
	const auto removePicked = Ui::CreateChild<Ui::RoundButton>(
		content.get(),
		rpl::single(U("Очистить (0)")),
		st::dialogsPickDanger);
	AddButtonsRow(content, { allDeleted, removePicked });
	allDeleted->setClickedCallback([=] {
		ShowHistoryBox(controller, 0, Kind::Deleted);
	});
	selection->changed.events(
	) | rpl::on_next([=] {
		removePicked->setText(rpl::single(
			U("Очистить (%1)").arg(selection->picked.size())));
	}, box->lifetime());
	const auto list = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		QMargins(),
		style::al_justify);
	const auto weakList = QPointer<Ui::VerticalLayout>(list);
	removePicked->setClickedCallback([=] {
		if (selection->picked.empty()) {
			return;
		}
		const auto count = selection->picked.size();
		ConfirmAndRun(
			controller,
			U("Очистить историю выбранных чатов (%1)?").arg(count),
			U("Очистить"),
			[=] {
				for (const auto &key : selection->picked) {
					store->clearPeer(key.first);
					selection->remove(key);
				}
				selection->picked.clear();
				selection->changed.fire({});
			});
	});
	auto rows = std::vector<std::pair<uint64, Counts>>();
	for (const auto &[peer, counts] : store->allCounts()) {
		rows.emplace_back(peer, counts);
	}
	std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
		return (a.second.deleted + a.second.edited)
			> (b.second.deleted + b.second.edited);
	});
	if (rows.empty()) {
		weakList->add(
			object_ptr<Ui::FlatLabel>(
				weakList,
				U("История пока пуста."),
				st::boxLabel),
			QMargins(24, 12, 24, 12));
	}
	for (const auto &[peer, counts] : rows) {
		const auto parts = AddCard(weakList);
		const auto card = parts.card;
		const auto key = Key(peer, 0);
		const auto kind = counts.deleted ? Kind::Deleted : Kind::Edited;
		const auto id = peer;
		const auto title = card->add(
			object_ptr<Ui::FlatLabel>(
				card,
				PeerTitle(session, peer),
				st::defaultSubsectionTitle),
			QMargins(18, 14, 56, 0));
		const auto sub = card->add(
			object_ptr<Ui::FlatLabel>(
				card,
				U("удалено %1 · правок %2").arg(counts.deleted).arg(
					counts.edited),
				st::boxLabel),
			QMargins(18, 4, 56, 14));
		title->setAttribute(Qt::WA_TransparentForMouseEvents);
		sub->setAttribute(Qt::WA_TransparentForMouseEvents);
		const auto hit = Ui::CreateChild<Ui::AbstractButton>(card);
		hit->setPointerCursor(true);
		hit->setClickedCallback([=] {
			ShowHistoryBox(controller, id, kind);
		});
		card->sizeValue(
		) | rpl::on_next([=](QSize size) {
			hit->setGeometry(0, 0, size.width(), size.height());
		}, hit->lifetime());
		selection->removers[key] = parts.collapse;
		AddCheck(card, false, [=](bool on) {
			selection->set(key, on);
		}, [=](Fn<void(bool)> setter) {
			selection->setters[key] = std::move(setter);
		});
	}
	Ui::AddSkip(content, 10);
}

void FiltersBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		uint64 peerId) {
	box->setTitle(rpl::single(U("Фильтры сообщений")));
	box->setWidth(st::boxWideWidth);
	const auto content = box->verticalLayout();
	Ui::AddSkip(content);
	const auto list = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		QMargins(),
		style::al_justify);
	const auto weakList = QPointer<Ui::VerticalLayout>(list);

	const auto rebuild = [=] {
		if (!weakList) {
			return;
		}
		weakList->clear();
		const auto filters = Filters();
		if (filters.empty()) {
			weakList->add(
				object_ptr<Ui::FlatLabel>(
					weakList,
					U("Фильтров пока нет."),
					st::boxLabel),
				st::boxPadding);
		}
		for (const auto &filter : filters) {
			auto text = filter.pattern;
			text += filter.peers.empty()
				? U("  (везде)")
				: U("  (чатов: %1)").arg(filter.peers.size());
			weakList->add(
				object_ptr<Ui::FlatLabel>(weakList, text, st::boxLabel),
				st::boxPadding);
			const auto remove = weakList->add(
				object_ptr<Ui::LinkButton>(weakList, U("Удалить")),
				st::boxPadding);
			const auto id = filter.id;
			remove->setClickedCallback([=] {
				auto now = Filters();
				now.erase(
					std::remove_if(now.begin(), now.end(), [&](const Filter &f) {
						return f.id == id;
					}),
					now.end());
				ReplaceFilters(std::move(now));
			});
		}
	};
	rebuild();
	FiltersChanged() | rpl::on_next(rebuild, box->lifetime());

	Ui::AddDivider(content);
	Ui::AddSkip(content);
	const auto pattern = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::defaultInputField,
			rpl::single(U("Регулярное выражение"))),
		st::boxPadding);
	const auto ignoreCase = content->add(
		object_ptr<Ui::Checkbox>(content, U("Без учёта регистра"), true),
		st::boxPadding);
	const auto sample = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::defaultInputField,
			rpl::single(U("Проверка: введи тестовую строку"))),
		st::boxPadding);
	const auto status = content->add(
		object_ptr<Ui::FlatLabel>(content, QString(), st::boxLabel),
		st::boxPadding);

	const auto refresh = [=] {
		const auto text = pattern->getLastText();
		if (text.isEmpty()) {
			status->setText(QString());
			return;
		}
		const auto error = ValidateFilter(text, ignoreCase->checked());
		if (!error.isEmpty()) {
			status->setText(U("Ошибка: ") + error);
		} else if (sample->getLastText().isEmpty()) {
			status->setText(U("Выражение корректно."));
		} else {
			status->setText(TestFilter(
				text,
				ignoreCase->checked(),
				sample->getLastText())
				? U("Совпадает, сообщение будет скрыто.")
				: U("Не совпадает."));
		}
	};
	pattern->changes() | rpl::on_next(refresh, pattern->lifetime());
	sample->changes() | rpl::on_next(refresh, sample->lifetime());
	ignoreCase->checkedChanges() | rpl::on_next(refresh, ignoreCase->lifetime());

	box->addButton(rpl::single(U("Добавить")), [=] {
		const auto text = pattern->getLastText();
		if (!ValidateFilter(text, ignoreCase->checked()).isEmpty()) {
			refresh();
			pattern->showError();
			return;
		}
		auto now = Filters();
		auto filter = Filter();
		filter.pattern = text;
		filter.ignoreCase = ignoreCase->checked();
		if (peerId) {
			filter.peers.emplace(peerId);
		}
		now.push_back(std::move(filter));
		ReplaceFilters(std::move(now));
		pattern->setText(QString());
		sample->setText(QString());
	});
	box->addButton(rpl::single(U("Закрыть")), [=] { box->closeBox(); });
	if (peerId) {
		const auto shown = [=] { return ShowHiddenInChat(peerId); };
		box->addLeftButton(
			rpl::single(U("Показать скрытое")),
			[=] {
				SetShowHiddenInChat(peerId, !shown());
			});
	}
}

} // namespace

void ShowHistoryBox(
		not_null<Window::SessionController*> controller,
		uint64 peerId,
		Kind kind) {
	controller->show(Box(
		HistoryBox,
		controller,
		peerId,
		kind,
		int64(0),
		HistoryFilter()));
}

void ShowRevisionsBox(
		not_null<Window::SessionController*> controller,
		uint64 peerId,
		int64 messageId) {
	controller->show(Box(
		HistoryBox,
		controller,
		peerId,
		Kind::Edited,
		messageId,
		HistoryFilter()));
}

void ShowChatsBox(not_null<Window::SessionController*> controller) {
	controller->show(Box(ChatsBox, controller));
}

void ShowFiltersBox(
		not_null<Window::SessionController*> controller,
		uint64 peerId) {
	controller->show(Box(FiltersBox, controller, peerId));
}

void FillGhostMenu(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		std::vector<not_null<HistoryItem*>> items,
		Fn<void(const TextWithTags &)> insertText) {
	if (items.empty()) {
		return;
	}
	std::stable_sort(items.begin(), items.end(), [](
			not_null<HistoryItem*> a,
			not_null<HistoryItem*> b) {
		return a->date() < b->date();
	});
	struct Entry {
		QString name;
		QString link;
		QString text;
		FullMsgId id;
	};
	auto entries = std::vector<Entry>();
	for (const auto &item : items) {
		const auto from = item->from();
		const auto username = from->username();
		entries.push_back({
			.name = from->name(),
			.link = username.isEmpty()
				? (u"tg://user?id="_q + QString::number(from->id.value))
				: (u"https://t.me/"_q + username),
			.text = item->originalText().text,
			.id = item->fullId(),
		});
	}
	const auto session = &items.front()->history()->session();
	const auto count = int(entries.size());
	const auto suffix = (count > 1)
		? (u" ("_q + QString::number(count) + ')')
		: QString();
	if (count == 1) {
		const auto &first = entries.front();
		menu->addAction(QString::fromUtf8("Ответить"), [=] {
			if (!insertText) {
				return;
			}
			// <blockquote><a href=…>Имя</a>\nтекст</blockquote>, затем строка.
			auto result = TextWithTags();
			const auto quote = first.name + '\n' + first.text;
			result.text = quote + '\n';
			const auto both = TextUtilities::TagWithAdded(
				first.link,
				Ui::InputField::kTagBlockquote);
			result.tags.push_back({ 0, int(first.name.size()), both });
			result.tags.push_back({
				int(first.name.size()),
				int(quote.size() - first.name.size()),
				Ui::InputField::kTagBlockquote });
			insertText(result);
		}, &st::menuIconReply);
	}
	menu->addAction(QString::fromUtf8("Переслать") + suffix, [=] {
		Window::ShowChooseRecipientBox(controller, [=](
				not_null<Data::Thread*> thread) {
			// По одному сообщению на каждое удалённое.
			for (const auto &entry : entries) {
				auto message = Api::MessageToSend(Api::SendAction(thread));
				const auto head = QString::fromUtf8("Переслано от ");
				message.textWithTags.text = head
					+ entry.name
					+ '\n'
					+ entry.text;
				message.textWithTags.tags.push_back({
					int(head.size()),
					int(entry.name.size()),
					entry.link });
				message.webPage.removed = true;
				thread->session().api().sendMessage(std::move(message));
			}
			return true;
		});
	}, &st::menuIconForward);
	menu->addAction(QString::fromUtf8("Копировать текст") + suffix, [=] {
		auto all = QStringList();
		for (const auto &entry : entries) {
			all.push_back((count > 1)
				? (entry.name + u": "_q + entry.text)
				: entry.text);
		}
		QGuiApplication::clipboard()->setText(all.join('\n'));
	}, &st::menuIconCopy);
	menu->addAction(QString::fromUtf8("Убрать из истории") + suffix, [=] {
		for (const auto &entry : entries) {
			if (const auto strong = session->data().message(entry.id)) {
				RemoveGhost(strong);
			}
		}
	}, &st::menuIconDelete);
}


namespace {

constexpr auto kDeletePage = 100;

struct DeleteMineState {
	base::weak_ptr<Main::Session> session;
	PeerId peerId = 0;
	std::vector<MTPint> ids;
	int found = 0;
	int deleted = 0;
	bool cancelled = false;
	bool deleting = false;
	mtpRequestId request = 0;
	QPointer<Ui::GenericBox> box;
	QPointer<Ui::FlatLabel> label;
	base::weak_ptr<Window::SessionController> controller;
	// Для обхода нескольких чатов: вместо закрытия окна идём к следующему.
	Fn<void()> onFinish;
};

void RunDeleteMine(std::shared_ptr<DeleteMineState> state) {
	const auto session = state->session.get();
	const auto peer = session ? session->data().peerLoaded(state->peerId) : nullptr;
	if (!peer) {
		return;
	}
	const auto setLabel = [=](const QString &text) {
		if (state->label) {
			state->label->setText(text);
		}
	};
	const auto finish = [=] {
		state->request = 0;
		if (state->onFinish) {
			const auto next = state->onFinish;
			next();
			return;
		}
		if (const auto box = state->box) {
			box->closeBox();
		}
		if (const auto controller = state->controller.get()) {
			controller->showToast(state->cancelled
				? QString::fromUtf8("Остановлено. Удалено сообщений: %1").arg(state->deleted)
				: QString::fromUtf8("Готово. Удалено сообщений: %1").arg(state->deleted));
		}
	};
	// Фаза 2: удаляем найденные id пачками.
	const auto deleteNext = std::make_shared<Fn<void()>>();
	*deleteNext = [=] {
		if (state->cancelled || state->ids.empty()) {
			finish();
			return;
		}
		const auto count = std::min<int>(state->ids.size(), kDeletePage);
		auto batch = QVector<MTPint>();
		for (auto i = 0; i != count; ++i) {
			batch.push_back(state->ids[state->ids.size() - 1 - i]);
		}
		state->ids.resize(state->ids.size() - count);
		const auto done = [=](const MTPmessages_AffectedMessages &result) {
			session->api().applyAffectedMessages(peer, result);
			state->deleted += count;
			setLabel(QString::fromUtf8("Удалено %1 из %2").arg(state->deleted).arg(state->found));
			(*deleteNext)();
		};
		const auto fail = [=](const MTP::Error &) {
			finish();
		};
		if (const auto channel = peer->asChannel()) {
			state->request = session->api().request(MTPchannels_DeleteMessages(
				channel->inputChannel(),
				MTP_vector<MTPint>(batch)
			)).done(done).fail(fail).send();
		} else {
			state->request = session->api().request(MTPmessages_DeleteMessages(
				MTP_flags(MTPmessages_DeleteMessages::Flag::f_revoke),
				MTP_vector<MTPint>(batch)
			)).done(done).fail(fail).send();
		}
	};
	// Фаза 1: собираем id своих сообщений постранично.
	const auto isUserChat = peer->isUser();
	const auto searchNext = std::make_shared<Fn<void(int)>>();
	*searchNext = [=](int offsetId) {
		if (state->cancelled) {
			finish();
			return;
		}
		const auto received = [=](const MTPmessages_Messages &result) {
			auto messages = QVector<MTPMessage>();
			result.match([&](const MTPDmessages_messagesNotModified &) {
			}, [&](const auto &data) {
				messages = data.vmessages().v;
			});
			auto lowest = 0;
			for (const auto &message : messages) {
				message.match([&](const MTPDmessage &data) {
					const auto id = data.vid().v;
					lowest = lowest ? std::min(lowest, id) : id;
					if (data.is_out() || !isUserChat) {
						state->ids.push_back(MTP_int(id));
						++state->found;
					}
				}, [&](const auto &) {
				});
			}
			setLabel(QString::fromUtf8("Найдено своих сообщений: %1").arg(state->found));
			if (messages.isEmpty() || !lowest) {
				if (qEnvironmentVariableIsSet("MOKAGRAM_DELETE_DRYRUN")) {
					state->ids.clear();
					state->deleted = state->found;
					state->cancelled = true;
					finish();
					return;
				}
				state->deleting = true;
				setLabel(QString::fromUtf8("Найдено: %1. Удаляю…").arg(state->found));
				(*deleteNext)();
			} else {
				(*searchNext)(lowest);
			}
		};
		const auto fail = [=](const MTP::Error &) {
			// Что нашли, то удаляем.
			state->deleting = true;
			(*deleteNext)();
		};
		if (isUserChat) {
			state->request = session->api().request(MTPmessages_GetHistory(
				peer->input(),
				MTP_int(offsetId),
				MTP_int(0),
				MTP_int(0),
				MTP_int(kDeletePage),
				MTP_int(0),
				MTP_int(0),
				MTP_long(0)
			)).done(received).fail(fail).send();
		} else {
			state->request = session->api().request(MTPmessages_Search(
				MTP_flags(MTPmessages_Search::Flag::f_from_id),
				peer->input(),
				MTP_string(),
				session->user()->input(),
				MTP_inputPeerEmpty(),
				MTP_vector<MTPReaction>(),
				MTP_int(0),
				MTP_inputMessagesFilterEmpty(),
				MTP_int(0),
				MTP_int(0),
				MTP_int(offsetId),
				MTP_int(0),
				MTP_int(kDeletePage),
				MTP_int(0),
				MTP_int(0),
				MTP_long(0)
			)).done(received).fail(fail).send();
		}
	};
	(*searchNext)(0);
}

} // namespace

bool CanDeleteMyMessages(not_null<PeerData*> peer) {
	if (const auto channel = peer->asChannel()) {
		return channel->isMegagroup();
	}
	return peer->isUser() || peer->isChat();
}

void ShowDeleteMyMessages(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto title = peer->isSelf()
		? QString::fromUtf8("Избранное")
		: peer->name();
	const auto weak = base::make_weak(controller);
	const auto session = base::make_weak(&peer->session());
	const auto peerId = peer->id;
	controller->show(Ui::MakeConfirmBox({
		.text = rpl::single(QString::fromUtf8(
			"Удалить все ваши сообщения в «%1»?\n\n"
			"Они исчезнут у всех участников чата. Отменить это нельзя.").arg(title)),
		.confirmed = [=](Fn<void()> &&close) {
			close();
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			const auto state = std::make_shared<DeleteMineState>();
			state->session = session;
			state->peerId = peerId;
			state->controller = weak;
			strong->show(Box([=](not_null<Ui::GenericBox*> box) {
				state->box = box;
				box->setTitle(rpl::single(QString::fromUtf8("Удаление моих сообщений")));
				const auto content = box->verticalLayout();
				Ui::AddSkip(content);
				state->label = content->add(
					object_ptr<Ui::FlatLabel>(
						content,
						QString::fromUtf8("Ищу ваши сообщения…"),
						st::boxLabel),
					st::boxPadding);
				Ui::AddSkip(content);
				box->addButton(
					rpl::single(QString::fromUtf8("Остановить")),
					[=] {
						state->cancelled = true;
						if (const auto strong = state->session.get()) {
							if (state->request) {
								strong->api().request(state->request).cancel();
							}
						}
						box->closeBox();
					},
					st::attentionBoxButton);
				box->boxClosing() | rpl::on_next([=] {
					state->cancelled = true;
				}, box->lifetime());
			}));
			RunDeleteMine(state);
		},
		.confirmText = rpl::single(QString::fromUtf8("Удалить")),
		.confirmStyle = &st::attentionBoxButton,
		.title = rpl::single(QString::fromUtf8("Удалить все мои сообщения")),
	}));
}

namespace {

struct DeleteMultiState {
	base::weak_ptr<Main::Session> session;
	base::weak_ptr<Window::SessionController> controller;
	std::vector<PeerId> peers;
	int index = 0;
	int deleted = 0;
	bool cancelled = false;
	QPointer<Ui::GenericBox> box;
	QPointer<Ui::FlatLabel> label;
	std::shared_ptr<DeleteMineState> current;
};

void RunDeleteMulti(std::shared_ptr<DeleteMultiState> multi) {
	const auto session = multi->session.get();
	const auto finishAll = [=] {
		if (const auto box = multi->box) {
			box->closeBox();
		}
		if (const auto controller = multi->controller.get()) {
			controller->showToast(multi->cancelled
				? QString::fromUtf8("Остановлено. Удалено сообщений: %1").arg(multi->deleted)
				: QString::fromUtf8("Готово. Удалено сообщений: %1").arg(multi->deleted));
		}
	};
	if (!session || multi->cancelled || multi->index >= int(multi->peers.size())) {
		finishAll();
		return;
	}
	const auto peerId = multi->peers[multi->index];
	const auto peer = session->data().peerLoaded(peerId);
	const auto title = !peer
		? QString()
		: peer->isSelf()
		? QString::fromUtf8("Избранное")
		: peer->name();
	if (multi->label) {
		multi->label->setText(QString::fromUtf8("Чат %1 из %2: %3")
			.arg(multi->index + 1)
			.arg(multi->peers.size())
			.arg(title));
	}
	const auto state = std::make_shared<DeleteMineState>();
	multi->current = state;
	state->session = multi->session;
	state->peerId = peerId;
	state->controller = multi->controller;
	state->label = multi->label;
	state->onFinish = [=] {
		multi->deleted += state->deleted;
		++multi->index;
		RunDeleteMulti(multi);
	};
	RunDeleteMine(state);
}

} // namespace

void ShowDeleteMyMessagesMulti(
		not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		Plugins::RoundedWindow(box);
		const auto content = box->verticalLayout();
		AddHeader(box, content, U("Удалить мои сообщения"));
		const auto selection = std::make_shared<Selection>();
		const auto run = Ui::CreateChild<Ui::RoundButton>(
			content.get(),
			rpl::single(U("Удалить (0)")),
			st::dialogsPickDanger);
		AddButtonsRow(content, { run });
		selection->changed.events(
		) | rpl::on_next([=] {
			run->setText(rpl::single(
				U("Удалить (%1)").arg(selection->picked.size())));
		}, box->lifetime());
		const auto list = content->add(
			object_ptr<Ui::VerticalLayout>(content),
			QMargins(),
			style::al_justify);
		const auto weakList = QPointer<Ui::VerticalLayout>(list);
		const auto session = &controller->session();
		auto count = 0;
		for (const auto &row : session->data().chatsList()->indexed()->all()) {
			const auto history = row->history();
			if (!history || !CanDeleteMyMessages(history->peer)) {
				continue;
			}
			if (++count > 200) {
				break;
			}
			const auto peer = history->peer;
			const auto key = Key(peer->id.value, 0);
			const auto parts = AddCard(weakList);
			const auto card = parts.card;
			const auto title = card->add(
				object_ptr<Ui::FlatLabel>(
					card,
					peer->isSelf() ? U("Избранное") : peer->name(),
					st::defaultSubsectionTitle),
				QMargins(18, 14, 56, 14));
			title->setAttribute(Qt::WA_TransparentForMouseEvents);
			AddCheck(card, false, [=](bool on) {
				selection->set(key, on);
			}, [=](Fn<void(bool)> setter) {
				selection->setters[key] = std::move(setter);
			});
		}
		Ui::AddSkip(content, 10);
		const auto weak = base::make_weak(controller);
		run->setClickedCallback([=] {
			if (selection->picked.empty()) {
				return;
			}
			const auto picked = selection->picked;
			ConfirmAndRun(
				controller,
				U("Удалить все ваши сообщения в выбранных чатах (%1)?\n\n"
					"Они исчезнут у всех участников. Отменить это нельзя.")
					.arg(picked.size()),
				U("Удалить"),
				[=] {
					const auto strong = weak.get();
					if (!strong) {
						return;
					}
					const auto multi = std::make_shared<DeleteMultiState>();
					multi->session = base::make_weak(&strong->session());
					multi->controller = weak;
					for (const auto &key : picked) {
						multi->peers.push_back(PeerId(key.first));
					}
					strong->show(Box([=](not_null<Ui::GenericBox*> progress) {
						multi->box = progress;
						progress->setTitle(rpl::single(U("Удаление моих сообщений")));
						const auto inner = progress->verticalLayout();
						Ui::AddSkip(inner);
						multi->label = inner->add(
							object_ptr<Ui::FlatLabel>(
								inner,
								U("Ищу ваши сообщения…"),
								st::boxLabel),
							st::boxPadding);
						Ui::AddSkip(inner);
						progress->addButton(
							rpl::single(U("Остановить")),
							[=] {
								multi->cancelled = true;
								if (const auto current = multi->current) {
									current->cancelled = true;
									if (const auto s = current->session.get()) {
										if (current->request) {
											s->api().request(current->request).cancel();
										}
									}
								}
								progress->closeBox();
							},
							st::attentionBoxButton);
						progress->boxClosing() | rpl::on_next([=] {
							multi->cancelled = true;
						}, progress->lifetime());
					}));
					RunDeleteMulti(multi);
				});
		});
	}));
}

} // namespace Mokagram::History
