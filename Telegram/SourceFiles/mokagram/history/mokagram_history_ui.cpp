// Mokagram: экраны истории (удалённые, правки, список чатов) и редактор фильтров.
#include "mokagram/history/mokagram_history.h"

#include "mokagram/settings/mokagram_settings.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/platform/base_platform_info.h"
#include "data/data_thread.h"
#include "data/data_peer.h"
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

void HistoryBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		uint64 peerId,
		Kind kind,
		int64 messageId) {
	const auto session = &controller->session();
	const auto store = &StoreFor(session);
	const auto deleted = (kind == Kind::Deleted);
	box->setTitle(rpl::single(messageId
		? U("Прежние версии сообщения")
		: deleted
		? U("Удалённые сообщения")
		: U("История правок")));
	box->setWidth(st::boxWideWidth);
	const auto content = box->verticalLayout();
	Ui::AddSkip(content);
	const auto empty = content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			U("Загрузка..."),
			st::boxLabel),
		st::boxPadding);
	const auto list = content->add(object_ptr<Ui::VerticalLayout>(content));
	const auto state = std::make_shared<PageState>();
	const auto mark = Settings::String(u"history.mark"_q);
	const auto weakList = QPointer<Ui::VerticalLayout>(list);
	const auto weakEmpty = QPointer<Ui::FlatLabel>(empty);

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
				if (deleted) {
					head = mark + U(" ") + head;
				}
				Ui::AddDivider(weakList);
				weakList->add(
					object_ptr<Ui::FlatLabel>(
						weakList,
						head,
						st::defaultSubsectionTitle),
					st::boxPadding);
				auto body = row.text;
				if (!row.media.isEmpty() && row.media != row.text) {
					body = body.isEmpty()
						? row.media
						: (row.media + U("\n") + body);
				}
				if (body.isEmpty()) {
					body = U("(пусто)");
				}
				weakList->add(
					object_ptr<Ui::FlatLabel>(
						weakList,
						body,
						st::boxLabel),
					st::boxPadding);
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

	box->addButton(rpl::single(U("Ещё")), loadMore);
	box->addButton(rpl::single(U("Закрыть")), [=] { box->closeBox(); });
	if (messageId) {
		return;
	}
	box->addLeftButton(
		rpl::single(deleted ? U("Правки") : U("Удалённые")),
		[=] {
			box->closeBox();
			ShowHistoryBox(
				controller,
				peerId,
				deleted ? Kind::Edited : Kind::Deleted);
		});
}

void ChatsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	const auto store = &StoreFor(session);
	box->setTitle(rpl::single(U("История по чатам")));
	const auto content = box->verticalLayout();
	Ui::AddSkip(content);
	const auto list = content->add(object_ptr<Ui::VerticalLayout>(content));
	const auto weakList = QPointer<Ui::VerticalLayout>(list);
	const auto rebuild = [=] {
		if (!weakList) {
			return;
		}
		weakList->clear();
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
				st::boxPadding);
		}
		for (const auto &[peer, counts] : rows) {
			const auto text = U("%1: удалено %2, правок %3").arg(
				PeerTitle(session, peer)).arg(counts.deleted).arg(counts.edited);
			const auto row = weakList->add(
				object_ptr<Ui::LinkButton>(weakList, text),
				st::boxPadding);
			const auto id = peer;
			const auto kind = counts.deleted ? Kind::Deleted : Kind::Edited;
			row->setClickedCallback([=] {
				ShowHistoryBox(controller, id, kind);
			});
		}
	};
	rebuild();
	store->changes() | rpl::on_next(rebuild, box->lifetime());
	box->addButton(rpl::single(U("Закрыть")), [=] { box->closeBox(); });
}

void FiltersBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		uint64 peerId) {
	box->setTitle(rpl::single(U("Фильтры сообщений")));
	box->setWidth(st::boxWideWidth);
	const auto content = box->verticalLayout();
	Ui::AddSkip(content);
	const auto list = content->add(object_ptr<Ui::VerticalLayout>(content));
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
	controller->show(Box(HistoryBox, controller, peerId, kind, int64(0)));
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
		messageId));
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

} // namespace Mokagram::History
