#include "mokagram/music/mokagram_music_ui.h"

#include "mokagram/music/mokagram_soundcloud.h"
#include "mokagram/music/mokagram_music_library.h"
#include "mokagram/music/mokagram_lyrics.h"
#include "mokagram/settings/mokagram_settings.h"

#include "base/unixtime.h"
#include "mainwindow.h"
#include "base/random.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "data/data_media_types.h"
#include "data/data_saved_music.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "mokagram/plugins/mokagram_plugin_install.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_peer_menu.h"
#include "styles/style_menu_icons.h"
#include "storage/localimageloader.h"

#include "core/file_location.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QLinearGradient>
#include <QtGui/QRadialGradient>
#include <QtGui/QGuiApplication>
#include <QtCore/QPointer>

namespace Mokagram::Music {
namespace {

constexpr auto kRowHeight = 76;
constexpr auto kArtSize = 56;

struct Queue {
	std::vector<Track> tracks;       // очередь воспроизведения
	std::vector<Track> results;      // то, что сейчас показано в списке
	std::vector<Track> searchResults;
	std::vector<Track> forYou;
	QString forYouNote;
	int tab = 0;                     // 0 поиск, 1 лайкнутые, 2 для вас
	Track current;                   // играющий трек (с разобранными ссылками)
	bool recorded = true;            // прослушивание уже записано в историю
	QString lastQuery;
	bool custom = false;             // очередь собрана вручную
	int index = -1;
	QString currentId;
	int pendingToken = 0;
	rpl::lifetime lifetime;
	rpl::event_stream<> changes;
	base::weak_ptr<Window::SessionController> controller;
	bool subscribed = false;
};

Queue &QueueState() {
	static auto queue = Queue();
	return queue;
}

[[nodiscard]] QString FormatDuration(int ms) {
	const auto seconds = ms / 1000;
	return u"%1:%2"_q.arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
}

[[nodiscard]] QString ArtUrl(const QString &url, const QString &size) {
	auto result = url;
	result.replace(u"-large."_q, u"-"_q + size + u"."_q);
	return result;
}

[[nodiscard]] not_null<DocumentData*> MakeDocument(
		not_null<Main::Session*> session,
		const Track &track,
		const QString &path) {
	const auto size = QFileInfo(path).size();
	auto attributes = QVector<MTPDocumentAttribute>();
	attributes.push_back(MTP_documentAttributeAudio(
		MTP_flags(MTPDdocumentAttributeAudio::Flag::f_title
			| MTPDdocumentAttributeAudio::Flag::f_performer),
		MTP_int(track.durationMs / 1000),
		MTP_string(track.title),
		MTP_string(track.artist),
		MTPstring()));
	attributes.push_back(MTP_documentAttributeFilename(
		MTP_string(track.title + '.' + track.extension)));
	const auto id = DocumentId(0x4D4F4B0000000000ULL
		+ uint64(track.id.toULongLong()));
	const auto document = session->data().document(
		id,
		0,
		QByteArray(),
		base::unixtime::now(),
		attributes,
		track.extension == u"mp3"_q ? u"audio/mpeg"_q : u"audio/mp4"_q,
		InlineImageLocation(),
		ImageWithLocation(),
		ImageWithLocation(),
		false,
		0,
		size);
	document->setLocation(Core::FileLocation(path));
	return document;
}

void PlayIndex(int index);

void ResolveThen(const Track &track, Fn<void(Track, QString)> done) {
	if (!track.transcodingUrl.isEmpty()) {
		done(track, QString());
		return;
	}
	Resolve(track, std::move(done));
}

// Записывает в историю то, что успели послушать, до перехода на другой трек.
void RecordCurrentListen(bool finished) {
	auto &queue = QueueState();
	if (queue.recorded || queue.current.id.isEmpty()) {
		return;
	}
	queue.recorded = true;
	auto listened = queue.current.durationMs;
	if (!finished) {
		const auto state = Media::Player::instance()->getState(AudioMsgId::Type::Song);
		const auto frequency = std::max<int64>(state.frequency, 1);
		listened = int(state.position * 1000 / frequency);
	}
	RecordListen(queue.current, listened, finished);
}

void Subscribe() {
	auto &queue = QueueState();
	if (queue.subscribed) {
		return;
	}
	queue.subscribed = true;
	Media::Player::instance()->updatedNotifier(
	) | rpl::on_next([=](const Media::Player::TrackState &state) {
		auto &queue = QueueState();
		if (queue.currentId.isEmpty()
			|| state.id.type() != AudioMsgId::Type::Song
			|| !state.id.audio()
			|| state.id.audio()->id != DocumentId(0x4D4F4B0000000000ULL
				+ uint64(queue.currentId.toULongLong()))) {
			return;
		}
		if (Media::Player::IsStoppedAtEnd(state.state)) {
			RecordCurrentListen(true);
			const auto count = int(queue.tracks.size());
			const auto &settings = Core::App().settings();
			if (settings.playerRepeatMode() == Media::RepeatMode::One) {
				PlayIndex(queue.index);
			} else if (settings.playerOrderMode() == Media::OrderMode::Shuffle
				&& count > 1) {
				auto next = queue.index;
				while (next == queue.index) {
					next = base::RandomIndex(count);
				}
				PlayIndex(next);
			} else if (queue.index + 1 < count) {
				PlayIndex(queue.index + 1);
			} else if (settings.playerRepeatMode() == Media::RepeatMode::All
				&& count > 0) {
				PlayIndex(0);
			}
		}
	}, queue.lifetime);
}

void PlayIndex(int index) {
	auto &queue = QueueState();
	const auto controller = queue.controller.get();
	if (!controller || index < 0 || index >= int(queue.tracks.size())) {
		return;
	}
	Subscribe();
	RecordCurrentListen(false);
	const auto track = queue.tracks[index];
	const auto token = ++queue.pendingToken;
	queue.index = index;
	queue.currentId = track.id;
	queue.changes.fire({});
	controller->showToast(QString::fromUtf8("Загружаю: ") + track.title);
	ResolveThen(track, [=](Track resolved, QString error) {
		auto &queue = QueueState();
		const auto controller = queue.controller.get();
		if (!controller || token != queue.pendingToken) {
			return;
		}
		if (!error.isEmpty()) {
			controller->showToast(QString::fromUtf8("Не удалось загрузить: ") + error);
			return;
		}
		if (index < int(queue.tracks.size()) && queue.tracks[index].id == resolved.id) {
			queue.tracks[index] = resolved;
		}
		Download(resolved, [=](const QString &path, const QString &error) {
			auto &queue = QueueState();
			const auto controller = queue.controller.get();
			if (!controller || token != queue.pendingToken) {
				return;
			}
			if (!error.isEmpty()) {
				controller->showToast(QString::fromUtf8("Не удалось загрузить: ") + error);
				return;
			}
			const auto document = MakeDocument(&controller->session(), resolved, path);
			LoadImage(ArtUrl(resolved.artworkUrl, u"t500x500"_q), [id = document->id](QImage image) {
				SetCover(id, std::move(image));
				QueueState().changes.fire({});
			});
			queue.current = resolved;
			queue.recorded = false;
			Media::Player::instance()->play(
				AudioMsgId(document, FullMsgId()),
				std::nullopt);
			// Следующий трек качаем заранее, чтобы переход был без паузы.
			if (index + 1 < int(queue.tracks.size())) {
				ResolveThen(queue.tracks[index + 1], [](Track next, QString error) {
					if (error.isEmpty()) {
						Download(next, [](const QString &, const QString &) {});
					}
				});
			}
		});
	});
}


[[nodiscard]] QString SafeName(const Track &track) {
	auto name = track.artist + u" - "_q + track.title;
	name.replace(QRegularExpression(u"[\\\\/:*?\"<>|]"_q), u"_"_q);
	return name.left(120).trimmed();
}

// Копия трека с понятным именем: оно уйдёт в Telegram как название файла.
[[nodiscard]] QString NamedCopy(const Track &track, const QString &path) {
	const auto dir = CacheDir() + u"/send"_q;
	QDir().mkpath(dir);
	const auto named = dir + '/' + SafeName(track) + '.' + track.extension;
	QFile::remove(named);
	QFile::copy(path, named);
	return named;
}

void EnsureFile(
		const Track &track,
		Fn<void(const QString &, const Track &)> done) {
	const auto controller = QueueState().controller.get();
	ResolveThen(track, [=](Track resolved, QString error) {
		if (!error.isEmpty()) {
			if (const auto controller = QueueState().controller.get()) {
				controller->showToast(QString::fromUtf8("Не удалось загрузить: ") + error);
			}
			return;
		}
		Download(resolved, [=](const QString &path, const QString &error) {
			if (!error.isEmpty()) {
				if (const auto controller = QueueState().controller.get()) {
					controller->showToast(QString::fromUtf8("Не удалось загрузить: ") + error);
				}
				return;
			}
			done(path, resolved);
		});
	});
	if (controller) {
		controller->showToast(QString::fromUtf8("Загружаю: ") + track.title);
	}
}

void SendTrack(
		not_null<History*> history,
		const Track &track,
		const QString &path) {
	const auto session = &history->session();
	auto list = Storage::PrepareMediaList(
		QStringList{ NamedCopy(track, path) },
		st::sendMediaPreviewSize,
		session->user()->isPremium());
	if (list.files.empty() && list.filesToProcess.empty()) {
		return;
	}
	auto action = Api::SendAction(history);
	action.clearDraft = false;
	session->api().sendFiles(
		std::move(list),
		SendMediaType::File,
		nullptr,
		std::move(action));
}

void SaveToDisk(const Track &track) {
	EnsureFile(track, [=](const QString &path, const Track &track) {
		auto dir = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
		if (dir.isEmpty()) {
			dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
		}
		QDir().mkpath(dir);
		const auto target = dir + '/' + SafeName(track) + '.' + track.extension;
		QFile::remove(target);
		const auto controller = QueueState().controller.get();
		if (QFile::copy(path, target)) {
			if (controller) {
				controller->showToast(QString::fromUtf8("Сохранено: ") + target, 4000);
			}
		} else if (controller) {
			controller->showToast(QString::fromUtf8("Не удалось сохранить файл"));
		}
	});
}

void ForwardTrack(
		not_null<Window::SessionController*> controller,
		const Track &track) {
	Window::ShowChooseRecipientBox(
		controller,
		[=](not_null<Data::Thread*> thread) {
			const auto history = thread->owningHistory();
			EnsureFile(track, [=](const QString &path, const Track &track) {
				SendTrack(history, track, path);
			});
			return true;
		},
		rpl::single(QString::fromUtf8("Переслать трек")));
}

// Профильная музыка Telegram хранится в документах: заливаем файл в «Избранное»
// и, когда он загрузился, сохраняем в профиль.
void AddToProfile(
		not_null<Window::SessionController*> controller,
		const Track &track) {
	const auto session = &controller->session();
	const auto weak = base::make_weak(session);
	EnsureFile(track, [=](const QString &path, const Track &track) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto history = strong->data().history(strong->user());
		SendTrack(history, track, path);
		const auto name = SafeName(track) + '.' + track.extension;
		const auto timer = new QTimer();
		const auto tries = std::make_shared<int>(0);
		timer->setInterval(1500);
		QObject::connect(timer, &QTimer::timeout, [=] {
			const auto session = weak.get();
			const auto finish = [&] {
				timer->stop();
				timer->deleteLater();
			};
			if (!session || ++*tries > 80) {
				finish();
				return;
			}
			const auto history = session->data().history(session->user());
			for (const auto &block : history->blocks) {
				for (const auto &view : block->messages) {
					const auto item = view->data();
					const auto document = item->media()
						? item->media()->document()
						: nullptr;
					if (!document
						|| item->isSending()
						|| item->hasFailed()
						|| document->filename() != name) {
						continue;
					}
					session->data().savedMusic().save(
						document,
						Data::FileOrigin(item->fullId()));
					if (const auto controller = QueueState().controller.get()) {
						controller->showToast(QString::fromUtf8("Добавлено в профиль: ") + track.title);
					}
					finish();
					return;
				}
			}
		});
		timer->start();
	});
}


// Клик по треку из результатов поиска.
void PlayFromResults(int resultIndex) {
	auto &queue = QueueState();
	if (resultIndex < 0 || resultIndex >= int(queue.results.size())) {
		return;
	}
	const auto track = queue.results[resultIndex];
	if (!queue.custom) {
		queue.tracks = queue.results;
		PlayIndex(resultIndex);
		return;
	}
	for (auto i = 0; i != int(queue.tracks.size()); ++i) {
		if (queue.tracks[i].id == track.id) {
			PlayIndex(i);
			return;
		}
	}
	const auto at = std::min<int>(queue.index + 1, queue.tracks.size());
	queue.tracks.insert(queue.tracks.begin() + at, track);
	PlayIndex(at);
}

void EnqueueNext(const Track &track) {
	auto &queue = QueueState();
	queue.custom = true;
	const auto at = std::min<int>(queue.index + 1, queue.tracks.size());
	queue.tracks.insert(queue.tracks.begin() + at, track);
	queue.changes.fire({});
}

void EnqueueLast(const Track &track) {
	auto &queue = QueueState();
	queue.custom = true;
	queue.tracks.push_back(track);
	queue.changes.fire({});
}

void QueueMove(int index, int delta) {
	auto &queue = QueueState();
	const auto target = index + delta;
	if (index < 0 || index >= int(queue.tracks.size())
		|| target < 0 || target >= int(queue.tracks.size())) {
		return;
	}
	std::swap(queue.tracks[index], queue.tracks[target]);
	if (queue.index == index) {
		queue.index = target;
	} else if (queue.index == target) {
		queue.index = index;
	}
	queue.custom = true;
	queue.changes.fire({});
}

void QueueRemove(int index) {
	auto &queue = QueueState();
	if (index < 0 || index >= int(queue.tracks.size())) {
		return;
	}
	queue.tracks.erase(queue.tracks.begin() + index);
	if (queue.index > index) {
		--queue.index;
	} else if (queue.index == index) {
		queue.index = -1;
	}
	queue.custom = true;
	queue.changes.fire({});
}

void DrawHeart(QPainter &p, const QRectF &r, bool filled, const QColor &color) {
	auto path = QPainterPath();
	const auto w = r.width();
	const auto h = r.height();
	const auto x = r.x();
	const auto y = r.y();
	path.moveTo(x + w * 0.5, y + h * 0.92);
	path.cubicTo(x - w * 0.15, y + h * 0.55, x + w * 0.05, y + h * 0.05, x + w * 0.5, y + h * 0.30);
	path.cubicTo(x + w * 0.95, y + h * 0.05, x + w * 1.15, y + h * 0.55, x + w * 0.5, y + h * 0.92);
	p.save();
	if (filled) {
		p.setPen(Qt::NoPen);
		p.setBrush(color);
	} else {
		p.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.setBrush(Qt::NoBrush);
	}
	p.drawPath(path);
	p.restore();
}

[[nodiscard]] const QColor &HeartColor() {
	static const auto color = QColor(255, 77, 109);
	return color;
}

class TabPill final : public Ui::AbstractButton {
public:
	TabPill(QWidget *parent, const QString &text)
	: AbstractButton(parent)
	, _text(text) {
		resize(width(), 36);
		setPointerCursor(true);
		paintRequest(
		) | rpl::on_next([=](QRect) {
			auto p = QPainter(this);
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			auto tone = QColor(st::windowActiveTextFg->c);
			tone.setAlpha(_active ? 60 : 0);
			p.setBrush(_active ? tone : (isOver() ? st::windowBgOver : st::windowBg)->c);
			p.drawRoundedRect(rect(), height() / 2., height() / 2.);
			p.setFont(st::semiboldFont);
			p.setPen(_active ? st::windowActiveTextFg : st::windowSubTextFg);
			p.drawText(rect(), Qt::AlignCenter, _text);
		}, lifetime());
	}
	void setActive(bool active) {
		if (_active != active) {
			_active = active;
			update();
		}
	}

private:
	QString _text;
	bool _active = false;

};

class PicksBar final : public Ui::AbstractButton {
public:
	PicksBar(QWidget *parent, Fn<void()> play, Fn<void()> refresh)
	: AbstractButton(parent) {
		resize(width(), 36);
		setPointerCursor(true);
		setClickedCallback([=] {
			if (refreshRect().contains(mapFromGlobal(QCursor::pos()))) {
				refresh();
			} else {
				play();
			}
		});
		paintRequest(
		) | rpl::on_next([=](QRect) {
			auto p = QPainter(this);
			// Только текст, без значков и плашек.
			const auto onRefresh = refreshRect().contains(
				mapFromGlobal(QCursor::pos()));
			p.setFont(st::semiboldFont);
			p.setPen(onRefresh || !isOver()
				? st::windowActiveTextFg
				: st::windowFg);
			p.drawText(
				QRect(8, 0, width() - 8 - 110, height()),
				Qt::AlignVCenter | Qt::AlignLeft,
				QString::fromUtf8("Слушать подборку"));
			p.setFont(st::normalFont);
			p.setPen(onRefresh ? st::windowFg : st::windowSubTextFg);
			p.drawText(
				refreshRect(),
				Qt::AlignVCenter | Qt::AlignRight,
				QString::fromUtf8("Обновить"));
		}, lifetime());
	}

private:
	[[nodiscard]] QRect refreshRect() const {
		return QRect(width() - 110, 0, 110 - 8, height());
	}

};

void PlayAll(const std::vector<Track> &tracks) {
	auto &queue = QueueState();
	if (tracks.empty()) {
		return;
	}
	queue.custom = false;
	queue.tracks = tracks;
	PlayIndex(0);
}

enum class RowMode { Results, Queue };

class Row final : public Ui::AbstractButton {
public:
	Row(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		const Track &track,
		int index,
		RowMode mode = RowMode::Results)
	: AbstractButton(parent)
	, _track(track)
	, _mode(mode) {
		resize(width(), kRowHeight);
		setPointerCursor(true);
		LoadImage(ArtUrl(track.artworkUrl, u"t200x200"_q), crl::guard(this, [=](QImage image) {
			_art = std::move(image);
			update();
		}));
		QueueState().changes.events(
		) | rpl::on_next([=] {
			update();
		}, lifetime());
		LikesChanged(
		) | rpl::on_next([=] {
			update();
		}, lifetime());
		setClickedCallback([=] {
			if (heartRect().contains(mapFromGlobal(QCursor::pos()))) {
				SetLiked(_track, !IsLiked(_track.id));
				return;
			}
			if (_mode == RowMode::Queue) {
				PlayIndex(index);
			} else {
				PlayFromResults(index);
			}
		});
		paintRequest(
		) | rpl::on_next([=](QRect) {
			paint();
		}, lifetime());
		events(
		) | rpl::filter([](not_null<QEvent*> e) {
			return e->type() == QEvent::ContextMenu;
		}) | rpl::on_next([=](not_null<QEvent*>) {
			showMenu(controller, index);
		}, lifetime());
	}

private:
	[[nodiscard]] QRect heartRect() const {
		const auto durationWidth = st::normalFont->width(FormatDuration(_track.durationMs));
		return QRect(
			width() - 24 - durationWidth - 14 - 28,
			(height() - 28) / 2,
			28,
			28);
	}

	void showMenu(not_null<Window::SessionController*> controller, int index) {
		_menu = base::make_unique_q<Ui::PopupMenu>(
			this,
			st::popupMenuWithIcons);
		const auto track = _track;
		const auto add = [&](const QString &text, Fn<void()> callback, const style::icon *icon) {
			_menu->addAction(text, std::move(callback), icon);
		};
		add(QString::fromUtf8("Воспроизвести"), [=] {
			if (_mode == RowMode::Queue) {
				PlayIndex(index);
			} else {
				PlayFromResults(index);
			}
		}, &st::menuIconSoundOn);
		add(IsLiked(track.id)
			? QString::fromUtf8("Убрать из избранного")
			: QString::fromUtf8("В избранное"), [=] {
			SetLiked(track, !IsLiked(track.id));
		}, &st::menuIconFave);
		if (_mode == RowMode::Results) {
			add(QString::fromUtf8("Играть следующим"), [=] { EnqueueNext(track); controller->showToast(QString::fromUtf8("Добавлено в очередь")); }, &st::menuIconAbove);
			add(QString::fromUtf8("В конец очереди"), [=] { EnqueueLast(track); controller->showToast(QString::fromUtf8("Добавлено в очередь")); }, &st::menuIconBelow);
		} else {
			add(QString::fromUtf8("Выше"), [=] { QueueMove(index, -1); }, &st::menuIconAbove);
			add(QString::fromUtf8("Ниже"), [=] { QueueMove(index, 1); }, &st::menuIconBelow);
			add(QString::fromUtf8("Убрать из очереди"), [=] { QueueRemove(index); }, &st::menuIconDelete);
		}
		add(QString::fromUtf8("Скачать"), [=] { SaveToDisk(track); }, &st::menuIconDownload);
		add(QString::fromUtf8("Переслать"), [=] { ForwardTrack(controller, track); }, &st::menuIconForward);
		add(QString::fromUtf8("Добавить в профиль"), [=] { AddToProfile(controller, track); }, &st::menuIconProfile);
		add(QString::fromUtf8("Копировать ссылку"), [=] {
			QGuiApplication::clipboard()->setText(track.permalink);
			controller->showToast(QString::fromUtf8("Ссылка скопирована"));
		}, &st::menuIconLink);
		_menu->popup(QCursor::pos());
	}

	void paint() {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto playing = (QueueState().currentId == _track.id);
		const auto over = isOver() || isDown();
		if (playing || over) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(rect().marginsRemoved({ 10, 3, 10, 3 }), 18, 18);
		}
		const auto art = QRect(20, (height() - kArtSize) / 2, kArtSize, kArtSize);
		auto path = QPainterPath();
		path.addRoundedRect(art, 14, 14);
		if (!_art.isNull()) {
			p.save();
			p.setClipPath(path);
			p.drawImage(art, _art);
			if (playing) {
				p.fillRect(art, QColor(0, 0, 0, 120));
			}
			p.restore();
		} else {
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawPath(path);
		}
		if (playing) {
			// Три столбика «играет».
			p.setPen(Qt::NoPen);
			p.setBrush(Qt::white);
			const auto base = art.center().y() + 9;
			const auto left = art.center().x() - 10;
			const int heights[] = { 12, 20, 15 };
			for (auto i = 0; i != 3; ++i) {
				p.drawRoundedRect(QRect(left + i * 8, base - heights[i], 4, heights[i]), 2, 2);
			}
		}
		const auto left = art.right() + 16;
		const auto right = width() - 24;
		const auto duration = FormatDuration(_track.durationMs);
		const auto durationWidth = st::normalFont->width(duration);
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(right - durationWidth, 0, durationWidth, height()),
			Qt::AlignVCenter | Qt::AlignRight,
			duration);
		const auto liked = IsLiked(_track.id);
		const auto heart = heartRect();
		DrawHeart(
			p,
			QRectF(heart).adjusted(5, 6, -5, -5),
			liked,
			liked ? HeartColor() : QColor(st::windowSubTextFg->c));
		const auto textWidth = heart.left() - 12 - left;
		p.setFont(st::semiboldFont);
		p.setPen(playing ? st::windowActiveTextFg : st::windowFg);
		p.drawText(
			QRect(left, height() / 2 - 24, textWidth, 22),
			Qt::AlignVCenter | Qt::AlignLeft,
			st::semiboldFont->elided(_track.title, textWidth));
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(left, height() / 2 + 2, textWidth, 20),
			Qt::AlignVCenter | Qt::AlignLeft,
			st::normalFont->elided(_track.artist, textWidth));
	}

	Track _track;
	RowMode _mode;
	QImage _art;
	base::unique_qptr<Ui::PopupMenu> _menu;

};


// ---- полноценный плеер ----

class SeekBar final : public Ui::RpWidget {
public:
	explicit SeekBar(QWidget *parent) : RpWidget(parent) {
		resize(width(), 28);
		setCursor(style::cur_pointer);
		paintRequest(
		) | rpl::on_next([=](QRect) {
			auto p = QPainter(this);
			auto hq = PainterHighQualityEnabler(p);
			const auto line = QRectF(0, height() / 2. - 3, width(), 6);
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(line, 3, 3);
			const auto progress = _dragging ? _drag : _progress;
			auto fill = line;
			fill.setWidth(std::max(6., line.width() * progress));
			p.setBrush(st::windowActiveTextFg);
			p.drawRoundedRect(fill, 3, 3);
			const auto knob = QPointF(fill.right(), line.center().y());
			p.setBrush(Qt::white);
			p.drawEllipse(knob, _dragging ? 8. : 6., _dragging ? 8. : 6.);
		}, lifetime());
	}

	void setProgress(float64 value) {
		_progress = std::clamp(value, 0., 1.);
		if (!_dragging) {
			update();
		}
	}
	[[nodiscard]] rpl::producer<float64> sought() const {
		return _sought.events();
	}

protected:
	void mousePressEvent(QMouseEvent *e) override {
		_dragging = true;
		moveTo(e->pos().x());
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (_dragging) {
			moveTo(e->pos().x());
		}
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		if (_dragging) {
			_dragging = false;
			moveTo(e->pos().x());
			_progress = _drag;
			_sought.fire_copy(_drag);
		}
	}

private:
	void moveTo(int x) {
		_drag = std::clamp(x / float64(std::max(width(), 1)), 0., 1.);
		update();
	}

	float64 _progress = 0.;
	float64 _drag = 0.;
	bool _dragging = false;
	rpl::event_stream<float64> _sought;

};

enum class Glyph { Previous, Next, Play, Heart };

class RoundGlyph final : public Ui::AbstractButton {
public:
	RoundGlyph(QWidget *parent, Glyph glyph, int size)
	: AbstractButton(parent)
	, _glyph(glyph) {
		resize(size, size);
		setPointerCursor(true);
		paintRequest(
		) | rpl::on_next([=](QRect) {
			paint();
		}, lifetime());
	}

	void setPlaying(bool playing) {
		if (_playing != playing) {
			_playing = playing;
			update();
		}
	}
	void setLiked(bool liked) {
		if (_liked != liked) {
			_liked = liked;
			update();
		}
	}

private:
	void paint() {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto r = QRectF(rect());
		const auto c = r.center();
		p.setPen(Qt::NoPen);
		if (_glyph == Glyph::Heart) {
			if (isOver()) {
				p.setBrush(st::windowBgOver);
				p.drawEllipse(r);
			}
			DrawHeart(
				p,
				r.adjusted(r.width() * 0.22, r.height() * 0.24, -r.width() * 0.22, -r.height() * 0.2),
				_liked,
				_liked ? HeartColor() : QColor(st::windowSubTextFg->c));
			return;
		}
		if (_glyph == Glyph::Play) {
			// Без подложки: белая пауза или треугольник.
			p.setBrush(isOver() ? QColor(st::windowFg->c).darker(115) : st::windowFg->c);
			const auto u = r.width() / 64.;
			if (_playing) {
				p.drawRoundedRect(QRectF(c.x() - 14 * u, c.y() - 17 * u, 10 * u, 34 * u), 3 * u, 3 * u);
				p.drawRoundedRect(QRectF(c.x() + 4 * u, c.y() - 17 * u, 10 * u, 34 * u), 3 * u, 3 * u);
			} else {
				auto path = QPainterPath();
				path.moveTo(c.x() - 11 * u, c.y() - 19 * u);
				path.lineTo(c.x() + 19 * u, c.y());
				path.lineTo(c.x() - 11 * u, c.y() + 19 * u);
				path.closeSubpath();
				p.drawPath(path);
			}
			return;
		}
		if (isOver()) {
			p.setBrush(st::windowBgOver);
			p.drawEllipse(r);
		}
		p.setBrush(st::windowFg);
		const auto u = r.width() / 48.;
		const auto dir = (_glyph == Glyph::Next) ? 1. : -1.;
		auto tri = [&](double x) {
			auto path = QPainterPath();
			path.moveTo(c.x() + dir * (x - 8) * u, c.y() - 11 * u);
			path.lineTo(c.x() + dir * (x + 8) * u, c.y());
			path.lineTo(c.x() + dir * (x - 8) * u, c.y() + 11 * u);
			path.closeSubpath();
			p.drawPath(path);
		};
		tri(-2);
		p.drawRoundedRect(QRectF(c.x() + dir * 9 * u - 2 * u, c.y() - 11 * u, 4 * u, 22 * u), 1.5 * u, 1.5 * u);
	}

	Glyph _glyph;
	bool _playing = false;
	bool _liked = false;

};

} // namespace

namespace {

[[nodiscard]] QColor ComputeAccent(const QImage &cover) {
	if (cover.isNull()) {
		return QColor();
	}
	const auto small = cover.scaled(
		24,
		24,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32);
	constexpr auto kBins = 12;
	double weight[kBins] = {};
	double sumR[kBins] = {}, sumG[kBins] = {}, sumB[kBins] = {};
	for (auto y = 0; y != small.height(); ++y) {
		for (auto x = 0; x != small.width(); ++x) {
			const auto c = QColor(small.pixel(x, y));
			const auto s = c.hsvSaturationF();
			const auto v = c.valueF();
			if (v < 0.12) {
				continue;
			}
			const auto bin = std::clamp(int(std::max(c.hsvHueF(), 0.f) * kBins), 0, kBins - 1);
			const auto w = (0.05 + s * s) * (0.3 + v);
			weight[bin] += w;
			sumR[bin] += w * c.redF();
			sumG[bin] += w * c.greenF();
			sumB[bin] += w * c.blueF();
		}
	}
	auto totalS = 0., totalV = 0., total = 0.;
	for (auto y = 0; y != small.height(); ++y) {
		for (auto x = 0; x != small.width(); ++x) {
			const auto c = QColor(small.pixel(x, y));
			totalS += c.hsvSaturationF();
			totalV += c.valueF();
			total += 1.;
		}
	}
	const auto avgS = totalS / total;
	const auto avgV = totalV / total;
	// Почти чёрно-белая обложка: нейтральный серый, без случайного оттенка.
	if (avgS < 0.16) {
		return QColor::fromHsvF(0., 0., std::clamp(avgV, 0.3, 0.5));
	}
	auto best = 0;
	for (auto i = 1; i != kBins; ++i) {
		if (weight[i] > weight[best]) {
			best = i;
		}
	}
	if (weight[best] <= 0.) {
		return QColor();
	}
	auto result = QColor::fromRgbF(
		sumR[best] / weight[best],
		sumG[best] / weight[best],
		sumB[best] / weight[best]);
	auto h = 0.f, s = 0.f, v = 0.f;
	result.getHsvF(&h, &s, &v);
	return QColor::fromHsvF(
		h < 0 ? 0.f : h,
		std::clamp(s, 0.25f, 0.8f),
		std::clamp(v, 0.4f, 0.7f));
}

class LyricsView final : public Ui::RpWidget {
public:
	explicit LyricsView(QWidget *parent)
	: RpWidget(parent) {
		resize(width(), 280);
		_clock.start();
		_timer.setInterval(16);
		connect(&_timer, &QTimer::timeout, this, [=] { tick(); });
		_timer.start();
	}

	void setPlasma(bool on) {
		_plasma = on;
		update();
	}
	void setLoading() {
		reset();
		_status = QString::fromUtf8("Ищу текст…");
		update();
	}
	void setLyrics(Lyrics lyrics) {
		reset();
		_lyrics = std::move(lyrics);
		_status = _lyrics.found
			? QString()
			: QString::fromUtf8("Текст этой песни не найден");
		_weights.assign(_lyrics.lines.size(), 0.f);
		update();
	}
	void setPosition(qint64 ms) {
		_position = ms;
		_positionAt = _clock.elapsed();
	}

private:
	void reset() {
		_lyrics = {};
		_current = -1;
		_scroll = 0.;
		_scrollReady = false;
		_heights.clear();
		_weights.clear();
	}

	// Позиция трека между обновлениями плеера: плавнее подсветка строк.
	[[nodiscard]] qint64 livePosition() const {
		return _position + std::min<qint64>(_clock.elapsed() - _positionAt, 600);
	}

	void tick() {
		if (!isVisible()) {
			_last = _clock.elapsed();
			return;
		}
		const auto now = _clock.elapsed();
		const auto dt = std::clamp((now - _last) / 1000., 0., 0.1);
		_last = now;
		_phase += dt * 0.7;
		if (_lyrics.found && _lyrics.synced) {
			auto index = -1;
			const auto at = livePosition() + 120;
			for (auto i = 0; i != int(_lyrics.lines.size()); ++i) {
				if (_lyrics.lines[i].timeMs <= at) {
					index = i;
				} else {
					break;
				}
			}
			_current = index;
			const auto target = centerOf(std::max(index, 0));
			if (!_scrollReady) {
				_scroll = target;
				_scrollReady = true;
			}
			// Пружина без рывков: чем дальше строка, тем быстрее догоняем.
			_scroll += (target - _scroll) * (1. - std::exp(-dt * 6.5));
			const auto fade = float(1. - std::exp(-dt * 9.));
			for (auto i = 0; i != int(_weights.size()); ++i) {
				const auto goal = (i == _current) ? 1.f : 0.f;
				_weights[i] += (goal - _weights[i]) * fade;
			}
		}
		update();
	}

	[[nodiscard]] static constexpr double kScaleMax() {
		return 1.12;
	}
	[[nodiscard]] int textWidth() const {
		return int((width() - 56) / kScaleMax());
	}
	[[nodiscard]] QFont lineFont() const {
		auto font = st::semiboldFont->f;
		font.setPixelSize(19);
		return font;
	}
	[[nodiscard]] int heightOf(int index) {
		if (_heights.size() != _lyrics.lines.size()) {
			_heights.assign(_lyrics.lines.size(), -1);
		}
		if (_heights[index] < 0) {
			const auto text = _lyrics.lines[index].text.isEmpty()
				? u" "_q
				: _lyrics.lines[index].text;
			_heights[index] = QFontMetrics(lineFont()).boundingRect(
				QRect(0, 0, textWidth(), 10000),
				Qt::TextWordWrap | Qt::AlignHCenter,
				text).height() + 20;
		}
		return _heights[index];
	}
	// Центр строки index в координатах содержимого.
	[[nodiscard]] double centerOf(int index) {
		auto y = 0.;
		for (auto i = 0; i != index; ++i) {
			y += heightOf(i);
		}
		return y + heightOf(index) / 2.;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		auto path = QPainterPath();
		path.addRoundedRect(QRectF(rect()), 20, 20);
		p.setClipPath(path);
		p.fillRect(rect(), st::windowBgOver);
		const auto accent = CurrentCoverAccent();
		if (_plasma && accent.isValid()) {
			const auto w = width();
			const auto h = height();
			for (auto i = 0; i != 3; ++i) {
				auto color = QColor::fromHsvF(
					std::fmod(accent.hsvHueF() + 0.09 * (i - 1) + 1., 1.),
					accent.hsvSaturationF() < 0.05
						? 0.
						: std::min(1., accent.hsvSaturationF() + 0.1),
					std::min(1., accent.valueF() + 0.1 * i));
				const auto cx = w * (0.5 + 0.38 * std::sin(_phase * (1. + 0.4 * i) + i * 2.1));
				const auto cy = h * (0.5 + 0.34 * std::cos(_phase * (0.8 + 0.3 * i) + i * 1.3));
				auto gradient = QRadialGradient(cx, cy, w * 0.55);
				color.setAlpha(120);
				gradient.setColorAt(0., color);
				color.setAlpha(0);
				gradient.setColorAt(1., color);
				p.fillRect(rect(), gradient);
			}
			p.fillRect(rect(), QColor(0, 0, 0, 90));
		}
		if (!_lyrics.found) {
			p.setPen(st::windowSubTextFg);
			p.setFont(st::normalFont);
			p.drawText(rect(), Qt::AlignCenter, _status);
			return;
		}
		const auto count = int(_lyrics.lines.size());
		auto top = _lyrics.synced ? (height() / 2. - _scroll) : 12.;
		p.setFont(lineFont());
		for (auto i = 0; i != count; ++i) {
			const auto lineHeight = heightOf(i);
			if (top + lineHeight >= -20 && top <= height() + 20) {
				const auto w = (_lyrics.synced && i < int(_weights.size()))
					? double(_weights[i])
					: 0.;
				// Дальние строки тусклее, текущая белая и крупнее.
				const auto distance = _lyrics.synced
					? std::abs(i - std::max(_current, 0))
					: 1;
				const auto rest = std::max(0.2, 0.55 - 0.07 * distance);
				auto color = QColor(255, 255, 255);
				color.setAlphaF(rest + (1. - rest) * w);
				p.setPen(color);
				const auto scale = 1. + (kScaleMax() - 1.) * w;
				const auto cy = top + lineHeight / 2.;
				p.save();
				p.translate(width() / 2., cy);
				p.scale(scale, scale);
				p.drawText(
					QRectF(-textWidth() / 2., -lineHeight / 2. + 10, textWidth(), lineHeight - 10),
					Qt::TextWordWrap | Qt::AlignHCenter | Qt::AlignTop,
					_lyrics.lines[i].text);
				p.restore();
			}
			top += lineHeight;
			if (top > height() + 400) {
				break;
			}
		}
		// Мягкое затухание сверху и снизу.
		auto fadeTop = QLinearGradient(0, 0, 0, 56);
		fadeTop.setColorAt(0., QColor(0, 0, 0, 140));
		fadeTop.setColorAt(1., QColor(0, 0, 0, 0));
		p.fillRect(QRect(0, 0, width(), 56), fadeTop);
		auto fadeBottom = QLinearGradient(0, height() - 56, 0, height());
		fadeBottom.setColorAt(0., QColor(0, 0, 0, 0));
		fadeBottom.setColorAt(1., QColor(0, 0, 0, 140));
		p.fillRect(QRect(0, height() - 56, width(), 56), fadeBottom);
	}

	Lyrics _lyrics;
	QString _status;
	QTimer _timer;
	QElapsedTimer _clock;
	std::vector<int> _heights;
	std::vector<float> _weights;
	qint64 _position = 0;
	qint64 _positionAt = 0;
	qint64 _last = 0;
	double _scroll = 0.;
	double _phase = 0.;
	int _current = -1;
	bool _scrollReady = false;
	bool _plasma = true;

};

} // namespace

QColor CurrentCoverAccent() {
	if (!Mokagram::Settings::Bool(u"player.cover_colors"_q)) {
		return QColor();
	}
	const auto current = Media::Player::instance()->current(AudioMsgId::Type::Song);
	if (!OwnsTrack(current)) {
		return QColor();
	}
	static auto cachedId = uint64();
	static auto cached = QColor();
	const auto id = current.audio()->id;
	const auto cover = CoverForDocument(id);
	if (cover.isNull()) {
		return QColor();
	}
	if (cachedId != id || !cached.isValid()) {
		cachedId = id;
		cached = ComputeAccent(cover);
	}
	return cached;
}

bool OwnsTrack(const AudioMsgId &id) {
	return id.audio()
		&& (id.audio()->id & 0xFFFFFF0000000000ULL) == 0x4D4F4B0000000000ULL;
}

bool NextAvailable() {
	const auto &queue = QueueState();
	const auto &settings = Core::App().settings();
	return !queue.tracks.empty()
		&& (queue.index + 1 < int(queue.tracks.size())
			|| settings.playerRepeatMode() == Media::RepeatMode::All
			|| settings.playerOrderMode() == Media::OrderMode::Shuffle);
}

bool PreviousAvailable() {
	const auto &queue = QueueState();
	return !queue.tracks.empty()
		&& (queue.index > 0
			|| Core::App().settings().playerRepeatMode()
				== Media::RepeatMode::All);
}

void Step(int delta) {
	auto &queue = QueueState();
	const auto count = int(queue.tracks.size());
	if (!count) {
		return;
	}
	auto target = queue.index + delta;
	if (delta > 0
		&& Core::App().settings().playerOrderMode()
			== Media::OrderMode::Shuffle
		&& count > 1) {
		target = queue.index;
		while (target == queue.index) {
			target = base::RandomIndex(count);
		}
	} else if (target >= count) {
		target = 0;
	} else if (target < 0) {
		target = count - 1;
	}
	PlayIndex(target);
}

rpl::producer<> QueueChanges() {
	return QueueState().changes.events();
}

bool CurrentTrackInfo(QString &title, QString &artist) {
	const auto &queue = QueueState();
	if (queue.index < 0 || queue.index >= int(queue.tracks.size())) {
		return false;
	}
	title = queue.tracks[queue.index].title;
	artist = queue.tracks[queue.index].artist;
	return true;
}

void ShowCurrentTrackMenu(
		not_null<Window::SessionController*> controller,
		QPoint globalPosition) {
	const auto &queue = QueueState();
	if (queue.index < 0 || queue.index >= int(queue.tracks.size())) {
		return;
	}
	QueueState().controller = base::make_weak(controller);
	static auto menu = base::unique_qptr<Ui::PopupMenu>();
	QWidget *parent = controller->widget().get();
	menu = base::make_unique_q<Ui::PopupMenu>(
		parent,
		st::popupMenuWithIcons);
	const auto track = queue.tracks[queue.index];
	const auto add = [&](const QString &text, Fn<void()> callback, const style::icon *icon) {
		menu->addAction(text, std::move(callback), icon);
	};
	const auto instance = Media::Player::instance();
	add(QString::fromUtf8("Пауза / играть"), [=] {
		instance->playPause(AudioMsgId::Type::Song);
	}, &st::menuIconSoundOn);
	if (PreviousAvailable()) {
		add(QString::fromUtf8("Предыдущий"), [] { Step(-1); }, &st::menuIconAbove);
	}
	if (NextAvailable()) {
		add(QString::fromUtf8("Следующий"), [] { Step(1); }, &st::menuIconBelow);
	}
	menu->addSeparator();
	add(IsLiked(track.id)
		? QString::fromUtf8("Убрать из избранного")
		: QString::fromUtf8("В избранное"), [=] {
		SetLiked(track, !IsLiked(track.id));
	}, &st::menuIconFave);
	add(QString::fromUtf8("Скачать"), [=] { SaveToDisk(track); }, &st::menuIconDownload);
	add(QString::fromUtf8("Переслать"), [=] { ForwardTrack(controller, track); }, &st::menuIconForward);
	add(QString::fromUtf8("Добавить в профиль"), [=] { AddToProfile(controller, track); }, &st::menuIconProfile);
	add(QString::fromUtf8("Копировать ссылку"), [=] {
		QGuiApplication::clipboard()->setText(track.permalink);
		controller->showToast(QString::fromUtf8("Ссылка скопирована"));
	}, &st::menuIconLink);
	menu->addSeparator();
	add(QString::fromUtf8("Поиск музыки"), [=] { ShowMusicBox(controller); }, &st::menuIconSearch);
	add(QString::fromUtf8("Очередь"), [=] { ShowPlayerBox(controller); }, &st::menuIconChats);
	menu->popup(globalPosition);
}

void ShowPlayerBox(not_null<Window::SessionController*> controller) {
	QueueState().controller = base::make_weak(controller);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		Mokagram::Plugins::RoundedWindow(box);
		const auto content = box->verticalLayout();
		const auto instance = Media::Player::instance();

		// Цвет обложки: мягкий градиент сверху, под всем содержимым.
		const auto tint = Ui::CreateChild<Ui::RpWidget>(box.get());
		tint->setAttribute(Qt::WA_TransparentForMouseEvents);
		tint->lower();
		box->sizeValue(
		) | rpl::on_next([=](QSize size) {
			tint->setGeometry(0, 0, size.width(), size.height());
		}, tint->lifetime());
		tint->paintOn([=](QPainter &p) {
			const auto accent = CurrentCoverAccent();
			if (!accent.isValid()) {
				return;
			}
			auto hq = PainterHighQualityEnabler(p);
			auto path = QPainterPath();
			path.addRoundedRect(QRectF(tint->rect()), 26, 26);
			p.setClipPath(path);
			auto top = accent;
			top.setAlpha(150);
			auto bottom = accent;
			bottom.setAlpha(0);
			auto gradient = QLinearGradient(0, 0, 0, tint->height() * 0.7);
			gradient.setColorAt(0., top);
			gradient.setColorAt(1., bottom);
			p.fillRect(tint->rect(), gradient);
		});

		const auto header = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(10, 10, 10, 0));
		header->resize(header->width(), 40);
		const auto close = Mokagram::Plugins::MakeHeaderButton(
			header,
			true,
			[=] { box->closeBox(); });
		const auto title = Ui::CreateChild<Ui::FlatLabel>(
			header,
			QString::fromUtf8("Сейчас играет"),
			st::boxTitle);
		const auto searchButton = Mokagram::Plugins::MakeHeaderButton(
			header,
			false,
			[=] {
				box->closeBox();
				ShowMusicBox(controller);
			});
		header->widthValue(
		) | rpl::on_next([=](int width) {
			close->move(0, (header->height() - close->height()) / 2);
			title->move(
				(width - title->width()) / 2,
				(header->height() - title->height()) / 2);
			searchButton->move(
				width - searchButton->width(),
				(header->height() - searchButton->height()) / 2);
		}, header->lifetime());

		// Обложка, название, исполнитель.
		const auto info = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(0, 8, 0, 0));
		info->resize(info->width(), 400);
		const auto trackFor = [=]() -> std::optional<Track> {
			const auto &queue = QueueState();
			if (queue.index >= 0 && queue.index < int(queue.tracks.size())) {
				return queue.tracks[queue.index];
			}
			return std::nullopt;
		};
		info->paintOn([=](QPainter &p) {
			auto hq = PainterHighQualityEnabler(p);
			const auto side = std::min(info->width() - 64, 340);
			const auto art = QRect((info->width() - side) / 2, 8, side, side);
			const auto current = instance->current(AudioMsgId::Type::Song);
			auto cover = current.audio()
				? CoverForDocument(current.audio()->id)
				: QImage();
			auto path = QPainterPath();
			path.addRoundedRect(art, 20, 20);
			if (!cover.isNull()) {
				p.save();
				p.setClipPath(path);
				p.drawImage(art, cover);
				p.restore();
			} else {
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowBgOver);
				p.drawPath(path);
				st::menuIconSoundOn.paintInCenter(p, art);
			}
			auto name = QString();
			auto artist = QString();
			if (const auto track = trackFor()) {
				name = track->title;
				artist = track->artist;
			} else if (const auto song = current.audio() ? current.audio()->song() : nullptr) {
				name = song->title;
				artist = song->performer;
			}
			auto big = st::semiboldFont->f;
			big.setPixelSize(22);
			p.setFont(big);
			p.setPen(st::windowFg);
			const auto textWidth = info->width() - 64 - 56;
			const auto metrics = QFontMetrics(big);
			p.drawText(
				QRect(32, art.bottom() + 24, textWidth, 30),
				Qt::AlignLeft | Qt::AlignVCenter,
				metrics.elidedText(name, Qt::ElideRight, textWidth));
			auto mid = st::normalFont->f;
			mid.setPixelSize(16);
			p.setFont(mid);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				QRect(32, art.bottom() + 56, textWidth, 24),
				Qt::AlignLeft | Qt::AlignVCenter,
				QFontMetrics(mid).elidedText(artist, Qt::ElideRight, textWidth));
		});

		const auto heart = Ui::CreateChild<RoundGlyph>(info, Glyph::Heart, 48);
		info->widthValue(
		) | rpl::on_next([=](int width) {
			const auto side = std::min(width - 64, 340);
			heart->move(width - 32 - 48 + 8, 8 + side + 24 + 4);
		}, heart->lifetime());

		// Перемотка и время.
		const auto seek = content->add(
			object_ptr<SeekBar>(content),
			QMargins(32, 4, 32, 0));
		const auto times = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(32, 0, 32, 0));
		times->resize(times->width(), 20);
		const auto position = std::make_shared<std::pair<QString, QString>>();
		times->paintOn([=](QPainter &p) {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(times->rect(), Qt::AlignLeft | Qt::AlignVCenter, position->first);
			p.drawText(times->rect(), Qt::AlignRight | Qt::AlignVCenter, position->second);
		});

		// Кнопки.
		const auto controls = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(0, 10, 0, 6));
		controls->resize(controls->width(), 84);
		const auto previous = Ui::CreateChild<RoundGlyph>(controls, Glyph::Previous, 56);
		const auto play = Ui::CreateChild<RoundGlyph>(controls, Glyph::Play, 72);
		const auto next = Ui::CreateChild<RoundGlyph>(controls, Glyph::Next, 56);
		controls->widthValue(
		) | rpl::on_next([=](int width) {
			const auto top = (controls->height() - 72) / 2;
			play->move((width - 72) / 2, top);
			previous->move(play->x() - 28 - 56, top + 8);
			next->move(play->x() + 72 + 28, top + 8);
		}, controls->lifetime());

		const auto step = [=](int delta) {
			if (!QueueState().tracks.empty()) {
				Step(delta);
			} else if (delta > 0) {
				instance->next(AudioMsgId::Type::Song);
			} else {
				instance->previous(AudioMsgId::Type::Song);
			}
		};
		heart->setClickedCallback([=] {
			const auto &queue = QueueState();
			if (!queue.current.id.isEmpty()) {
				SetLiked(queue.current, !IsLiked(queue.current.id));
			}
		});
		previous->setClickedCallback([=] { step(-1); });
		next->setClickedCallback([=] { step(1); });
		play->setClickedCallback([=] {
			instance->playPause(AudioMsgId::Type::Song);
		});
		seek->sought(
		) | rpl::on_next([=](float64 progress) {
			instance->startSeeking(AudioMsgId::Type::Song);
			instance->finishSeeking(AudioMsgId::Type::Song, progress);
		}, seek->lifetime());

		const auto refresh = [=] {
			const auto state = instance->getState(AudioMsgId::Type::Song);
			const auto frequency = std::max<int64>(state.frequency, 1);
			const auto length = state.length;
			seek->setProgress(length > 0 ? state.position / float64(length) : 0.);
			const auto fmt = [](int64 seconds) {
				return u"%1:%2"_q.arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
			};
			position->first = fmt(state.position / frequency);
			position->second = fmt(length / frequency);
			times->update();
			info->update();
			tint->update();
			heart->setLiked(!QueueState().current.id.isEmpty()
				&& IsLiked(QueueState().current.id));
			play->setPlaying(state.state == Media::Player::State::Playing
				|| state.state == Media::Player::State::Starting
				|| state.state == Media::Player::State::Resuming);
		};
		instance->updatedNotifier(
		) | rpl::on_next(refresh, box->lifetime());
		QueueState().changes.events(
		) | rpl::on_next(refresh, box->lifetime());
		CoversChanged() | rpl::on_next(refresh, box->lifetime());
		LikesChanged() | rpl::on_next(refresh, box->lifetime());
		refresh();

		// Текст песни (LRCLIB), под кнопками.
		const auto lyricsView = content->add(
			object_ptr<LyricsView>(content),
			QMargins(24, 8, 24, 4));
		lyricsView->setVisible(Mokagram::Settings::Bool(u"lyrics.enabled"_q));
		lyricsView->setPlasma(Mokagram::Settings::Bool(u"lyrics.plasma"_q));
		const auto lyricsFor = std::make_shared<QString>();
		const auto updateLyrics = [=] {
			const auto &track = QueueState().current;
			if (!Mokagram::Settings::Bool(u"lyrics.enabled"_q)
				|| track.id.isEmpty()
				|| *lyricsFor == track.id) {
				return;
			}
			*lyricsFor = track.id;
			lyricsView->setLoading();
			const auto id = track.id;
			FetchLyrics(
				id,
				track.title,
				track.artist,
				track.durationMs,
				crl::guard(lyricsView, [=](Lyrics lyrics) {
					if (*lyricsFor == id) {
						lyricsView->setLyrics(std::move(lyrics));
					}
				}));
		};
		instance->updatedNotifier(
		) | rpl::on_next([=] {
			const auto state = instance->getState(AudioMsgId::Type::Song);
			lyricsView->setPosition(
				state.position * 1000 / std::max<int64>(state.frequency, 1));
			updateLyrics();
		}, lyricsView->lifetime());
		QueueState().changes.events(
		) | rpl::on_next(updateLyrics, lyricsView->lifetime());
		updateLyrics();

		// Очередь: живой список, меняется при добавлении, перемещении и смене трека.
		Ui::AddSubsectionTitle(content, rpl::single(QString::fromUtf8("Очередь")));
		const auto queueList = content->add(object_ptr<Ui::VerticalLayout>(content));
		const auto empty = content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				QString::fromUtf8("Очередь пуста. Найдите музыку и выберите «Играть следующим» или «В конец очереди» в меню трека."),
				st::boxLabel),
			QMargins(24, 4, 24, 8));
		const auto rebuild = [=] {
			queueList->clear();
			const auto &queue = QueueState();
			empty->setVisible(queue.tracks.empty());
			for (auto i = 0; i != int(queue.tracks.size()); ++i) {
				queueList->add(object_ptr<Row>(
					queueList,
					controller,
					queue.tracks[i],
					i,
					RowMode::Queue));
			}
		};
		rebuild();
		QueueState().changes.events(
		) | rpl::on_next(rebuild, box->lifetime());
		Ui::AddSkip(content, 10);
	}));
}

void DebugPlay(int index) {
	PlayFromResults(index);
}

void DebugLike(int index) {
	const auto &queue = QueueState();
	if (index >= 0 && index < int(queue.results.size())) {
		SetLiked(queue.results[index], !IsLiked(queue.results[index].id));
	}
}

void DebugTab(int tab) {
	QueueState().tab = tab;
	if (const auto controller = QueueState().controller.get()) {
		ShowMusicBox(controller);
	}
}

void DebugSaveTrack(int index) {
	const auto &queue = QueueState();
	if (index >= 0 && index < int(queue.tracks.size())) {
		SaveToDisk(queue.tracks[index]);
	}
}

void ShowMusicBox(
		not_null<Window::SessionController*> controller,
		const QString &initialQuery) {
	QueueState().controller = base::make_weak(controller);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		Mokagram::Plugins::RoundedWindow(box);
		const auto content = box->verticalLayout();

		// Шапка: закрыть слева, название по центру.
		const auto header = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(10, 10, 10, 0));
		header->resize(header->width(), 40);
		const auto close = Mokagram::Plugins::MakeHeaderButton(
			header,
			true,
			[=] { box->closeBox(); });
		const auto title = Ui::CreateChild<Ui::FlatLabel>(
			header,
			QString::fromUtf8("Музыка"),
			st::boxTitle);
		const auto queueButton = Ui::CreateChild<Ui::RoundButton>(
			header,
			rpl::single(QString::fromUtf8("Очередь")),
			st::defaultLightButton);
		queueButton->setClickedCallback([=] {
			box->closeBox();
			ShowPlayerBox(controller);
		});
		header->widthValue(
		) | rpl::on_next([=](int width) {
			close->move(0, (header->height() - close->height()) / 2);
			title->move(
				(width - title->width()) / 2,
				(header->height() - title->height()) / 2);
			queueButton->move(
				width - queueButton->width(),
				(header->height() - queueButton->height()) / 2);
		}, header->lifetime());

		// Поиск: круглая плашка с полем.
		static const auto clear = style::owned_color(QColor(0, 0, 0, 0));
		static const auto fieldStyle = [] {
			auto result = st::defaultInputField;
			result.textBg = clear.color();
			result.textBgActive = clear.color();
			const auto pad = std::max(0, (44 - st::defaultInputField.style.font->height) / 2);
			result.textMargins = style::margins(0, pad, 0, pad);
			result.placeholderFgActive = st::windowSubTextFg;
			result.placeholderScale = 1.;
			result.placeholderShift = 0;
			result.placeholderFont = st::normalFont;
			result.border = 0;
			result.borderActive = 0;
			result.heightMin = 44;
			return result;
		}();
		const auto searchWrap = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(16, 14, 16, 6));
		searchWrap->resize(searchWrap->width(), 44);
		searchWrap->paintOn([=](QPainter &p) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBg);
			p.drawRoundedRect(searchWrap->rect(), 22, 22);
		});
		const auto input = Ui::CreateChild<Ui::InputField>(
			searchWrap,
			fieldStyle,
			rpl::single(QString::fromUtf8("Поиск на SoundCloud")));
		searchWrap->widthValue(
		) | rpl::on_next([=](int width) {
			input->setGeometry(18, 0, width - 36, 44);
		}, searchWrap->lifetime());

		// Вкладки: поиск, лайкнутые, подборка.
		const auto tabs = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(16, 4, 16, 6));
		tabs->resize(tabs->width(), 36);
		const auto tabSearch = Ui::CreateChild<TabPill>(tabs, QString::fromUtf8("Поиск"));
		const auto tabLiked = Ui::CreateChild<TabPill>(tabs, QString::fromUtf8("Лайкнутые"));
		const auto tabForYou = Ui::CreateChild<TabPill>(tabs, QString::fromUtf8("Для вас"));
		tabs->widthValue(
		) | rpl::on_next([=](int width) {
			const auto gap = 8;
			const auto each = (width - 2 * gap) / 3;
			tabSearch->setGeometry(0, 0, each, 36);
			tabLiked->setGeometry(each + gap, 0, each, 36);
			tabForYou->setGeometry(2 * (each + gap), 0, width - 2 * (each + gap), 36);
		}, tabs->lifetime());

		const auto list = content->add(object_ptr<Ui::VerticalLayout>(content));
		Ui::AddSkip(content, 10);
		const auto statusText = std::make_shared<QString>();
		const auto refreshForYou = std::make_shared<Fn<void()>>();

		// Список: сверху строка действий (для подборки) и пояснение, дальше треки.
		const auto fill = [=] {
			list->clear();
			const auto &queue = QueueState();
			if (queue.tab == 2 && !queue.forYou.empty()) {
				list->add(
					object_ptr<PicksBar>(
						list,
						[=] { PlayAll(QueueState().forYou); },
						[=] {
							QueueState().forYou.clear();
							QueueState().results.clear();
							*statusText = QString::fromUtf8("Подбираю музыку под ваш вкус…");
							(*refreshForYou)();
						}),
					QMargins(16, 4, 16, 10));
			}
			if (!statusText->isEmpty()) {
				list->add(
					object_ptr<Ui::FlatLabel>(list, *statusText, st::boxLabel),
					QMargins(24, 8, 24, 8));
			}
			for (auto i = 0; i != int(queue.results.size()); ++i) {
				list->add(object_ptr<Row>(list, controller, queue.results[i], i));
			}
		};
		const auto showStatus = [=](const QString &text) {
			*statusText = text;
			fill();
		};
		const auto buildForYou = [=] {
			*statusText = QString::fromUtf8("Подбираю музыку под ваш вкус…");
			fill();
			BuildForYou(30, crl::guard(box, [=](std::vector<Track> tracks, QString note) {
				auto &queue = QueueState();
				queue.forYou = std::move(tracks);
				queue.forYouNote = note;
				if (queue.tab == 2) {
					queue.results = queue.forYou;
					showStatus(note);
					fill();
				}
			}));
		};
		*refreshForYou = buildForYou;
		const auto setTab = [=](int tab) {
			auto &queue = QueueState();
			queue.tab = tab;
			tabSearch->setActive(tab == 0);
			tabLiked->setActive(tab == 1);
			tabForYou->setActive(tab == 2);
			if (tab == 0) {
				queue.results = queue.searchResults;
				showStatus(queue.searchResults.empty()
					? QString::fromUtf8("Введите название или исполнителя и нажмите Enter")
					: QString());
			} else if (tab == 1) {
				queue.results = LikedTracks();
				showStatus(queue.results.empty()
					? QString::fromUtf8("Пока пусто. Нажмите сердечко у трека, и он появится здесь.")
					: QString());
			} else {
				queue.results = queue.forYou;
				if (queue.forYou.empty()) {
					buildForYou();
				} else {
					showStatus(QString());
				}
			}
			fill();
		};
		tabSearch->setClickedCallback([=] { setTab(0); });
		tabLiked->setClickedCallback([=] { setTab(1); });
		tabForYou->setClickedCallback([=] { setTab(2); });
		LikesChanged(
		) | rpl::filter([=] {
			return QueueState().tab == 1;
		}) | rpl::on_next([=] {
			auto &queue = QueueState();
			queue.results = LikedTracks();
			showStatus(queue.results.empty()
				? QString::fromUtf8("Пока пусто. Нажмите сердечко у трека, и он появится здесь.")
				: QString());
			fill();
		}, box->lifetime());

		const auto search = [=] {
			const auto query = input->getLastText().trimmed();
			if (query.isEmpty()) {
				return;
			}
			if (QueueState().tab != 0) {
				setTab(0);
			}
			showStatus(QString::fromUtf8("Ищу…"));
			Search(query, 0, crl::guard(box, [=](SearchResult result) {
				list->clear();
				if (!result.error.isEmpty()) {
					showStatus(result.error);
					return;
				}
				showStatus(result.tracks.empty()
					? QString::fromUtf8("Ничего не найдено")
					: QString());
				auto &queue = QueueState();
				queue.searchResults = result.tracks;
				queue.results = result.tracks;
				queue.lastQuery = query;
				fill();
			}));
		};
		input->changes() | rpl::on_next([=] {
			// Подсказка не должна просвечивать под текстом.
			input->setPlaceholder(rpl::single(input->getLastText().isEmpty()
				? QString::fromUtf8("Поиск на SoundCloud")
				: QString()));
		}, input->lifetime());
		input->submits() | rpl::on_next(search, input->lifetime());
		input->setFocusFast();
		if (!initialQuery.isEmpty()) {
			input->setText(initialQuery);
			search();
		} else {
			if (!QueueState().lastQuery.isEmpty()) {
				// Возвращаем последний запрос и его результаты без нового запроса.
				input->setText(QueueState().lastQuery);
			}
			setTab(QueueState().tab);
		}
	}));
}

} // namespace Mokagram::Music
