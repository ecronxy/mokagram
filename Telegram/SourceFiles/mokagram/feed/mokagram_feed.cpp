#include "mokagram/feed/mokagram_feed.h"

#include "boxes/peer_list_box.h"
#include "data/data_channel.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/session/session_show.h"
#include "main/main_session.h"
#include "mokagram/lang/mokagram_lang.h"
#include "mokagram/settings/mokagram_settings.h"
#include "mtproto/sender.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/painter.h"
#include "ui/userpic_view.h"
#include "window/section_memento.h"
#include "window/section_widget.h"
#include "window/window_session_controller.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QUuid>
#include <QtGui/QKeyEvent>

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_mokagram_feed.h"

namespace Mokagram::Feed {
namespace {

constexpr auto kMaxCategories = 20;
constexpr auto kMaxChannels = 32;
constexpr auto kPageSize = 20;
constexpr auto kMaxPosts = 2000;

QString Text(const char *value) {
	return QString::fromUtf8(Lang::Tr(value));
}

struct Category {
	QString id;
	QString name;
	std::vector<PeerId> channels;
};

struct Post {
	PeerId peer;
	int id = 0;
	int date = 0;
	QString text;
};

struct Source {
	PeerId peer;
	int offset = 0;
	bool exhausted = false;
};

struct FeedSnapshot {
	std::vector<Post> posts;
	std::vector<Source> sources;
	int failures = 0;
};

class FeedMemento final : public Window::SectionMemento {
public:
	FeedMemento(QString categoryId = {}, int scrollTop = 0,
		std::shared_ptr<const FeedSnapshot> snapshot = nullptr);
	object_ptr<Window::SectionWidget> createWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		Window::Column column,
		const QRect &geometry) override;

	QString categoryId;
	int scrollTop = 0;
	std::shared_ptr<const FeedSnapshot> snapshot;

};

class PostRow final : public Ui::RippleButton {
public:
	PostRow(QWidget *parent, not_null<Main::Session*> session,
		const Post &post, Fn<void()> open);
	QString accessibilityName() override;

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *event) override;

private:
	PeerData *_peer = nullptr;
	Ui::PeerUserpicView _userpic;
	Ui::Text::String _title;
	Ui::Text::String _body;
	QString _date;

};

class ChannelsController final : public PeerListController {
public:
	ChannelsController(not_null<Main::Session*> session, Category category);
	Main::Session &session() const override;
	void prepare() override;
	void rowClicked(not_null<PeerListRow*> row) override;
	rpl::producer<QString> searchPlaceholder() const override;
	QString categoryName() const;
	void setNameField(not_null<Ui::InputField*> field);
	void showNameError();

private:
	not_null<Main::Session*> _session;
	Category _category;
	QPointer<Ui::InputField> _name;

};

class FeedSection final : public Window::SectionWidget {
public:
	FeedSection(QWidget *parent,
		not_null<Window::SessionController*> controller,
		const FeedMemento &memento);
	bool showInternal(not_null<Window::SectionMemento*> memento,
		const Window::SectionShow &params) override;
	std::shared_ptr<Window::SectionMemento> createMemento() override;
	std::shared_ptr<Window::SectionMemento> createIdentityMemento() override;
	QPixmap grabForShowAnimation(const Window::SectionSlideParams &params) override;
	bool floatPlayerHandleWheelEvent(QEvent *event) override;
	QRect floatPlayerAvailableRect() override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void showFinishedHook() override;

private:
	void syncVisibility();
	void updateLayout();
	void updateCategory();
	void showCategories();
	void showMenu();
	void showEmpty(const QString &title, const QString &hint, bool canEdit);
	void renderPosts();
	void loadCategories();
	void saveCategories();
	void populateCategories(int selected = 0);
	void editCategory(bool create);
	void refresh();
	void loadMore();
	void loadNext();

	not_null<Main::Session*> _session;
	QString _key;
	std::unique_ptr<MTP::Sender> _api;
	Ui::IconButton *_back = nullptr;
	Ui::IconButton *_menuButton = nullptr;
	Ui::RoundButton *_category = nullptr;
	Ui::LinkButton *_edit = nullptr;
	Ui::ScrollArea *_scroll = nullptr;
	QPointer<Ui::VerticalLayout> _content;
	Ui::RpWidget *_empty = nullptr;
	Ui::FlatLabel *_emptyTitle = nullptr;
	Ui::FlatLabel *_emptyHint = nullptr;
	Ui::RoundButton *_emptyAction = nullptr;
	QPointer<Ui::RoundButton> _older;
	QPointer<PeerListBox> _editor;
	base::unique_qptr<Ui::PopupMenu> _menu;
	QString _summary;
	std::vector<Category> _groups;
	std::vector<Source> _sources;
	std::vector<Post> _posts;
	int _selected = -1;
	int _restoreTop = 0;
	size_t _source = 0;
	int _failures = 0;
	bool _busy = false;
	bool _showingEmpty = true;

};

FeedMemento::FeedMemento(
		QString categoryId,
		int scrollTop,
		std::shared_ptr<const FeedSnapshot> snapshot)
: categoryId(std::move(categoryId))
, scrollTop(scrollTop)
, snapshot(std::move(snapshot)) {
}

object_ptr<Window::SectionWidget> FeedMemento::createWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		Window::Column column,
		const QRect &geometry) {
	if (column == Window::Column::Third) {
		return nullptr;
	}
	auto result = object_ptr<FeedSection>(parent, controller, *this);
	result->setGeometry(geometry);
	return result;
}

PostRow::PostRow(
		QWidget *parent,
		not_null<Main::Session*> session,
		const Post &post,
		Fn<void()> open)
: Ui::RippleButton(parent, st::defaultRippleAnimationBgOver)
, _peer(session->data().peerLoaded(post.peer))
, _title(st::semiboldTextStyle, _peer ? _peer->name() : QString())
, _body(st::feedPostText, post.text)
, _date(QDateTime::fromSecsSinceEpoch(post.date).toLocalTime()
	.toString(u"dd.MM · HH:mm"_q)) {
	setClickedCallback(std::move(open));
	setPointerCursor(true);
	setFocusPolicy(Qt::StrongFocus);
}

QString PostRow::accessibilityName() {
	return _title.toString() + u" · "_q + _date + '\n' + _body.toString();
}

int PostRow::resizeGetHeight(int newWidth) {
	const auto textWidth = std::max(st::feedAvatarSize,
		newWidth - 2 * st::feedPostPadding);
	return 2 * st::feedPostPadding + st::feedAvatarSize
		+ st::feedPostGap + _body.countHeight(textWidth);
}

void PostRow::paintEvent(QPaintEvent *event) {
	auto p = Painter(this);
	p.fillRect(event->rect(), st::mainMenuBg);
	if (isOver() || hasFocus()) {
		auto hover = st::windowFg->c;
		hover.setAlphaF(0.04);
		p.fillRect(rect(), hover);
	}
	paintRipple(p, 0, 0);
	const auto left = st::feedPostPadding;
	const auto top = st::feedPostPadding;
	if (_peer) {
		_peer->paintUserpicLeft(p, _userpic, left, top,
			width(), st::feedAvatarSize);
	}
	const auto textLeft = left + st::feedAvatarSize + st::feedPostGap;
	p.setPen(st::windowFg);
	_title.draw(p, {
		.position = { textLeft, top },
		.outerWidth = width(),
		.availableWidth = width() - textLeft - left,
		.elisionLines = 1,
	});
	p.setFont(st::feedMetaFont);
	p.setPen(st::windowSubTextFg);
	p.drawTextLeft(textLeft, top + st::semiboldFont->height
		+ st::feedMetaGap, width(), _date);
	p.setPen(st::windowFg);
	_body.draw(p, {
		.position = { left, top + st::feedAvatarSize + st::feedPostGap },
		.outerWidth = width(),
		.availableWidth = width() - 2 * left,
		.clip = event->rect(),
	});
	auto line = st::windowFg->c;
	line.setAlphaF(0.08);
	p.fillRect(left, height() - st::lineWidth,
		width() - 2 * left, st::lineWidth, line);
	if (hasFocus()) {
		p.fillRect(0, 0, st::feedFocusWidth, height(), st::windowActiveTextFg);
	}
}

ChannelsController::ChannelsController(
		not_null<Main::Session*> session,
		Category category)
: _session(session)
, _category(std::move(category)) {
	setStyleOverrides(&st::feedPeerList, &st::feedSearch);
}

Main::Session &ChannelsController::session() const {
	return *_session;
}

void ChannelsController::prepare() {
	delegate()->peerListSetTitle(rpl::single(Text("Выбрать каналы")));
	delegate()->peerListSetSearchMode(PeerListSearchMode::Enabled);
	auto selected = std::vector<not_null<PeerData*>>();
	_session->data().enumerateBroadcasts([&](not_null<ChannelData*> channel) {
		if (!channel->amIn()) {
			return;
		}
		delegate()->peerListAppendRow(std::make_unique<PeerListRow>(channel));
		if (std::find(_category.channels.begin(), _category.channels.end(),
				channel->id) != _category.channels.end()) {
			selected.push_back(channel);
		}
	});
	sortByName();
	delegate()->peerListAddSelectedPeers(selected);
	delegate()->peerListRefreshRows();
}

void ChannelsController::rowClicked(not_null<PeerListRow*> row) {
	const auto checked = delegate()->peerListIsRowChecked(row);
	if (!checked && delegate()->peerListSelectedRowsCount() >= kMaxChannels) {
		delegate()->peerListUiShow()->showToast(Text("Выберите не больше 32 каналов"));
		return;
	}
	delegate()->peerListSetRowChecked(row, !checked);
}

rpl::producer<QString> ChannelsController::searchPlaceholder() const {
	return rpl::single(Text("Найти канал"));
}

QString ChannelsController::categoryName() const {
	return _name ? _name->getLastText().trimmed() : QString();
}

void ChannelsController::setNameField(not_null<Ui::InputField*> field) {
	_name = field;
}

void ChannelsController::showNameError() {
	if (_name) {
		_name->showError();
		_name->setFocusFast();
	}
}

FeedSection::FeedSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		const FeedMemento &memento)
: Window::SectionWidget(parent, controller)
, _session(&controller->session())
, _key(u"feed.%1.categories"_q.arg(_session->uniqueId()))
, _api(std::make_unique<MTP::Sender>(&_session->mtp()))
, _restoreTop(memento.scrollTop) {
	Settings::Register({ .key = _key, .kind = Settings::Kind::String,
		.defaultValue = QString(), .hidden = true });
	loadCategories();
	_back = Ui::CreateChild<Ui::IconButton>(this, st::backButton);
	_back->setAccessibleName(tr::lng_go_back(tr::now));
	_back->setClickedCallback([=] { this->controller()->showBackFromStack(); });
	_menuButton = Ui::CreateChild<Ui::IconButton>(this, st::menuToggle);
	_menuButton->setAccessibleName(Text("Управление лентой"));
	_menuButton->setClickedCallback([=] { showMenu(); });
	_category = Ui::CreateChild<Ui::RoundButton>(this,
		rpl::single(QString()), st::feedCategoryButton);
	_category->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	_category->setClickedCallback([=] { showCategories(); });
	_edit = Ui::CreateChild<Ui::LinkButton>(this, Text("Каналы"));
	_edit->setClickedCallback([=] { editCategory(false); });
	_scroll = Ui::CreateChild<Ui::ScrollArea>(this, st::feedScroll);
	_content = _scroll->setOwnedWidget(object_ptr<Ui::VerticalLayout>(_scroll));
	_empty = Ui::CreateChild<Ui::RpWidget>(this);
	_emptyTitle = Ui::CreateChild<Ui::FlatLabel>(_empty,
		rpl::single(QString()), st::feedEmptyTitle);
	_emptyHint = Ui::CreateChild<Ui::FlatLabel>(_empty,
		rpl::single(QString()), st::feedEmptyHint);
	_emptyAction = Ui::CreateChild<Ui::RoundButton>(_empty,
		rpl::single(Text("Добавить категорию")), st::feedAction);
	_emptyAction->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	_emptyAction->setClickedCallback([=] { editCategory(_groups.empty()); });
	style::PaletteChanged() | rpl::on_next([=] {
		update();
		_content->update();
		for (const auto child : _content->children()) {
			if (const auto widget = qobject_cast<QWidget*>(child)) {
				widget->update();
			}
		}
		_scroll->updateBars();
	}, lifetime());
	_session->downloaderTaskFinished() | rpl::on_next([=] {
		for (const auto child : _content->children()) {
			if (const auto widget = qobject_cast<QWidget*>(child)) {
				widget->update();
			}
		}
	}, lifetime());
	const auto position = std::find_if(_groups.begin(), _groups.end(),
		[&](const Category &group) { return group.id == memento.categoryId; });
	if (memento.snapshot && position != _groups.end()) {
		_selected = int(position - _groups.begin());
		updateCategory();
		_posts = memento.snapshot->posts;
		_sources = memento.snapshot->sources;
		_failures = memento.snapshot->failures;
		std::sort(_posts.begin(), _posts.end(), [](const Post &a, const Post &b) {
			return std::tie(a.date, a.peer, a.id) > std::tie(b.date, b.peer, b.id);
		});
		renderPosts();
	} else {
		populateCategories(position == _groups.end()
			? 0 : int(position - _groups.begin()));
	}
}

bool FeedSection::showInternal(
		not_null<Window::SectionMemento*> memento,
		const Window::SectionShow &params) {
	const auto feed = dynamic_cast<FeedMemento*>(memento.get());
	if (!feed) {
		return false;
	}
	if (feed->categoryId.isEmpty()) {
		return true;
	}
	const auto position = std::find_if(_groups.begin(), _groups.end(),
		[&](const Category &group) { return group.id == feed->categoryId; });
	const auto selected = position == _groups.end()
		? 0 : int(position - _groups.begin());
	if (selected != _selected) {
		_restoreTop = feed->scrollTop;
		populateCategories(selected);
	} else if (!feed->categoryId.isEmpty()) {
		_scroll->scrollToY(feed->scrollTop);
	}
	return true;
}

std::shared_ptr<Window::SectionMemento> FeedSection::createMemento() {
	return createIdentityMemento();
}

std::shared_ptr<Window::SectionMemento> FeedSection::createIdentityMemento() {
	const auto snapshot = _posts.empty() ? nullptr
		: std::make_shared<FeedSnapshot>(FeedSnapshot{
			.posts = _posts,
			.sources = _sources,
			.failures = _failures,
		});
	return std::make_shared<FeedMemento>(
		_selected >= 0 ? _groups[_selected].id : QString(),
		_scroll->scrollTop(), snapshot);
}

QPixmap FeedSection::grabForShowAnimation(
		const Window::SectionSlideParams &params) {
	syncVisibility();
	return Window::SectionWidget::grabForShowAnimation(params);
}

void FeedSection::showFinishedHook() {
	syncVisibility();
	updateLayout();
}

void FeedSection::syncVisibility() {
	_empty->setVisible(_showingEmpty);
	_scroll->setVisible(!_showingEmpty);
	_edit->setVisible(_selected >= 0);
}

bool FeedSection::floatPlayerHandleWheelEvent(QEvent *event) {
	return _scroll->viewportEvent(event);
}

QRect FeedSection::floatPlayerAvailableRect() {
	return QRect(_scroll->mapToGlobal(QPoint()), _scroll->size());
}

void FeedSection::paintEvent(QPaintEvent *event) {
	if (animatingShow()) {
		Window::SectionWidget::paintEvent(event);
		return;
	}
	auto p = Painter(this);
	p.fillRect(event->rect(), st::mainMenuBg);
	p.setFont(st::boxTitleFont);
	p.setPen(st::windowFg);
	p.drawTextLeft(st::feedTitleLeft, st::feedTitleTop, width(),
		st::boxTitleFont->elided(Text("Лента каналов"),
			width() - st::feedTitleLeft - st::feedTitleRight));
	p.setFont(st::feedMetaFont);
	p.setPen(st::windowSubTextFg);
	p.drawTextLeft(st::feedTitleLeft, st::feedSummaryTop, width(),
		st::feedMetaFont->elided(_summary,
			width() - st::feedTitleLeft - st::feedTitleRight));
	auto line = st::windowFg->c;
	line.setAlphaF(0.08);
	p.fillRect(0, st::feedHeaderHeight - st::lineWidth,
		width(), st::lineWidth, line);
}

void FeedSection::resizeEvent(QResizeEvent *event) {
	Window::SectionWidget::resizeEvent(event);
	updateLayout();
}

void FeedSection::keyPressEvent(QKeyEvent *event) {
	if (event->key() == Qt::Key_Escape) {
		controller()->showBackFromStack();
		return;
	}
	Window::SectionWidget::keyPressEvent(event);
}

void FeedSection::updateLayout() {
	_back->moveToLeft(st::feedControlInset, st::feedControlInset);
	_menuButton->moveToRight(st::feedControlInset, st::feedControlInset);
	_edit->moveToRight(st::feedPostPadding,
		st::feedFilterTop + (st::feedFilterHeight - _edit->height()) / 2);
	_category->setFullWidth(0);
	_category->setFullWidth(std::max(st::feedCategoryMinWidth,
		std::min(_category->width(),
			width() - 3 * st::feedPostPadding - _edit->width())));
	_category->moveToLeft(st::feedPostPadding,
		st::feedFilterTop + (st::feedFilterHeight - _category->height()) / 2);
	_scroll->setGeometry(0, st::feedHeaderHeight, width(),
		std::max(0, height() - st::feedHeaderHeight));
	_content->resizeToWidth(_scroll->width());
	_empty->setGeometry(_scroll->geometry());
	const auto textWidth = std::max(st::feedCategoryMinWidth,
		width() - 2 * st::feedEmptyPadding);
	_emptyTitle->resizeToWidth(textWidth);
	_emptyHint->resizeToWidth(textWidth);
	const auto total = _emptyTitle->height() + st::feedEmptyGap
		+ _emptyHint->height() + (_emptyAction->isHidden()
			? 0 : st::feedEmptyActionGap + _emptyAction->height());
	const auto top = std::max(st::feedPostPadding, (_empty->height() - total) / 2);
	_emptyTitle->moveToLeft(st::feedEmptyPadding, top);
	_emptyHint->moveToLeft(st::feedEmptyPadding,
		top + _emptyTitle->height() + st::feedEmptyGap);
	_emptyAction->move((width() - _emptyAction->width()) / 2,
		top + _emptyTitle->height() + st::feedEmptyGap
			+ _emptyHint->height() + st::feedEmptyActionGap);
}

void FeedSection::updateCategory() {
	_category->setText(rpl::single(_selected >= 0
		? _groups[_selected].name : Text("Категории")));
	_edit->setVisible(_selected >= 0);
	updateLayout();
	update();
}

void FeedSection::showCategories() {
	_menu = base::make_unique_q<Ui::PopupMenu>(this);
	for (auto i = 0; i != int(_groups.size()); ++i) {
		_menu->addAction(_groups[i].name, [=] {
			_restoreTop = 0;
			populateCategories(i);
		}, i == _selected ? &st::menuIconSelect : nullptr);
	}
	if (!_groups.empty()) {
		_menu->addSeparator();
	}
	_menu->addAction(Text("Добавить категорию"), [=] {
		editCategory(true);
	}, &st::menuIconAdd)->setEnabled(_groups.size() < kMaxCategories);
	_menu->popup(_category->mapToGlobal(QPoint(0, _category->height())));
}

void FeedSection::showMenu() {
	_menu = base::make_unique_q<Ui::PopupMenu>(this);
	_menu->addAction(Text("Обновить"), [=] { refresh(); }, &st::feedReloadIcon)
		->setEnabled(!_sources.empty() && !_busy);
	_menu->addAction(Text("Выбрать каналы"), [=] { editCategory(false); },
		&st::menuIconChannel)->setEnabled(_selected >= 0);
	_menu->addAction(Text("Добавить категорию"), [=] { editCategory(true); },
		&st::menuIconAdd)->setEnabled(_groups.size() < kMaxCategories);
	if (_selected >= 0) {
		_menu->addSeparator();
		const auto id = _groups[_selected].id;
		_menu->addAction(Text("Удалить категорию"), [=] {
			const auto position = std::find_if(_groups.begin(), _groups.end(),
				[&](const Category &group) { return group.id == id; });
			if (position == _groups.end()) {
				return;
			}
			_groups.erase(position);
			saveCategories();
			populateCategories(_selected);
		}, &st::menuIconDeleteAttention);
	}
	_menu->popup(_menuButton->mapToGlobal(QPoint(0, _menuButton->height())));
}

void FeedSection::showEmpty(
		const QString &title,
		const QString &hint,
		bool canEdit) {
	_showingEmpty = true;
	_scroll->hide();
	_emptyTitle->setText(title);
	_emptyHint->setText(hint);
	_emptyAction->setVisible(canEdit);
	_emptyAction->setText(rpl::single(_groups.empty()
		? Text("Добавить категорию") : Text("Выбрать каналы")));
	_empty->show();
	updateLayout();
	update();
}

void FeedSection::renderPosts() {
	const auto scrollTop = _restoreTop
		? base::take(_restoreTop) : _scroll->scrollTop();
	_older = nullptr;
	_content->clear();
	for (const auto &post : _posts) {
		const auto row = _content->add(object_ptr<PostRow>(
			_content, _session, post, [=] {
				if (const auto peer = _session->data().peerLoaded(post.peer)) {
					controller()->showPeerHistory(peer, {}, MsgId(post.id));
				}
			}), style::al_justify);
		row->show();
	}
	const auto canLoad = _posts.size() < kMaxPosts && std::any_of(
		_sources.begin(), _sources.end(), [](const Source &source) {
			return !source.exhausted;
		});
	if (canLoad) {
		_older = _content->add(object_ptr<Ui::RoundButton>(_content,
			rpl::single(Text("Ещё сообщения")), st::feedMoreButton),
			st::feedMorePadding, style::al_center);
		_older->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		_older->setClickedCallback([=] { loadMore(); });
	}
	_content->resizeToWidth(_scroll->width());
	if (_posts.empty()) {
		showEmpty(Text("Пока нет публикаций"), _failures
			? Text("Не удалось загрузить каналы. Обновите ленту через меню")
			: Text("Новые сообщения выбранных каналов появятся здесь"), false);
	} else {
		_showingEmpty = false;
		_empty->hide();
		_scroll->show();
		_scroll->scrollToY(scrollTop);
	}
	_summary = _failures
		? Text("Сообщений: %1 · Ошибок загрузки: %2")
			.arg(_posts.size()).arg(_failures)
		: Text("Сообщений: %1 · Каналов: %2")
			.arg(_posts.size()).arg(_sources.size());
	update();
}

void FeedSection::loadCategories() {
	const auto bytes = Settings::String(_key).toUtf8();
	if (bytes.size() > 65536) return;
	const auto groups = QJsonDocument::fromJson(bytes).array();
	for (const auto &value : groups) {
		if (_groups.size() == kMaxCategories) break;
		const auto object = value.toObject();
		auto category = Category{
			.id = object.value(u"id"_q).toString().left(64),
			.name = object.value(u"name"_q).toString().trimmed().left(80),
		};
		if (category.id.isEmpty() || category.name.isEmpty()) continue;
		for (const auto &id : object.value(u"channels"_q).toArray()) {
			auto ok = false;
			const auto raw = id.toString().toULongLong(&ok);
			const auto peer = PeerId(raw);
			if (ok && raw && std::find(category.channels.begin(),
				category.channels.end(), peer) == category.channels.end()) {
				category.channels.push_back(peer);
				if (category.channels.size() == kMaxChannels) break;
			}
		}
		_groups.push_back(std::move(category));
	}
}

void FeedSection::saveCategories() {
	auto groups = QJsonArray();
	for (const auto &category : _groups) {
		auto channels = QJsonArray();
		for (const auto id : category.channels) channels.push_back(QString::number(id.value));
		groups.push_back(QJsonObject{
			{ u"id"_q, category.id }, { u"name"_q, category.name },
			{ u"channels"_q, channels },
		});
	}
	Settings::SetString(_key,
		QString::fromUtf8(QJsonDocument(groups).toJson(QJsonDocument::Compact)));
}

void FeedSection::populateCategories(int selected) {
	_selected = _groups.empty() ? -1
		: std::clamp(selected, 0, int(_groups.size()) - 1);
	updateCategory();
	refresh();
}

void FeedSection::editCategory(bool create) {
	if (_editor || (create && _groups.size() >= kMaxCategories)
		|| (!create && _selected < 0)) {
		return;
	}
	const auto category = create ? Category{
		.id = QUuid::createUuid().toString(QUuid::WithoutBraces),
	} : _groups[_selected];
	auto list = std::make_unique<ChannelsController>(_session, category);
	const auto listController = list.get();
	const auto weak = QPointer(this);
	controller()->show(Box<PeerListBox>(std::move(list),
		[=](not_null<PeerListBox*> box) {
			if (!weak) {
				box->closeBox();
				return;
			}
			weak->_editor = box;
			box->setStyle(st::feedEditorBox);
			const auto header = Ui::CreateChild<Ui::VerticalLayout>(box.get());
			header->paintOn([=](QPainter &p) {
				p.fillRect(header->rect(), st::mainMenuBg);
			});
			const auto name = header->add(object_ptr<Ui::InputField>(header,
				st::feedNameField, rpl::single(Text("Название категории")),
				category.name), st::feedEditorPadding);
			name->setMaxLength(80);
			listController->setNameField(name);
			header->add(object_ptr<Ui::FlatLabel>(header,
				rpl::single(Text("Выберите до 32 каналов")), st::feedHint),
				st::feedEditorHintPadding);
			box->widthValue() | rpl::on_next([=](int width) {
				header->resizeToWidth(width);
			}, header->lifetime());
			header->heightValue() | rpl::on_next([=](int height) {
				box->setAddedTopScrollSkip(height, true);
			}, header->lifetime());
			header->show();
			header->raise();
			if (create) {
				listController->setShowFinishedCallback([=] { name->setFocusFast(); });
			}
			box->addButton(tr::lng_settings_save(), [=] {
				if (!weak) {
					box->closeBox();
					return;
				}
				auto updated = category;
				updated.name = listController->categoryName();
				if (updated.name.isEmpty()) {
					listController->showNameError();
					return;
				}
				updated.channels.clear();
				for (const auto &peer : box->collectSelectedRows()) {
					updated.channels.push_back(peer->id);
				}
				if (updated.channels.size() > kMaxChannels) {
					return;
				}
				const auto position = std::find_if(weak->_groups.begin(),
					weak->_groups.end(), [&](const Category &group) {
						return group.id == updated.id;
					});
				auto selected = 0;
				if (position == weak->_groups.end()) {
					if (weak->_groups.size() >= kMaxCategories) {
						return;
					}
					weak->_groups.push_back(std::move(updated));
					selected = int(weak->_groups.size()) - 1;
				} else {
					selected = int(position - weak->_groups.begin());
					*position = std::move(updated);
				}
				weak->saveCategories();
				box->closeBox();
				weak->populateCategories(selected);
			});
			box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		}));
}

void FeedSection::refresh() {
	_api = std::make_unique<MTP::Sender>(&_session->mtp());
	_busy = false;
	_failures = 0;
	_sources.clear();
	_posts.clear();
	_older = nullptr;
	_content->clear();
	if (_selected >= 0) {
		for (const auto id : _groups[_selected].channels) {
			const auto peer = _session->data().peerLoaded(id);
			const auto channel = peer ? peer->asChannel() : nullptr;
			if (channel && channel->isBroadcast() && channel->amIn()) {
				_sources.push_back({ .peer = id });
			}
		}
	}
	if (_sources.empty()) {
		_summary.clear();
		showEmpty(Text("Выберите каналы для ленты"),
			Text("Объедините каналы в категорию, чтобы читать их вместе"), true);
	} else {
		loadMore();
	}
}

void FeedSection::loadMore() {
	if (_busy || _posts.size() >= kMaxPosts || _sources.empty()) {
		return;
	}
	_busy = true;
	if (_older) {
		_older->setDisabled(true);
		_older->setText(rpl::single(Text("Загрузка…")));
	}
	if (_posts.empty()) {
		showEmpty(Text("Загрузка публикаций"), QString(), false);
	}
	_source = 0;
	_failures = 0;
	loadNext();
}

void FeedSection::loadNext() {
	while (_source < _sources.size() && _sources[_source].exhausted) {
		++_source;
	}
	if (_source == _sources.size() || _posts.size() >= kMaxPosts) {
		_busy = false;
		std::sort(_posts.begin(), _posts.end(), [](const Post &a, const Post &b) {
			return std::tie(a.date, a.peer, a.id) > std::tie(b.date, b.peer, b.id);
		});
		renderPosts();
		return;
	}
	const auto peer = _session->data().peerLoaded(_sources[_source].peer);
	if (!peer) {
		++_source;
		loadNext();
		return;
	}
	_summary = Text("Загрузка: %1").arg(peer->name());
	update();
	_api->request(MTPmessages_GetHistory(peer->input(), MTP_int(_sources[_source].offset),
		MTP_int(0), MTP_int(0), MTP_int(kPageSize), MTP_int(0), MTP_int(0), MTP_long(0)
	)).done([=](const MTPmessages_Messages &result) {
		auto messages = QVector<MTPMessage>();
		result.match([&](const MTPDmessages_messages &data) { messages = data.vmessages().v; },
			[&](const MTPDmessages_messagesSlice &data) { messages = data.vmessages().v; },
			[&](const MTPDmessages_channelMessages &data) { messages = data.vmessages().v; },
			[&](const MTPDmessages_messagesNotModified &) {});
		auto &source = _sources[_source];
		auto lowest = source.offset;
		for (const auto &message : messages) {
			message.match([&](const MTPDmessage &data) {
				const auto id = data.vid().v;
				if (id <= 0) return;
				lowest = lowest ? std::min(lowest, id) : id;
				if (std::any_of(_posts.begin(), _posts.end(), [&](const Post &p) {
					return p.peer == source.peer && p.id == id;
				})) return;
				if (_posts.size() < kMaxPosts) _posts.push_back({ source.peer, id,
					data.vdate().v, data.vmessage().v.isEmpty() ? Text("[Медиа]")
						: qs(data.vmessage()).left(4000) });
			}, [&](const MTPDmessageService &data) {
				lowest = lowest ? std::min(lowest, data.vid().v) : data.vid().v;
			}, [&](const MTPDmessageEmpty &) {});
		}
		source.exhausted = messages.size() < kPageSize || !lowest || lowest == source.offset;
		source.offset = lowest;
		++_source;
		loadNext();
	}).fail([=](const MTP::Error &error) {
		++_failures;
		if (error.type().startsWith(u"FLOOD_WAIT"_q)) _source = _sources.size();
		else ++_source;
		loadNext();
	}).handleFloodErrors().send();
}

} // namespace

void Show(not_null<Window::SessionController*> controller) {
	controller->showSection(std::make_shared<FeedMemento>());
}

} // namespace Mokagram::Feed
