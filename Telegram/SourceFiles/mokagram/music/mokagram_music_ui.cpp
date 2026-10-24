#include "mokagram/music/mokagram_music_ui.h"

#include "mokagram/music/mokagram_soundcloud.h"

#include "base/unixtime.h"
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
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtCore/QPointer>

namespace Mokagram::Music {
namespace {

constexpr auto kRowHeight = 76;
constexpr auto kArtSize = 56;

struct Queue {
	std::vector<Track> tracks;
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
		if (Media::Player::IsStoppedAtEnd(state.state)
			&& queue.index + 1 < int(queue.tracks.size())) {
			PlayIndex(queue.index + 1);
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
	const auto track = queue.tracks[index];
	const auto token = ++queue.pendingToken;
	queue.index = index;
	queue.currentId = track.id;
	queue.changes.fire({});
	controller->showToast(QString::fromUtf8("Загружаю: ") + track.title);
	Download(track, [=](const QString &path, const QString &error) {
		auto &queue = QueueState();
		const auto controller = queue.controller.get();
		if (!controller || token != queue.pendingToken) {
			return;
		}
		if (!error.isEmpty()) {
			controller->showToast(QString::fromUtf8("Не удалось загрузить: ") + error);
			return;
		}
		const auto document = MakeDocument(&controller->session(), track, path);
		Media::Player::instance()->play(
			AudioMsgId(document, FullMsgId()),
			std::nullopt);
		// Следующий трек качаем заранее, чтобы переход был без паузы.
		if (index + 1 < int(queue.tracks.size())) {
			Download(queue.tracks[index + 1], [](const QString &, const QString &) {});
		}
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
		Fn<void(const QString &)> done) {
	const auto controller = QueueState().controller.get();
	Download(track, [=](const QString &path, const QString &error) {
		if (!error.isEmpty()) {
			if (const auto controller = QueueState().controller.get()) {
				controller->showToast(QString::fromUtf8("Не удалось загрузить: ") + error);
			}
			return;
		}
		done(path);
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
	EnsureFile(track, [=](const QString &path) {
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
			EnsureFile(track, [=](const QString &path) {
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
	EnsureFile(track, [=](const QString &path) {
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

class Row final : public Ui::AbstractButton {
public:
	Row(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		const Track &track,
		int index)
	: AbstractButton(parent)
	, _track(track) {
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
		setClickedCallback([=] { PlayIndex(index); });
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
	void showMenu(not_null<Window::SessionController*> controller, int index) {
		_menu = base::make_unique_q<Ui::PopupMenu>(
			this,
			st::popupMenuWithIcons);
		const auto track = _track;
		const auto add = [&](const QString &text, Fn<void()> callback, const style::icon *icon) {
			_menu->addAction(text, std::move(callback), icon);
		};
		add(QString::fromUtf8("Воспроизвести"), [=] { PlayIndex(index); }, &st::menuIconSoundOn);
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
		const auto textWidth = right - durationWidth - 16 - left;
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
	QImage _art;
	base::unique_qptr<Ui::PopupMenu> _menu;

};

} // namespace

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
		header->widthValue(
		) | rpl::on_next([=](int width) {
			close->move(0, (header->height() - close->height()) / 2);
			title->move(
				(width - title->width()) / 2,
				(header->height() - title->height()) / 2);
		}, header->lifetime());

		// Поиск: круглая плашка с полем.
		static const auto clear = style::owned_color(QColor(0, 0, 0, 0));
		static const auto fieldStyle = [] {
			auto result = st::defaultInputField;
			result.textBg = clear.color();
			result.textBgActive = clear.color();
			result.textMargins = style::margins(0, 10, 0, 10);
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

		const auto status = content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				QString::fromUtf8("Введите название или исполнителя и нажмите Enter"),
				st::boxLabel),
			QMargins(24, 8, 24, 8));
		const auto list = content->add(object_ptr<Ui::VerticalLayout>(content));
		Ui::AddSkip(content, 10);

		const auto search = [=] {
			const auto query = input->getLastText().trimmed();
			if (query.isEmpty()) {
				return;
			}
			status->setText(QString::fromUtf8("Ищу…"));
			Search(query, 0, crl::guard(box, [=](SearchResult result) {
				list->clear();
				if (!result.error.isEmpty()) {
					status->setText(result.error);
					return;
				}
				status->setText(result.tracks.empty()
					? QString::fromUtf8("Ничего не найдено")
					: QString());
				status->setVisible(result.tracks.empty());
				auto &queue = QueueState();
				queue.tracks = result.tracks;
				for (auto i = 0; i != int(result.tracks.size()); ++i) {
					list->add(object_ptr<Row>(list, controller, result.tracks[i], i));
				}
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
		}
	}));
}

} // namespace Mokagram::Music
