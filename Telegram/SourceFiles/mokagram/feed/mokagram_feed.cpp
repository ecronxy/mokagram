#include "mokagram/feed/mokagram_feed.h"

#include "data/data_channel.h"
#include "base/weak_ptr.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "mokagram/lang/mokagram_lang.h"
#include "mokagram/settings/mokagram_settings.h"
#include "mtproto/sender.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QSignalBlocker>
#include <QtCore/QUuid>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QVBoxLayout>

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

class FeedWindow final : public QMainWindow {
public:
	explicit FeedWindow(not_null<Window::SessionController*> controller)
	: _controller(base::make_weak(controller.get()))
	, _session(&controller->session())
	, _key(u"feed.%1.categories"_q.arg(_session->uniqueId()))
	, _api(std::make_unique<MTP::Sender>(&_session->mtp())) {
		setAttribute(Qt::WA_DeleteOnClose);
		setWindowTitle(Text("Лента каналов") + u" — mokaGram"_q);
		resize(st::boxWideWidth * 2, st::boxWideWidth * 2);
		Settings::Register({ .key = _key, .kind = Settings::Kind::String,
			.defaultValue = QString(), .hidden = true });
		loadCategories();
		const auto toolbar = addToolBar(Text("Лента каналов"));
		toolbar->setMovable(false);
		_categories = new QComboBox(toolbar);
		toolbar->addWidget(_categories);
		connect(_categories, &QComboBox::currentIndexChanged, this,
			[=] { refresh(); });
		const auto add = toolbar->addAction(Text("Добавить категорию"));
		connect(add, &QAction::triggered, this, [=] { editCategory(true); });
		const auto edit = toolbar->addAction(Text("Выбрать каналы"));
		connect(edit, &QAction::triggered, this, [=] { editCategory(false); });
		const auto remove = toolbar->addAction(Text("Удалить категорию"));
		connect(remove, &QAction::triggered, this, [=] {
			const auto index = _categories->currentIndex();
			if (index < 0 || index >= int(_groups.size())) return;
			_groups.erase(_groups.begin() + index);
			saveCategories();
			populateCategories();
		});
		const auto reload = toolbar->addAction(Text("Обновить"));
		connect(reload, &QAction::triggered, this, [=] { refresh(); });
		_older = toolbar->addAction(Text("Ещё сообщения"));
		connect(_older, &QAction::triggered, this, [=] { loadMore(); });
		_list = new QListWidget(this);
		_list->setWordWrap(true);
		_list->setAccessibleName(Text("Лента каналов"));
		_list->setToolTip(Text("Двойной щелчок открывает сообщение в канале."));
		setCentralWidget(_list);
		connect(_list, &QListWidget::itemActivated, this, [=](QListWidgetItem *item) {
			const auto peer = _session->data().peerLoaded(
				PeerId(item->data(Qt::UserRole).toULongLong()));
			if (peer && _controller) _controller->showPeerHistory(peer, {},
				MsgId(item->data(Qt::UserRole + 1).toLongLong()));
		});
		_session->lifetime().add([weak = QPointer(this)] {
			if (weak) {
				weak->_api.reset();
				weak->close();
			}
		});
		populateCategories();
	}

private:
	struct Source {
		PeerId peer;
		int offset = 0;
		bool exhausted = false;
	};
	struct Post {
		PeerId peer;
		int id = 0;
		int date = 0;
		QString text;
	};

	void loadCategories() {
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

	void saveCategories() {
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

	void populateCategories(int selected = 0) {
		{
			const auto block = QSignalBlocker(_categories);
			_categories->clear();
			for (const auto &group : _groups) _categories->addItem(group.name);
			_categories->setCurrentIndex(_groups.empty() ? -1
				: std::clamp(selected, 0, int(_groups.size()) - 1));
		}
		refresh();
	}

	void editCategory(bool create) {
		const auto index = _categories->currentIndex();
		if ((create && _groups.size() >= kMaxCategories)
			|| (!create && (index < 0 || index >= int(_groups.size())))) return;
		const auto category = create ? Category{
			.id = QUuid::createUuid().toString(QUuid::WithoutBraces),
		} : _groups[index];
		const auto dialog = new QDialog(this);
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->setWindowTitle(Text("Выбрать каналы"));
		dialog->resize(st::boxWideWidth, st::boxWideWidth * 2);
		const auto layout = new QVBoxLayout(dialog);
		const auto name = new QLineEdit(category.name, dialog);
		name->setPlaceholderText(Text("Название категории"));
		name->setMaxLength(80);
		layout->addWidget(name);
		layout->addWidget(new QLabel(Text("До 32 выбранных каналов из загруженного списка чатов."), dialog));
		const auto channels = new QListWidget(dialog);
		layout->addWidget(channels);
		_session->data().enumerateBroadcasts([&](not_null<ChannelData*> channel) {
			if (!channel->amIn()) return;
			const auto row = new QListWidgetItem(channel->name(), channels);
			row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
			row->setData(Qt::UserRole, qulonglong(channel->id.value));
			row->setCheckState(std::find(category.channels.begin(), category.channels.end(),
				channel->id) != category.channels.end() ? Qt::Checked : Qt::Unchecked);
		});
		channels->sortItems();
		const auto hint = new QLabel(dialog);
		layout->addWidget(hint);
		const auto buttons = new QDialogButtonBox(
			QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
		layout->addWidget(buttons);
		connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
		connect(buttons, &QDialogButtonBox::accepted, this, [=] {
			auto updated = category;
			updated.name = name->text().trimmed();
			updated.channels.clear();
			for (auto i = 0; i < channels->count(); ++i) {
				const auto row = channels->item(i);
				if (row->checkState() == Qt::Checked) {
					updated.channels.push_back(PeerId(row->data(Qt::UserRole).toULongLong()));
				}
			}
			if (updated.name.isEmpty() || updated.channels.size() > kMaxChannels) {
				hint->setText(Text("Укажите название и выберите не больше 32 каналов."));
				return;
			}
			const auto position = std::find_if(_groups.begin(), _groups.end(),
				[&](const Category &group) { return group.id == updated.id; });
			if (position == _groups.end()) _groups.push_back(std::move(updated));
			else *position = std::move(updated);
			saveCategories();
			dialog->accept();
			populateCategories(create ? int(_groups.size()) - 1 : index);
		});
		dialog->open();
	}

	void refresh() {
		_api = std::make_unique<MTP::Sender>(&_session->mtp());
		_busy = false;
		_sources.clear();
		_posts.clear();
		_list->clear();
		const auto index = _categories->currentIndex();
		if (index >= 0 && index < int(_groups.size())) {
			for (const auto id : _groups[index].channels) {
				const auto peer = _session->data().peerLoaded(id);
				const auto channel = peer ? peer->asChannel() : nullptr;
				if (channel && channel->isBroadcast() && channel->amIn()) {
					_sources.push_back({ .peer = id });
				}
			}
		}
		if (_sources.empty()) {
			_older->setEnabled(false);
			statusBar()->showMessage(Text("Создайте категорию и выберите каналы."));
		} else loadMore();
	}

	void loadMore() {
		if (_busy || _posts.size() >= kMaxPosts) return;
		_busy = true;
		_older->setEnabled(false);
		_source = 0;
		_failures = 0;
		loadNext();
	}

	void loadNext() {
		while (_source < _sources.size() && _sources[_source].exhausted) ++_source;
		if (_source == _sources.size() || _posts.size() >= kMaxPosts) {
			_busy = false;
			std::sort(_posts.begin(), _posts.end(), [](const Post &a, const Post &b) {
				return std::tie(a.date, a.peer, a.id) > std::tie(b.date, b.peer, b.id);
			});
			_list->clear();
			for (const auto &post : _posts) {
				const auto peer = _session->data().peerLoaded(post.peer);
				const auto title = peer ? peer->name() : QString();
				const auto date = QDateTime::fromSecsSinceEpoch(post.date)
					.toLocalTime().toString(u"dd.MM HH:mm"_q);
				const auto row = new QListWidgetItem(title + u" · "_q + date
					+ '\n' + post.text + '\n', _list);
				row->setData(Qt::UserRole, qulonglong(post.peer.value));
				row->setData(Qt::UserRole + 1, post.id);
			}
			_older->setEnabled(_posts.size() < kMaxPosts && std::any_of(
				_sources.begin(), _sources.end(), [](const Source &s) { return !s.exhausted; }));
			statusBar()->showMessage(Text("Сообщений: %1 · Ошибок загрузки: %2")
				.arg(_posts.size()).arg(_failures));
			return;
		}
		const auto peer = _session->data().peerLoaded(_sources[_source].peer);
		if (!peer) { ++_source; loadNext(); return; }
		statusBar()->showMessage(Text("Загрузка: %1").arg(peer->name()));
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

	base::weak_ptr<Window::SessionController> _controller;
	not_null<Main::Session*> _session;
	QString _key;
	std::unique_ptr<MTP::Sender> _api;
	QComboBox *_categories = nullptr;
	QListWidget *_list = nullptr;
	QAction *_older = nullptr;
	std::vector<Category> _groups;
	std::vector<Source> _sources;
	std::vector<Post> _posts;
	size_t _source = 0;
	int _failures = 0;
	bool _busy = false;

};

} // namespace

void Show(not_null<Window::SessionController*> controller) {
	const auto window = new FeedWindow(controller);
	window->show();
	window->raise();
	window->activateWindow();
}

} // namespace Mokagram::Feed
