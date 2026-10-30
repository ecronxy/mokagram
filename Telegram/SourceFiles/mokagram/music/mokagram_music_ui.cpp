// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 7b8dec14
#include "mokagram/music/mokagram_music_ui.h"
#include "mokagram/lang/mokagram_lang.h"

#include "mokagram/music/mokagram_soundcloud.h"
#include "mokagram/music/mokagram_music_library.h"
#include "mokagram/music/mokagram_lyrics.h"
#include "mokagram/settings/mokagram_settings.h"

#include "base/timer.h"
#include "base/unixtime.h"
#include "base/options.h"
#include "mainwindow.h"
#include "base/random.h"
#include <set>
#include "core/application.h"
#include "core/core_settings.h"
#include "api/api_common.h"
#include "apiwrap.h"
#include "data/data_media_types.h"
#include "data/data_saved_music.h"
#include "data/data_search_controller.h"
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
#include "storage/serialize_document.h"
#include "chat_helpers/stickers_lottie.h"
#include "chat_helpers/tabbed_panel.h"
#include "chat_helpers/tabbed_selector.h"
#include "core/version.h"
#include "history/view/media/history_view_media_common.h"
#include "history/view/media/history_view_sticker_player.h"
#include "lottie/lottie_single_player.h"
#include "styles/style_chat_helpers.h"

#include "core/file_location.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "ui/effects/animations.h"
#include <QtWidgets/QGraphicsEffect>
#include <array>
#include "ui/image/image.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/power_saving.h"
#include "ui/text/format_song_document_name.h"
#include "ui/vertical_list.h"
#include "ui/resize_area.h"
#include "styles/style_chat.h"
#include "styles/style_window.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/widgets/scroll_area.h"
#include "media/player/media_player_volume_controller.h"
#include "window/window_session_controller.h"
#include "mainwidget.h"
#include "window/window_peer_menu.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_media_player.h"
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
#include <array>
#include <cmath>

namespace Mokagram::Music {
namespace {

constexpr auto kRowHeight = 76;
constexpr auto kSwitchDuration = crl::time(220);
constexpr auto kCoverFadeDuration = crl::time(320);
constexpr auto kTypingDelay = crl::time(400);
constexpr auto kArtSize = 56;

struct Queue {
	base::flat_map<QString, FullMsgId> messages;
	base::weak_ptr<Main::Session> session;
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
	bool telegramSearch = false;
	PeerId selectedChat;
	// Умный поток: очередь сама пополняется, пока играет.
	bool stream = false;
	bool refilling = false;
	bool waiting = false;            // очередь кончилась, ждём пополнения
	int streamEpoch = 0;
	std::set<QString> autoIds;       // добавлено подборщиком, не вручную
};

Queue &QueueState() {
	static auto queue = Queue();
	return queue;
}

[[nodiscard]] bool IsTelegramTrack(const Track &track) {
	return track.id.startsWith(u"telegram:"_q);
}

[[nodiscard]] HistoryItem *TrackMessage(const Track &track) {
	const auto &queue = QueueState();
	const auto session = queue.session.get();
	const auto i = queue.messages.find(track.id);
	return (session && i != queue.messages.end())
		? session->data().message(i->second)
		: nullptr;
}

[[nodiscard]] bool CanForwardTrack(const Track &track) {
	if (!IsTelegramTrack(track)) {
		return true;
	}
	const auto item = TrackMessage(track);
	return item && item->allowsForward();
}

void BindController(not_null<Window::SessionController*> controller) {
	auto &queue = QueueState();
	if (queue.session.get() != &controller->session()) {
		++queue.pendingToken;
		queue.messages.clear();
		queue.selectedChat = {};
		queue.tracks.clear();
		queue.results.clear();
		queue.searchResults.clear();
		queue.current = {};
		queue.currentId.clear();
		queue.lastQuery.clear();
		queue.index = -1;
		queue.custom = false;
		queue.recorded = true;
		queue.stream = false;
		queue.refilling = false;
		queue.waiting = false;
		++queue.streamEpoch;
		queue.autoIds.clear();
		queue.session = base::make_weak(&controller->session());
	}
	queue.controller = base::make_weak(controller);
}

[[nodiscard]] Track TrackFromMessage(not_null<HistoryItem*> item) {
	const auto document = item->media() ? item->media()->document() : nullptr;
	if (!document || !document->isSong()) {
		return {};
	}
	const auto name = Ui::Text::FormatSongNameFor(document).composedName();
	auto track = Track();
	const auto id = item->fullId();
	track.id = u"telegram:%1:%2"_q.arg(id.peer.value).arg(id.msg.bare);
	track.title = name.title;
	track.artist = name.performer;
	track.durationMs = document->duration();
	QueueState().messages.emplace(track.id, id);
	return track;
}

class ChatMusicSearch final {
public:
	ChatMusicSearch(
		not_null<Main::Session*> session,
		Fn<void(std::vector<Track>, QString)> done,
		PeerId peerId = {});
	~ChatMusicSearch();
	void search(const QString &query);
	void searchMore();
	void cancel();
	[[nodiscard]] bool hasMore() const;

private:
	base::weak_ptr<Main::Session> _session;
	Fn<void(std::vector<Track>, QString)> _done;
	PeerId _peerId;
	QString _query;
	std::vector<Track> _tracks;
	Data::MessagePosition _offset;
	mtpRequestId _requestId = 0;
	int _rate = 0;
	int _loaded = 0;
	int _generation = 0;
	bool _more = false;

};

ChatMusicSearch::ChatMusicSearch(
	not_null<Main::Session*> session,
		Fn<void(std::vector<Track>, QString)> done,
		PeerId peerId)
: _session(base::make_weak(session))
, _done(std::move(done))
, _peerId(peerId) {
}

ChatMusicSearch::~ChatMusicSearch() {
	cancel();
}

void ChatMusicSearch::cancel() {
	++_generation;
	if (const auto session = _session.get()) {
		session->api().request(base::take(_requestId)).cancel();
	}
	_more = false;
}

void ChatMusicSearch::search(const QString &query) {
	cancel();
	_query = query;
	_tracks.clear();
	_offset = {};
	_rate = 0;
	_loaded = 0;
	_more = true;
	searchMore();
}

bool ChatMusicSearch::hasMore() const {
	return _more && !_requestId;
}

void ChatMusicSearch::searchMore() {
	const auto session = _session.get();
	if (!session || _requestId || !_more) {
		return;
	}
	const auto generation = _generation;
	const auto received = [=](const MTPmessages_Messages &result) {
		if (generation != _generation
			|| QueueState().session.get() != session) {
			return;
		}
		_requestId = 0;
		if (result.type() == mtpc_messages_messagesNotModified) {
			_more = false;
			_done(_tracks, QString());
			return;
		}
		const auto parsed = Api::ParseGlobalMediaResult(session, result, false);
		_loaded += int(parsed.messageIds.size());
		_more = !parsed.messageIds.empty()
			&& _loaded < parsed.fullCount
			&& (parsed.offsetPosition != _offset || parsed.offsetRate != _rate);
		_offset = parsed.offsetPosition;
		_rate = parsed.offsetRate;
		for (const auto &position : parsed.messageIds) {
			if (const auto item = session->data().message(position.fullId)) {
				auto track = TrackFromMessage(item);
				if (!track.id.isEmpty()
					&& !ranges::contains(_tracks, track.id, &Track::id)) {
					_tracks.push_back(std::move(track));
				}
			}
		}
		_done(_tracks, QString());
	};
	const auto failed = [=](const MTP::Error &error) {
		if (generation != _generation
			|| QueueState().session.get() != session) {
			return;
		}
		_requestId = 0;
		_done(_tracks, MTP::IgnoreError(error) ? QString() : error.type());
	};
	if (_peerId) {
		_requestId = session->api().request(MTPmessages_Search(
			MTP_flags(0),
			session->data().peer(_peerId)->input(),
			MTP_string(_query),
			MTP_inputPeerEmpty(),
			MTP_inputPeerEmpty(),
			MTP_vector<MTPReaction>(),
			MTP_int(0),
			MTP_inputMessagesFilterMusic(),
			MTP_int(0),
			MTP_int(0),
			MTP_int(_offset.fullId.msg),
			MTP_int(0),
			MTP_int(50),
			MTP_int(0),
			MTP_int(0),
			MTP_long(0)
		)).done(received).fail(failed).send();
	} else {
		_requestId = session->api().request(MTPmessages_SearchGlobal(
			MTP_flags(0),
			MTP_int(0),
			MTPInputChannel(),
			MTP_string(_query),
			MTP_inputMessagesFilterMusic(),
			MTP_int(0),
			MTP_int(0),
			MTP_int(_rate),
			(_offset.fullId.peer
				? session->data().peer(_offset.fullId.peer)->input()
				: MTP_inputPeerEmpty()),
			MTP_int(_offset.fullId.msg),
			MTP_int(50)
		)).done(received).fail(failed).send();
	}
}

[[nodiscard]] Track PlayingTrack() {
	const auto current = Media::Player::instance()->current(AudioMsgId::Type::Song);
	const auto document = current.audio();
	if (!document) {
		return {};
	}
	const auto &track = QueueState().current;
	if (OwnsTrack(current)
		&& !track.id.isEmpty()
		&& document->id == DocumentIdForTrack(track.id)) {
		return track;
	}
	if (const auto item = document->session().data().message(current.contextId())) {
		return TrackFromMessage(item);
	}
	const auto name = Ui::Text::FormatSongNameFor(document).composedName();
	auto result = Track();
	result.id = u"telegram:%1"_q.arg(document->id);
	result.title = name.title;
	result.artist = name.performer;
	result.durationMs = document->duration();
	return result;
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
void RefillStream();

void ResolveThen(const Track &track, Fn<void(Track, QString)> done) {
	if (!track.transcodingUrl.isEmpty()) {
		done(track, QString());
		return;
	}
	Resolve(track, std::move(done));
}

// Записывает в историю то, что успели послушать, до перехода на другой трек.
// Возвращает true, если трек пропущен в первые секунды.
bool RecordCurrentListen(bool finished) {
	auto &queue = QueueState();
	if (queue.recorded || queue.current.id.isEmpty()) {
		return false;
	}
	queue.recorded = true;
	auto listened = queue.current.durationMs;
	if (!finished) {
		const auto state = Media::Player::instance()->getState(AudioMsgId::Type::Song);
		const auto frequency = std::max<int64>(state.frequency, 1);
		listened = int(state.position * 1000 / frequency);
	}
	RecordListen(queue.current, listened, finished);
	return !finished && listened < 30 * 1000;
}

bool StreamEnabled() {
	return Mokagram::Settings::Bool(u"music.smart_stream"_q);
}

// Убирает из хвоста очереди то, что добавил подборщик (после позиции from).
void DropUpcoming(Fn<bool(const Track &)> predicate, int from, bool onlyAuto) {
	auto &queue = QueueState();
	for (auto i = int(queue.tracks.size()) - 1; i > from; --i) {
		const auto &t = queue.tracks[i];
		if ((!onlyAuto || queue.autoIds.contains(t.id)) && predicate(t)) {
			queue.autoIds.erase(t.id);
			queue.tracks.erase(queue.tracks.begin() + i);
		}
	}
	queue.changes.fire({});
}

// Дозагрузка очереди с учётом того, что играет сейчас и что недавно было.
void RefillStream() {
	auto &queue = QueueState();
	if (!queue.stream || queue.refilling || !StreamEnabled()) {
		return;
	}
	queue.refilling = true;
	const auto epoch = queue.streamEpoch;
	auto context = StreamContext();
	context.current = queue.current;
	const auto from = std::max(0, queue.index - 4);
	for (auto i = from; i < int(queue.tracks.size()); ++i) {
		if (i != queue.index) {
			context.avoid.push_back(queue.tracks[i]);
		}
	}
	BuildStream(std::move(context), 12, [=](std::vector<Track> tracks, QString note) {
		auto &queue = QueueState();
		if (epoch != queue.streamEpoch) {
			return;
		}
		queue.refilling = false;
		for (auto &t : tracks) {
			queue.autoIds.insert(t.id);
			queue.tracks.push_back(std::move(t));
		}
		queue.changes.fire({});
		if (queue.waiting) {
			queue.waiting = false;
			if (queue.index + 1 < int(queue.tracks.size())) {
				PlayIndex(queue.index + 1);
			} else if (const auto controller = queue.controller.get(); controller && !note.isEmpty()) {
				controller->showToast(note);
			}
		}
	});
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
		if (queue.currentId.isEmpty() || !UsesQueue(state.id)) {
			return;
		}
		if (Media::Player::IsStoppedAtEnd(state.state)) {
			RecordCurrentListen(true);
			const auto count = int(queue.tracks.size());
			const auto &settings = Core::App().settings();
			if (settings.playerRepeatMode() == Media::RepeatMode::One) {
				PlayIndex(queue.index);
			} else if (base::options::value<bool>(
					Media::Player::kOptionDisableAutoplayNext)
				|| !Mokagram::Settings::Bool(u"player.autoplay_next"_q)) {
				return;
			} else if (queue.stream && StreamEnabled()) {
				if (queue.index + 1 < count) {
					PlayIndex(queue.index + 1);
				} else {
					queue.waiting = true;
					RefillStream();
				}
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
	const auto previousArtist = queue.current.artistId;
	const auto skippedEarly = RecordCurrentListen(false);
	if (skippedEarly && queue.stream && !previousArtist.isEmpty()) {
		// Пропуск в первые секунды: убираем автора из предстоящих авто-треков.
		DropUpcoming([=](const Track &t) {
			return t.artistId == previousArtist;
		}, std::max(queue.index, index), true);
	}
	const auto track = queue.tracks[index];
	const auto token = ++queue.pendingToken;
	queue.index = index;
	queue.currentId = track.id;
	queue.changes.fire({});
	if (IsTelegramTrack(track)) {
		const auto item = TrackMessage(track);
		const auto document = item && item->media()
			? item->media()->document()
			: nullptr;
		if (!document) {
			controller->showToast(QString::fromUtf8(
				Mokagram::Lang::Tr("Трек больше недоступен")));
			return;
		}
		queue.current = track;
		queue.recorded = true;
		Media::Player::instance()->play(
			AudioMsgId(document, item->fullId()),
			std::nullopt);
		return;
	}
	controller->showToast(QString::fromUtf8(Mokagram::Lang::Tr("Загружаю: ")) + track.title);
	ResolveThen(track, [=](Track resolved, QString error) {
		auto &queue = QueueState();
		const auto controller = queue.controller.get();
		if (!controller || token != queue.pendingToken) {
			return;
		}
		if (!error.isEmpty()) {
			controller->showToast(QString::fromUtf8(Mokagram::Lang::Tr("Не удалось загрузить: ")) + error);
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
				controller->showToast(QString::fromUtf8(Mokagram::Lang::Tr("Не удалось загрузить: ")) + error);
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
			if (queue.stream && int(queue.tracks.size()) - index - 1 <= 3) {
				RefillStream();
			}
			// Следующий трек качаем заранее, чтобы переход был без паузы.
			if (index + 1 < int(queue.tracks.size())
				&& !IsTelegramTrack(queue.tracks[index + 1])) {
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
				controller->showToast(QString::fromUtf8(Mokagram::Lang::Tr("Не удалось загрузить: ")) + error);
			}
			return;
		}
		Download(resolved, [=](const QString &path, const QString &error) {
			if (!error.isEmpty()) {
				if (const auto controller = QueueState().controller.get()) {
					controller->showToast(QString::fromUtf8(Mokagram::Lang::Tr("Не удалось загрузить: ")) + error);
				}
				return;
			}
			done(path, resolved);
		});
	});
	if (controller) {
		controller->showToast(QString::fromUtf8(Mokagram::Lang::Tr("Загружаю: ")) + track.title);
	}
}

void UploadTrack(
		not_null<History*> history,
		const Track &track,
		const QString &path,
		const QImage &cover) {
	const auto session = &history->session();
	auto list = Storage::PrepareMediaList(
		QStringList{ NamedCopy(track, path) },
		st::sendMediaPreviewSize,
		session->user()->isPremium());
	if (list.error != Ui::PreparedList::Error::None || list.files.empty()) {
		return;
	}
	auto &file = list.files.front();
	auto &information = *file.information;
	auto song = std::get_if<Ui::PreparedFileInformation::Song>(
		&information.media);
	if (!song) {
		information.media = Ui::PreparedFileInformation::Song();
		song = std::get_if<Ui::PreparedFileInformation::Song>(
			&information.media);
	}
	if (song->duration <= 0) {
		song->duration = track.durationMs;
	}
	if (!track.title.isEmpty()) {
		song->title = track.title;
	}
	if (!track.artist.isEmpty()) {
		song->performer = track.artist;
	}
	if (!cover.isNull()) {
		song->cover = cover;
	}
	file.type = Ui::PreparedFile::Type::Music;
	auto action = Api::SendAction(history);
	action.clearDraft = false;
	session->api().sendFiles(
		std::move(list),
		SendMediaType::File,
		nullptr,
		std::move(action));
}

void SendTrack(
		not_null<History*> history,
		const Track &track,
		const QString &path) {
	const auto cover = CoverForDocument(DocumentIdForTrack(track.id));
	if (!cover.isNull() || track.artworkUrl.isEmpty()) {
		UploadTrack(history, track, path, cover);
		return;
	}
	const auto weak = base::make_weak(history.get());
	LoadImage(ArtUrl(track.artworkUrl, u"t500x500"_q), [=](QImage image) {
		if (const auto history = weak.get()) {
			UploadTrack(history, track, path, image);
		}
	});
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
				controller->showToast(QString::fromUtf8(Mokagram::Lang::Tr("Сохранено: ")) + target, 4000);
			}
		} else if (controller) {
			controller->showToast(QString::fromUtf8(Mokagram::Lang::Tr("Не удалось сохранить файл")));
		}
	});
}

void ForwardTrack(
		not_null<Window::SessionController*> controller,
		const Track &track) {
	if (IsTelegramTrack(track)) {
		const auto item = TrackMessage(track);
		if (item && item->allowsForward()) {
			Window::ShowForwardMessagesBox(
				controller,
				MessageIdsList{ item->fullId() });
		}
		return;
	}
	Window::ShowChooseRecipientBox(
		controller,
		[=](not_null<Data::Thread*> thread) {
			const auto weak = base::make_weak(thread->owningHistory().get());
			EnsureFile(track, [=](const QString &path, const Track &track) {
				if (const auto history = weak.get()) {
					SendTrack(history, track, path);
				}
			});
			return true;
		},
		rpl::single(QString::fromUtf8(Mokagram::Lang::Tr("Переслать трек"))));
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
						controller->showToast(QString::fromUtf8(Mokagram::Lang::Tr("Добавлено в профиль: ")) + track.title);
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
[[nodiscard]] std::vector<Track> &ContextTracks() {
	static auto result = std::vector<Track>();
	return result;
}

void PlayFromResults(int resultIndex);

// Трек из списка чата, откуда играет Telegram: запускаем его уже внутри очереди плеера.
void PlayContext(int index) {
	auto &queue = QueueState();
	if (index < 0 || index >= int(ContextTracks().size())) {
		return;
	}
	queue.results = ContextTracks();
	PlayFromResults(index);
}

void PlayFromResults(int resultIndex) {
	auto &queue = QueueState();
	if (resultIndex < 0 || resultIndex >= int(queue.results.size())) {
		return;
	}
	const auto track = queue.results[resultIndex];
	if (!queue.custom) {
		queue.tracks = queue.results;
		queue.stream = false;
		++queue.streamEpoch;
		queue.refilling = false;
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

constexpr auto kLikeDuration = crl::time(650);

[[nodiscard]] bool LikeAnimationsEnabled() {
	return !PowerSaving::On(PowerSaving::kAnimations);
}

// Масштаб сердца по ходу анимации лайка: затухающая пружина к 1.
[[nodiscard]] float64 LikeScale(float64 progress, bool liked) {
	const auto from = liked ? 0.4 : 0.75;
	return 1.
		- (1. - from) * std::exp(-7. * progress) * std::cos(14. * progress);
}

// Вспышка вокруг сердца при лайке: расходящееся кольцо и искры.
void DrawLikeBurst(
		QPainter &p,
		QPointF center,
		float64 radius,
		float64 progress,
		const QColor &color) {
	const auto easeOut = [](float64 t) {
		return 1. - std::pow(1. - t, 3);
	};
	p.save();
	if (progress < 0.45) {
		const auto t = progress / 0.45;
		auto ring = color;
		ring.setAlphaF(0.9 * (1. - t));
		const auto width = std::max(radius * 0.3 * (1. - t), 0.5);
		const auto r = radius * (0.35 + 0.6 * easeOut(t)) - width / 2.;
		p.setPen(QPen(ring, width));
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(center, r, r);
	}
	const auto t = std::clamp((progress - 0.12) / 0.6, 0., 1.);
	if (t > 0. && t < 1.) {
		static const auto spark = QColor(255, 179, 71);
		const auto distance = radius * (0.55 + 0.35 * easeOut(t));
		const auto size = radius * 0.12 * (1. - t);
		constexpr auto kCount = 7;
		p.setPen(Qt::NoPen);
		for (auto i = 0; i != kCount; ++i) {
			const auto angle = -M_PI / 2. + i * 2. * M_PI / kCount;
			const auto big = center
				+ QPointF(std::cos(angle), std::sin(angle)) * distance;
			const auto side = angle + 0.35;
			const auto small = center
				+ QPointF(std::cos(side), std::sin(side)) * distance * 0.8;
			p.setBrush(color);
			p.drawEllipse(big, size, size);
			p.setBrush(spark);
			p.drawEllipse(small, size * 0.7, size * 0.7);
		}
	}
	p.restore();
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

class SourceSwitch final : public Ui::AbstractButton {
public:
	SourceSwitch(QWidget *parent, QString first, QString second)
	: AbstractButton(parent)
	, _labels{ std::move(first), std::move(second) } {
	resize(width(), 40);
	setPointerCursor(true);
	setClickedCallback([=] {
		const auto chats = mapFromGlobal(QCursor::pos()).x() >= width() / 2;
		if (_chats != chats && _changed) {
			_changed(chats);
		}
	});
	paintRequest(
	) | rpl::on_next([=](QRect) {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(rect(), height() / 2., height() / 2.);
		const auto margin = 4;
		const auto half = (width() - 2 * margin) / 2;
		const auto progress = _animation.value(_chats ? 1. : 0.);
		// Плашка едет к выбранной половине и слегка вытягивается на ходу.
		const auto stretch = int(base::SafeRound(
			half * 0.18 * (1. - std::abs(2. * progress - 1.))));
		const auto left = margin + int(base::SafeRound(half * progress));
		const auto selected = QRect(
			left - stretch / 2,
			margin,
			half + stretch,
			height() - 2 * margin);
		p.setBrush(st::windowActiveTextFg->c);
		p.drawRoundedRect(selected, selected.height() / 2., selected.height() / 2.);
		p.setFont(st::semiboldFont);
		for (auto i = 0; i != 2; ++i) {
			const auto active = (i == 1) ? progress : (1. - progress);
			p.setPen(anim::color(st::windowSubTextFg, st::windowFg, active));
			p.drawText(QRect(i * width() / 2, 0, width() / 2, height()), Qt::AlignCenter, _labels[i]);
		}
	}, lifetime());
	}
	void setChats(bool chats) {
		if (_chats != chats) {
			_chats = chats;
			_animation.start(
				[=] { update(); },
				_chats ? 0. : 1.,
				_chats ? 1. : 0.,
				kSwitchDuration,
				anim::easeOutCubic);
		}
	}
	void setChangedCallback(Fn<void(bool)> callback) {
		_changed = std::move(callback);
	}
	void finishAnimating() {
		_animation.stop();
		update();
	}

private:
	const std::array<QString, 2> _labels;
	Ui::Animations::Simple _animation;
	Fn<void(bool)> _changed;
	bool _chats = false;

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
				QString::fromUtf8(Mokagram::Lang::Tr("Слушать подборку")));
			p.setFont(st::normalFont);
			p.setPen(onRefresh ? st::windowFg : st::windowSubTextFg);
			p.drawText(
				refreshRect(),
				Qt::AlignVCenter | Qt::AlignRight,
				QString::fromUtf8(Mokagram::Lang::Tr("Обновить")));
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
	queue.stream = StreamEnabled();
	queue.refilling = false;
	queue.waiting = false;
	++queue.streamEpoch;
	queue.autoIds.clear();
	if (queue.stream) {
		for (const auto &t : tracks) {
			queue.autoIds.insert(t.id);
		}
	}
	PlayIndex(0);
}

enum class RowMode { Results, Queue, Context };

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
		_equalizerTimer.setInterval(33);
		connect(&_equalizerTimer, &QTimer::timeout, this, [=] {
			_equalizerPhase += _equalizerClock.restart() / 1000.;
			update(QRect(20, (height() - kArtSize) / 2, kArtSize, kArtSize));
		});
		Media::Player::instance()->updatedNotifier(
		) | rpl::on_next([=] { updateAnimation(); }, lifetime());
		PowerSaving::Changes(
		) | rpl::on_next([=] { updateAnimation(); }, lifetime());
		connect(qApp, &QGuiApplication::applicationStateChanged, this, [=] {
			updateAnimation();
		});
		if (const auto item = TrackMessage(track)) {
			const auto document = item->media()->document();
			_media = document->createMediaView();
			_media->thumbnailWanted(item->fullId());
			document->session().downloaderTaskFinished(
			) | rpl::on_next([=] { update(); }, lifetime());
		} else if (!IsTelegramTrack(track)) {
			LoadImage(ArtUrl(track.artworkUrl, u"t200x200"_q), crl::guard(this, [=](QImage image) {
				_art = std::move(image);
				update();
			}));
		}
		QueueState().changes.events(
		) | rpl::on_next([=] {
			updateAnimation();
			update();
		}, lifetime());
		LikesChanged(
		) | rpl::on_next([=] {
			update();
		}, lifetime());
		setClickedCallback([=] {
			if (!IsTelegramTrack(_track)
				&& heartRect().contains(mapFromGlobal(QCursor::pos()))) {
				// Во вкладке «Любимые» LikesChanged пересобирает список и удаляет эту строку.
				const auto weak = base::make_weak(this);
				SetLiked(_track, !IsLiked(_track.id));
				if (weak && LikeAnimationsEnabled()) {
					_likeAnimation.stop();
					_likeAnimation.start([=] { update(); }, 0., 1., kLikeDuration);
				}
				return;
			}
			if (_mode == RowMode::Queue) {
				PlayIndex(index);
			} else if (_mode == RowMode::Context) {
				PlayContext(index);
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

protected:
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;

private:
	void updateAnimation();

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
		add(QString::fromUtf8(Mokagram::Lang::Tr("Воспроизвести")), [=] {
			if (_mode == RowMode::Queue) {
				PlayIndex(index);
			} else if (_mode == RowMode::Context) {
				PlayContext(index);
			} else {
				PlayFromResults(index);
			}
		}, &st::menuIconSoundOn);
		if (!IsTelegramTrack(track)) {
			add(IsLiked(track.id)
				? QString::fromUtf8(Mokagram::Lang::Tr("Убрать из избранного"))
				: QString::fromUtf8(Mokagram::Lang::Tr("В избранное")), [=] {
				SetLiked(track, !IsLiked(track.id));
			}, &st::menuIconFave);
		}
		if (_mode == RowMode::Results) {
			add(QString::fromUtf8(Mokagram::Lang::Tr("Играть следующим")), [=] { EnqueueNext(track); controller->showToast(QString::fromUtf8(Mokagram::Lang::Tr("Добавлено в очередь"))); }, &st::menuIconAbove);
			add(QString::fromUtf8(Mokagram::Lang::Tr("В конец очереди")), [=] { EnqueueLast(track); controller->showToast(QString::fromUtf8(Mokagram::Lang::Tr("Добавлено в очередь"))); }, &st::menuIconBelow);
		} else {
			add(QString::fromUtf8(Mokagram::Lang::Tr("Выше")), [=] { QueueMove(index, -1); }, &st::menuIconAbove);
			add(QString::fromUtf8(Mokagram::Lang::Tr("Ниже")), [=] { QueueMove(index, 1); }, &st::menuIconBelow);
			add(QString::fromUtf8(Mokagram::Lang::Tr("Убрать из очереди")), [=] { QueueRemove(index); }, &st::menuIconDelete);
		}
		if (CanForwardTrack(track)) {
			add(QString::fromUtf8(Mokagram::Lang::Tr("Переслать")), [=] {
				ForwardTrack(controller, track);
			}, &st::menuIconForward);
		}
		if (IsTelegramTrack(track)) {
			add(QString::fromUtf8(Mokagram::Lang::Tr("Показать в чате")), [=] {
				if (const auto item = TrackMessage(track)) {
					controller->showPeerHistory(item->history(), {}, item->id);
				}
			}, &st::menuIconChats);
		} else {
			add(QString::fromUtf8(Mokagram::Lang::Tr("Скачать")), [=] { SaveToDisk(track); }, &st::menuIconDownload);
			add(QString::fromUtf8(Mokagram::Lang::Tr("Добавить в профиль")), [=] { AddToProfile(controller, track); }, &st::menuIconProfile);
			add(QString::fromUtf8(Mokagram::Lang::Tr("Копировать ссылку")), [=] {
				QGuiApplication::clipboard()->setText(track.permalink);
				controller->showToast(QString::fromUtf8(Mokagram::Lang::Tr("Ссылка скопирована")));
			}, &st::menuIconLink);
		}
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
		if (_media) {
			if (const auto image = _media->thumbnail()) {
				_art = image->original();
			} else if (const auto image = _media->thumbnailInline()) {
				_art = image->original();
			}
		}
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
			for (auto i = 0; i != 3; ++i) {
				const auto wave = std::sin(_equalizerPhase * (5. + i) + i * 2.1);
				const auto barHeight = 6. + 7. * (wave + 1.);
				p.drawRoundedRect(
					QRectF(left + i * 8, base - barHeight, 4, barHeight),
					2,
					2);
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
		if (!IsTelegramTrack(_track)) {
			const auto progress = _likeAnimation.value(1.);
			auto glyph = QRectF(heart).adjusted(5, 6, -5, -5);
			if (progress < 1.) {
				if (liked) {
					DrawLikeBurst(p, glyph.center(), 14., progress, HeartColor());
				}
				const auto scale = LikeScale(progress, liked);
				const auto center = glyph.center();
				glyph = QRectF(
					center - QPointF(glyph.width(), glyph.height()) * scale / 2.,
					glyph.size() * scale);
			}
			DrawHeart(
				p,
				glyph,
				liked,
				liked ? HeartColor() : QColor(st::windowSubTextFg->c));
		}
		const auto textWidth = (IsTelegramTrack(_track)
			? right - durationWidth - 14
			: heart.left() - 12) - left;
		p.setFont(st::semiboldFont);
		p.setPen(playing ? st::windowActiveTextFg : st::windowFg);
		p.drawText(
			QRect(left, height() / 2 - 24, textWidth, 22),
			Qt::AlignVCenter | Qt::AlignLeft,
			st::semiboldFont->elided(_track.title, textWidth));
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		const auto item = TrackMessage(_track);
		const auto subtitle = item
			? _track.artist + u" · "_q + item->history()->peer->name()
			: _track.artist;
		p.drawText(
			QRect(left, height() / 2 + 2, textWidth, 20),
			Qt::AlignVCenter | Qt::AlignLeft,
			st::normalFont->elided(subtitle, textWidth));
	}

	QTimer _equalizerTimer;
	QElapsedTimer _equalizerClock;
	double _equalizerPhase = 0.;
	Track _track;
	RowMode _mode;
	QImage _art;
	std::shared_ptr<Data::DocumentMedia> _media;
	base::unique_qptr<Ui::PopupMenu> _menu;
	Ui::Animations::Simple _likeAnimation;

};

void Row::updateAnimation() {
	const auto instance = Media::Player::instance();
	const auto state = instance->getState(AudioMsgId::Type::Song);
	const auto animate = isVisible()
		&& QGuiApplication::applicationState() == Qt::ApplicationActive
		&& QueueState().currentId == _track.id
		&& UsesQueue(state.id)
		&& (state.state == Media::Player::State::Playing
			|| state.state == Media::Player::State::Starting
			|| state.state == Media::Player::State::Resuming)
		&& !PowerSaving::On(PowerSaving::kAnimations);
	if (animate && !_equalizerTimer.isActive()) {
		_equalizerClock.start();
		_equalizerTimer.start();
	} else if (!animate) {
		_equalizerTimer.stop();
	}
}

void Row::showEvent(QShowEvent *e) {
	AbstractButton::showEvent(e);
	updateAnimation();
}

void Row::hideEvent(QHideEvent *e) {
	_equalizerTimer.stop();
	AbstractButton::hideEvent(e);
}

// ---- полноценный плеер ----

// Загрузка списка: бегущая полоска, подпись и мерцающие заглушки строк.
class LoadingView final : public Ui::RpWidget {
public:
	LoadingView(QWidget *parent, const QString &text)
	: RpWidget(parent)
	, _text(text) {
		resize(width(), kHeaderHeight + kRows * kRowStep + 8);
		paintRequest(
		) | rpl::on_next([=](QRect) {
			paint();
		}, lifetime());
		if (!PowerSaving::On(PowerSaving::kAnimations)) {
			_started = crl::now();
			_animation.init([=](crl::time) {
				update();
				return true;
			});
			_animation.start();
		}
	}

private:
	static constexpr auto kHeaderHeight = 44;
	static constexpr auto kRows = 4;
	static constexpr auto kRowStep = 60;

	void paint() {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto time = _started ? (crl::now() - _started) : crl::time(0);
		const auto skeleton = [&] {
			auto color = st::windowSubTextFg->c;
			color.setAlphaF(0.16);
			return color;
		}();

		// Подпись слегка «дышит».
		auto textColor = st::windowSubTextFg->c;
		textColor.setAlphaF(_started
			? 0.6 + 0.4 * (0.5 + 0.5 * std::sin(time / 260.))
			: 1.);
		p.setPen(textColor);
		p.setFont(st::normalFont);
		p.drawText(
			QRect(24, 0, width() - 48, 24),
			Qt::AlignLeft | Qt::AlignVCenter,
			_text);

		// Неопределённая полоска: сегмент бежит слева направо.
		const auto barRect = QRectF(24, 28, width() - 48, 4);
		p.setPen(Qt::NoPen);
		p.setBrush(skeleton);
		p.drawRoundedRect(barRect, 2, 2);
		const auto phase = std::fmod(time / 1100., 1.);
		const auto eased = 1. - std::pow(1. - phase, 2.);
		const auto segment = barRect.width() * 0.32;
		const auto x = barRect.left()
			- segment
			+ (barRect.width() + segment) * eased;
		p.save();
		auto clip = QPainterPath();
		clip.addRoundedRect(barRect, 2, 2);
		p.setClipPath(clip);
		p.setBrush(st::windowActiveTextFg->c);
		p.drawRoundedRect(QRectF(x, barRect.top(), segment, barRect.height()), 2, 2);
		p.restore();

		// Заглушки строк с бегущим бликом.
		const auto sweep = _started
			? std::fmod(time / 1400., 1.) * (width() + 160) - 80
			: -1000.;
		for (auto i = 0; i != kRows; ++i) {
			const auto top = kHeaderHeight + i * kRowStep;
			auto shape = QPainterPath();
			shape.addRoundedRect(QRectF(16, top + 6, 44, 44), 8, 8);
			shape.addRoundedRect(QRectF(72, top + 12, width() * 0.45 - i * 14, 10), 5, 5);
			shape.addRoundedRect(QRectF(72, top + 32, width() * 0.28, 9), 4.5, 4.5);
			p.setPen(Qt::NoPen);
			p.setBrush(skeleton);
			p.drawPath(shape);
			p.save();
			p.setClipPath(shape);
			auto glow = QLinearGradient(sweep - 70, 0, sweep + 70, 0);
			auto edge = st::windowFg->c;
			edge.setAlphaF(0.);
			auto mid = st::windowFg->c;
			mid.setAlphaF(0.10);
			glow.setColorAt(0., edge);
			glow.setColorAt(0.5, mid);
			glow.setColorAt(1., edge);
			p.fillRect(QRectF(0, top, width(), kRowStep), glow);
			p.restore();
		}
	}

	const QString _text;
	Ui::Animations::Basic _animation;
	crl::time _started = 0;

};

constexpr auto kHintStickerKey = "music.hint_sticker";

// Свой стикер вместо стандартного: документ целиком, с версией формата,
// чтобы после перезапуска не зависеть от загруженных наборов.
[[nodiscard]] DocumentData *LoadHintSticker(not_null<Main::Session*> session) {
	const auto encoded = Mokagram::Settings::String(
		QString::fromLatin1(kHintStickerKey));
	if (encoded.isEmpty()) {
		return nullptr;
	}
	auto bytes = QByteArray::fromBase64(encoded.toLatin1());
	auto stream = QDataStream(&bytes, QIODevice::ReadOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	auto version = qint32();
	stream >> version;
	const auto document = Serialize::Document::readFromStream(
		session,
		version,
		stream);
	return (stream.status() == QDataStream::Ok
		&& document
		&& document->sticker())
		? document
		: nullptr;
}

void SaveHintSticker(DocumentData *document) {
	const auto key = QString::fromLatin1(kHintStickerKey);
	if (!document) {
		Mokagram::Settings::Reset(key);
		return;
	}
	auto bytes = QByteArray();
	{
		auto stream = QDataStream(&bytes, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		stream << qint32(AppVersion);
		Serialize::Document::writeToStream(stream, document);
	}
	Mokagram::Settings::SetString(key, QString::fromLatin1(bytes.toBase64()));
}

// Пустой поиск: анимированный стикер посередине. Правой кнопкой его
// можно сменить на любой свой стикер.
class StickerHint final : public Ui::RpWidget {
public:
	StickerHint(
		QWidget *parent,
		not_null<Main::Session*> session,
		Fn<void(QPoint)> showMenu)
	: RpWidget(parent)
	, _session(session)
	, _showMenu(std::move(showMenu)) {
		_sheet = QPixmap(u":/gui/art/mokagram_music_hint.png"_q);
		_sheet.setDevicePixelRatio(2.);
		resize(width(), kSide + 56);
		paintRequest(
		) | rpl::on_next([=](QRect) {
			paint();
		}, lifetime());
		Mokagram::Settings::StringValue(
			QString::fromLatin1(kHintStickerKey)
		) | rpl::on_next([=](const QString &) {
			loadSticker();
		}, lifetime());
		if (!PowerSaving::On(PowerSaving::kAnimations)) {
			_started = crl::now();
			_animation.init([=](crl::time) {
				update();
				return true;
			});
			_animation.start();
		}
	}

protected:
	void contextMenuEvent(QContextMenuEvent *e) override {
		if (_showMenu) {
			_showMenu(e->globalPos());
		}
	}

private:
	void loadSticker() {
		_playerLifetime.destroy();
		_player = nullptr;
		_custom = LoadHintSticker(_session);
		update();
		if (!_custom) {
			return;
		}
		const auto document = not_null<DocumentData*>(_custom);
		const auto media = document->createMediaView();
		media->checkStickerLarge();
		media->goodThumbnailWanted();
		rpl::single() | rpl::then(
			_session->downloaderTaskFinished()
		) | rpl::filter([=] {
			return media->loaded();
		}) | rpl::take(1) | rpl::on_next([=] {
			using namespace HistoryView;
			const auto info = document->sticker();
			const auto box = QSize(kSide, kSide);
			if (info->isLottie()) {
				_player = std::make_shared<LottiePlayer>(
					ChatHelpers::LottiePlayerFromDocument(
						media.get(),
						ChatHelpers::StickerLottieSize::ChatIntroHelloSticker,
						box,
						Lottie::Quality::High));
			} else if (info->isWebm()) {
				_player = std::make_shared<WebmPlayer>(
					media->owner()->location(),
					media->bytes(),
					box);
			} else {
				_player = std::make_shared<StaticStickerPlayer>(
					media->owner()->location(),
					media->bytes(),
					box);
			}
			_player->setRepaintCallback([=] { update(); });
			update();
		}, _playerLifetime);
	}

	void paint() {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto target = QRect((width() - kSide) / 2, 28, kSide, kSide);
		if (_custom) {
			// Пока свой стикер грузится, место остаётся пустым,
			// чтобы стандартный не мелькал перед ним.
			if (!_player || !_player->ready()) {
				return;
			}
			const auto paused = !_started || !window()->isActiveWindow();
			const auto frame = _player->frame(
				QSize(kSide, kSide),
				QColor(0, 0, 0, 0),
				false,
				crl::now(),
				paused).image;
			const auto size = HistoryView::DownscaledSize(
				frame.size() / style::DevicePixelRatio(),
				QSize(kSide, kSide));
			p.drawImage(
				QRect(
					target.x() + (kSide - size.width()) / 2,
					target.y() + (kSide - size.height()) / 2,
					size.width(),
					size.height()),
				frame);
			_player->markFrameShown();
			return;
		}
		if (_sheet.isNull()) {
			return;
		}
		const auto frame = _started
			? int(((crl::now() - _started) / kFrameDuration) % kFrames)
			: 0;
		// Кадр в листе задан в пикселях устройства, а целевой квадрат —
		// в логических, иначе при DPR 2 картинка ужимается и уезжает влево.
		p.drawPixmap(
			target,
			_sheet,
			QRect(
				(frame % kColumns) * kSource,
				(frame / kColumns) * kSource,
				kSource,
				kSource));
	}

	static constexpr auto kSource = 192;
	static constexpr auto kSide = kSource / 2;
	static constexpr auto kColumns = 9;
	static constexpr auto kFrames = 49;
	static constexpr auto kFrameDuration = crl::time(1000 / 15);

	const not_null<Main::Session*> _session;
	const Fn<void(QPoint)> _showMenu;
	QPixmap _sheet;
	Ui::Animations::Basic _animation;
	crl::time _started = 0;
	DocumentData *_custom = nullptr;
	std::shared_ptr<HistoryView::StickerPlayer> _player;
	rpl::lifetime _playerLifetime;

};

class SeekBar final : public Ui::RpWidget {
public:
	explicit SeekBar(QWidget *parent) : RpWidget(parent) {
		resize(width(), 28);
		setCursor(style::cur_pointer);
		paintRequest(
		) | rpl::on_next([=](QRect) {
			auto p = QPainter(this);
			auto hq = PainterHighQualityEnabler(p);
			if (_thin) {
				// Тонкая полоса: толще на наведении и при перетаскивании.
				const auto grow = _grow.value((_over || _dragging) ? 1. : 0.);
				const auto h = 4. + 4. * grow;
				const auto line = QRectF(0, height() / 2. - h / 2., width(), h);
				p.setPen(Qt::NoPen);
				p.setBrush(QColor(255, 255, 255, 46));
				p.drawRoundedRect(line, h / 2., h / 2.);
				const auto progress = _dragging ? _drag : _progress;
				auto fill = line;
				fill.setWidth(std::max(line.width() * progress, h));
				p.setBrush(_fill);
				p.drawRoundedRect(fill, h / 2., h / 2.);
				return;
			}
			const auto line = QRectF(8, height() / 2. - 2, width() - 16, 4);
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(line, 3, 3);
			const auto progress = _dragging ? _drag : _progress;
			auto fill = line;
			fill.setWidth(line.width() * progress);
			p.setBrush(st::windowFg);
			p.drawRoundedRect(fill, 2, 2);
			const auto knob = QPointF(fill.right(), line.center().y());
			p.setBrush(Qt::white);
			p.drawEllipse(knob, _dragging ? 8. : 6., _dragging ? 8. : 6.);
		}, lifetime());
	}

	void setFillColor(QColor color) {
		_fill = color;
		update();
	}
	void setThin(bool thin) {
		_thin = thin;
		setMouseTracking(true);
		update();
	}
	void setProgress(float64 value) {
		value = std::clamp(value, 0., 1.);
		if (_progress == value) {
			return;
		}
		_progress = value;
		if (!_dragging) {
			update();
		}
	}
	[[nodiscard]] rpl::producer<float64> sought() const {
		return _sought.events();
	}
	// Пока тянут: значение меняется на лету (для громкости).
	[[nodiscard]] rpl::producer<float64> moved() const {
		return _moved.events();
	}

protected:
	void enterEventHook(QEnterEvent *e) override {
		setOver(true);
	}
	void leaveEventHook(QEvent *e) override {
		setOver(false);
	}
	void mousePressEvent(QMouseEvent *e) override {
		_dragging = true;
		startGrow();
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
			startGrow();
			moveTo(e->pos().x());
			_progress = _drag;
			_sought.fire_copy(_drag);
		}
	}

private:
	void setOver(bool over) {
		if (_over != over) {
			_over = over;
			startGrow();
		}
	}
	void startGrow() {
		const auto to = (_over || _dragging) ? 1. : 0.;
		_grow.start([=] { update(); }, 1. - to, to, 180, anim::easeOutCubic);
	}
	void moveTo(int x) {
		const auto pad = _thin ? 0 : 8;
		_drag = std::clamp((x - pad) / float64(std::max(width() - 2 * pad, 1)), 0., 1.);
		_moved.fire_copy(_drag);
		update();
	}

	Ui::Animations::Simple _grow;
	bool _thin = false;
	bool _over = false;
	QColor _fill = Qt::white;
	float64 _progress = 0.;
	float64 _drag = 0.;
	bool _dragging = false;
	rpl::event_stream<float64> _sought;
	rpl::event_stream<float64> _moved;

};

enum class Glyph { Previous, Next, Play, Add, Shuffle, Repeat, More, Block, Collapse, Queue };

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
			if (_overlay && liked) {
				_pop.start([=] { update(); }, 0., 1., 360, anim::easeOutCubic);
			}
			update();
		}
	}
	void animateLike(bool liked) {
		setLiked(liked);
		if (LikeAnimationsEnabled()) {
			_likeAnimation.stop();
			_likeAnimation.start([=] { update(); }, 0., 1., kLikeDuration);
		}
	}
	// Кнопки полноэкранного плеера: белые значки, тёмный полупрозрачный круг.
	void setOverlay(bool overlay) {
		_overlay = overlay;
		update();
	}

protected:
	void onStateChanged(State was, StateChangeSource source) override {
		AbstractButton::onStateChanged(was, source);
		if (_overlay && ((was & StateFlag::Over) != (state() & StateFlag::Over))) {
			const auto over = isOver();
			_hover.start([=] { update(); }, over ? 0. : 1., over ? 1. : 0., 180, anim::easeOutCubic);
		}
		if (_overlay && ((was & StateFlag::Down) != (state() & StateFlag::Down))) {
			const auto down = isDown();
			_press.start([=] { update(); }, down ? 0. : 1., down ? 1. : 0., 120, anim::easeOutCubic);
		}
	}

private:
	void paintOverlay(QPainter &p) {
		auto hq = PainterHighQualityEnabler(p);
		const auto r = QRectF(rect());
		auto c = r.center();
		const auto over = _hover.value(isOver() ? 1. : 0.);
		const auto down = _press.value(isDown() ? 1. : 0.);
		const auto dark = (_glyph == Glyph::Add
			|| _glyph == Glyph::Queue
			|| _glyph == Glyph::Collapse
			|| _glyph == Glyph::Play);
		p.setPen(Qt::NoPen);
		p.setBrush(dark
			? QColor(0, 0, 0, int(80 + 50 * over))
			: QColor(255, 255, 255, int(40 * over)));
		p.translate(c);
		const auto squeeze = 1. - 0.08 * down;
		p.scale(squeeze, squeeze);
		p.translate(-c);
		p.drawEllipse(r);
		const auto pen = QPen(Qt::white, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
		const auto accent = QColor(0x0a, 0x84, 0xff);
		const auto u = r.width() / 48.;
		if (_glyph == Glyph::Collapse) {
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			p.drawPolyline(QPolygonF({
				c + QPointF(-7, -3) * u * 1.2,
				c + QPointF(0, 4) * u * 1.2,
				c + QPointF(7, -3) * u * 1.2 }));
			return;
		}
		if (_glyph == Glyph::Queue) {
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			for (auto i = 0; i != 3; ++i) {
				const auto y = c.y() + (i - 1) * 6 * u * 1.2;
				p.drawLine(QPointF(c.x() - 3 * u * 1.2, y), QPointF(c.x() + 8 * u * 1.2, y));
				p.drawPoint(QPointF(c.x() - 8 * u * 1.2, y));
			}
			return;
		}
		if (_glyph == Glyph::Add) {
			const auto pop = 1. + 0.3 * std::sin(_pop.value(1.) * M_PI);
			p.translate(c);
			p.scale(pop * 0.8, pop * 0.8);
			auto heart = QPainterPath();
			heart.moveTo(QPointF(0, 8.5));
			heart.cubicTo(QPointF(-14, -1), QPointF(-8.5, -11.5), QPointF(0, -4));
			heart.cubicTo(QPointF(8.5, -11.5), QPointF(14, -1), QPointF(0, 8.5));
			heart.closeSubpath();
			if (_liked) {
				p.setPen(Qt::NoPen);
				p.setBrush(QColor(0xFF, 0x3B, 0x5C));
			} else {
				p.setPen(QPen(Qt::white, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
				p.setBrush(Qt::NoBrush);
			}
			p.drawPath(heart);
			return;
		}
		if (_glyph == Glyph::Shuffle || _glyph == Glyph::Repeat) {
			const auto &settings = Core::App().settings();
			const auto shuffle = (_glyph == Glyph::Shuffle);
			const auto mode = settings.playerRepeatMode();
			const auto active = shuffle
				? settings.playerOrderMode() == Media::OrderMode::Shuffle
				: mode != Media::RepeatMode::None;
			const auto &icon = shuffle
				? st::mediaPlayerOrderIconShuffle
				: mode == Media::RepeatMode::One
				? st::mediaPlayerRepeatOneIcon
				: st::mediaPlayerRepeatDisabledIcon;
			const auto scale = 28. / std::max(icon.width(), icon.height());
			p.translate(
				c.x() - icon.width() * scale / 2.,
				c.y() - icon.height() * scale / 2.);
			p.scale(scale, scale);
			icon.paint(p, QPoint(), icon.width(), active ? accent : QColor(Qt::white));
			return;
		}
		p.setBrush(Qt::white);
		p.setPen(Qt::NoPen);
		if (_glyph == Glyph::Play) {
			if (_playing) {
				p.drawRoundedRect(QRectF(c.x() - 11 * u, c.y() - 12 * u, 8 * u, 24 * u), 2.5 * u, 2.5 * u);
				p.drawRoundedRect(QRectF(c.x() + 3 * u, c.y() - 12 * u, 8 * u, 24 * u), 2.5 * u, 2.5 * u);
			} else {
				auto path = QPainterPath();
				path.moveTo(c.x() - 7 * u, c.y() - 12 * u);
				path.lineTo(c.x() + 13 * u, c.y());
				path.lineTo(c.x() - 7 * u, c.y() + 12 * u);
				path.closeSubpath();
				p.drawPath(path);
			}
			return;
		}
		const auto dir = (_glyph == Glyph::Next) ? 1. : -1.;
		auto path = QPainterPath();
		path.moveTo(c.x() + dir * (-9) * u, c.y() - 11 * u);
		path.lineTo(c.x() + dir * 7 * u, c.y());
		path.lineTo(c.x() + dir * (-9) * u, c.y() + 11 * u);
		path.closeSubpath();
		p.drawPath(path);
		p.drawRoundedRect(QRectF(c.x() + dir * 9 * u - 2 * u, c.y() - 11 * u, 4 * u, 22 * u), 1.5 * u, 1.5 * u);
	}

	void paint() {
		auto p = QPainter(this);
		if (_overlay) {
			paintOverlay(p);
			return;
		}
		auto hq = PainterHighQualityEnabler(p);
		const auto r = QRectF(rect());
		const auto c = r.center();
		p.setPen(Qt::NoPen);
		if (_glyph == Glyph::Add) {
			// Лайк: контурное сердце, после добавления в профиль закрашено.
			auto heart = QPainterPath();
			heart.moveTo(c + QPointF(0, 8.5));
			heart.cubicTo(
				c + QPointF(-14, -1),
				c + QPointF(-8.5, -11.5),
				c + QPointF(0, -4));
			heart.cubicTo(
				c + QPointF(8.5, -11.5),
				c + QPointF(14, -1),
				c + QPointF(0, 8.5));
			heart.closeSubpath();
			const auto progress = _likeAnimation.value(1.);
			if (progress < 1.) {
				if (_liked) {
					DrawLikeBurst(p, c, 18., progress, QColor(0xFF, 0x3B, 0x5C));
				}
				const auto scale = LikeScale(progress, _liked);
				p.translate(c);
				p.scale(scale, scale);
				p.translate(-c);
			}
			if (_liked) {
				p.setPen(Qt::NoPen);
				p.setBrush(QColor(0xFF, 0x3B, 0x5C));
			} else {
				p.setPen(QPen(
					QColor(st::windowFg->c),
					2.,
					Qt::SolidLine,
					Qt::RoundCap,
					Qt::RoundJoin));
				p.setBrush(Qt::NoBrush);
			}
			p.drawPath(heart);
			return;
		}
		if (_glyph == Glyph::More || _glyph == Glyph::Block) {
			const auto pen = QPen(
				isOver() ? QColor(st::windowActiveTextFg->c) : QColor(st::windowFg->c),
				2.,
				Qt::SolidLine,
				Qt::RoundCap,
				Qt::RoundJoin);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			if (_glyph == Glyph::More) {
				p.drawLine(c + QPointF(-8, 0), c + QPointF(8, 0));
				p.drawLine(c + QPointF(0, -8), c + QPointF(0, 8));
			} else {
				p.drawEllipse(c, 9., 9.);
				p.drawLine(c + QPointF(-6.4, 6.4), c + QPointF(6.4, -6.4));
			}
			return;
		}
		if (_glyph == Glyph::Shuffle || _glyph == Glyph::Repeat) {
			const auto &settings = Core::App().settings();
			const auto shuffle = (_glyph == Glyph::Shuffle);
			const auto mode = settings.playerRepeatMode();
			const auto active = shuffle
				? settings.playerOrderMode() == Media::OrderMode::Shuffle
				: mode != Media::RepeatMode::None;
			const auto &icon = shuffle
				? st::mediaPlayerOrderIconShuffle
				: mode == Media::RepeatMode::One
				? st::mediaPlayerRepeatOneIcon
				: st::mediaPlayerRepeatDisabledIcon;
			const auto scale = 22. / std::max(icon.width(), icon.height());
			p.translate(
				c.x() - icon.width() * scale / 2.,
				c.y() - icon.height() * scale / 2.);
			p.scale(scale, scale);
			icon.paint(p, QPoint(), icon.width(), active
				? QColor(st::windowActiveTextFg->c)
				: QColor(st::windowFg->c));
			return;
		}
		if (_glyph == Glyph::Play) {
			p.setBrush(isOver()
				? QColor(st::windowFg->c).darker(110)
				: st::windowFg->c);
			p.drawEllipse(r.adjusted(2, 2, -2, -2));
			p.setBrush(st::windowBg);
			const auto u = r.width() / 100.;
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
	Ui::Animations::Simple _likeAnimation;
	bool _overlay = false;
	Ui::Animations::Simple _hover;
	Ui::Animations::Simple _press;
	Ui::Animations::Simple _pop;

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
		setMouseTracking(true);
		_clock.start();
		_timer.setInterval(16);
		_timer.setTimerType(Qt::PreciseTimer);
		connect(&_timer, &QTimer::timeout, this, [=] { tick(); });
		connect(qApp, &QGuiApplication::applicationStateChanged, this, [=] {
			updateTimer();
		}, Qt::QueuedConnection);
		PowerSaving::Changes() | rpl::on_next([=] {
			updateTimer();
		}, lifetime());
	}

	// Полноэкранный плеер: текст слева, крупнее, без своей подложки.
	void setBig(bool big) {
		_big = big;
		_plasma = false;
		_heights.clear();
		updateTimer();
		update();
	}
	void setPlasma(bool on) {
		_plasma = on;
		updateTimer();
		update();
	}
	void setLoading() {
		reset();
		_status = QString::fromUtf8(Mokagram::Lang::Tr("Ищу текст…"));
		updateTimer();
		update();
	}
	void setLyrics(Lyrics lyrics) {
		reset();
		_lyrics = std::move(lyrics);
		_status = _lyrics.found
			? QString()
			: QString::fromUtf8(Mokagram::Lang::Tr("Текст этой песни не найден"));
		_weights.assign(_lyrics.lines.size(), 0.f);
		updateTimer();
		tick();
		update();
	}
	void setPosition(qint64 ms, bool playing) {
		_position = ms;
		_positionAt = _clock.elapsed();
		_playing = playing;
		updateTimer();
		if (!_timer.isActive()) {
			tick();
		}
	}

	// Нажали на строку синхронного текста: время строки в мс (как в Spotify, перемотка к этой строке).
	[[nodiscard]] rpl::producer<qint64> lineClicked() const {
		return _lineClicked.events();
	}

protected:
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void wheelEvent(QWheelEvent *e) override {
		if (!_lyrics.found || _lyrics.lines.empty()) {
			e->ignore();
			return;
		}
		const auto delta = e->pixelDelta().isNull()
			? -e->angleDelta().y() / 120. * 48.
			: -double(e->pixelDelta().y());
		if (_lyrics.synced) {
			const auto last = int(_lyrics.lines.size()) - 1;
			_scroll = std::clamp(_scroll + delta, centerOf(0), centerOf(last));
			_manualUntil = _clock.elapsed() + 3000;
		} else {
			auto total = 24.;
			for (auto i = 0; i != int(_lyrics.lines.size()); ++i) {
				total += heightOf(i);
			}
			_plainScroll = std::clamp(_plainScroll + delta, 0., std::max(total - height(), 0.));
		}
		e->accept();
		update();
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		setCursor((lineAt(e->pos().y()) >= 0) ? style::cur_pointer : style::cur_default);
	}
	void mousePressEvent(QMouseEvent *e) override {
		if (e->button() != Qt::LeftButton) {
			return;
		}
		if (const auto index = lineAt(e->pos().y()); index >= 0) {
			_lineClicked.fire_copy(_lyrics.lines[index].timeMs);
		}
	}

private:
	// Номер строки под точкой по вертикали (-1 нет), считается так же, как рисуется.
	[[nodiscard]] int lineAt(int y) {
		if (!_lyrics.found || !_lyrics.synced || _lyrics.lines.empty()) {
			return -1;
		}
		auto top = anchorY() - _scroll;
		for (auto i = 0; i != int(_lyrics.lines.size()); ++i) {
			const auto lineHeight = heightOf(i);
			if (y >= top && y < top + lineHeight) {
				return i;
			}
			top += lineHeight;
			if (top > height()) {
				break;
			}
		}
		return -1;
	}
	rpl::event_stream<qint64> _lineClicked;
	void updateTimer();
	void reset() {
		_lyrics = {};
		_current = -1;
		_scroll = 0.;
		_plainScroll = 0.;
		_manualUntil = 0;
		_scrollReady = false;
		_heights.clear();
		_weights.clear();
	}

	// Позиция трека между обновлениями плеера: плавнее подсветка строк.
	[[nodiscard]] qint64 livePosition() const {
		return _position + (_playing
			? std::min<qint64>(_clock.elapsed() - _positionAt, 600)
			: 0);
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
		auto changed = _plasma && CurrentCoverAccent().isValid();
		if (_lyrics.found && _lyrics.synced && !_lyrics.lines.empty()) {
			auto index = -1;
			const auto at = livePosition() + 120;
			for (auto i = 0; i != int(_lyrics.lines.size()); ++i) {
				if (_lyrics.lines[i].timeMs <= at) {
					index = i;
				} else {
					break;
				}
			}
			changed |= (_current != index);
			_current = index;
			auto target = centerOf(std::max(index, 0));
			if (_big) {
				// В начале песни сверху не оставляем пустоты: первая строка не ниже верхней трети.
				target = std::max(target, anchorY() - height() * 0.16);
			}
			if (!_scrollReady) {
				_scroll = target;
				_scrollReady = true;
			}
			// Пружина без рывков: чем дальше строка, тем быстрее догоняем.
			// Пока пользователь листает текст сам (и 3 с после), автопрокрутка к строке не работает.
			if (now >= _manualUntil) {
				changed |= (std::abs(target - _scroll) > 0.1);
				_scroll += (target - _scroll)
					* (_timer.isActive() ? (1. - std::exp(-dt * 6.5)) : 1.);
			}
			const auto fade = _timer.isActive()
				? float(1. - std::exp(-dt * 9.))
				: 1.f;
			for (auto i = 0; i != int(_weights.size()); ++i) {
				const auto goal = (i == _current) ? 1.f : 0.f;
				changed |= (std::abs(goal - _weights[i]) > 0.001f);
				_weights[i] += (goal - _weights[i]) * fade;
			}
		}
		if (changed) {
			update();
		}
	}

	[[nodiscard]] double kScaleMax() const {
		return _big ? 1.16 : 1.12;
	}
	[[nodiscard]] double anchorY() const {
		return height() * (_big ? 0.38 : 0.5);
	}
	[[nodiscard]] int textWidth() const {
		return int((width() - (_big ? 0 : 56)) / kScaleMax());
	}
	[[nodiscard]] QFont lineFont() const {
		auto font = st::semiboldFont->f;
		font.setPixelSize(_big ? style::ConvertScale(30) : 19);
		if (_big) {
			font.setWeight(QFont::ExtraBold);
		}
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
				Qt::TextWordWrap | (_big ? Qt::AlignLeft : Qt::AlignHCenter),
				text).height() + (_big ? style::ConvertScale(22) : 20);
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
		if (!_big) {
			auto path = QPainterPath();
			path.addRoundedRect(QRectF(rect()), 20, 20);
			p.setClipPath(path);
			p.fillRect(rect(), st::windowBgOver);
		}
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
			p.setPen(_big ? QColor(255, 255, 255, 140) : st::windowSubTextFg->c);
			p.setFont(st::normalFont);
			p.drawText(rect(), Qt::AlignCenter, _status);
			return;
		}
		const auto count = int(_lyrics.lines.size());
		auto top = _lyrics.synced ? (anchorY() - _scroll) : (12. - _plainScroll);
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
				const auto cy = top + lineHeight / 2.;
				auto alpha = rest + (1. - rest) * w;
				if (_big) {
					// Края текста растворяются в фоне плавно.
					const auto edge = [](double t) {
						t = std::clamp(t, 0., 1.);
						return t * t * (3. - 2. * t);
					};
					alpha *= edge(cy / (height() * 0.2))
						* edge((height() - cy) / (height() * 0.28));
				}
				color.setAlphaF(alpha);
				p.setPen(color);
				const auto scale = 1. + (kScaleMax() - 1.) * w;
				p.save();
				if (_big) {
					p.translate(0., cy);
				} else {
					p.translate(width() / 2., cy);
				}
				p.scale(scale, scale);
				if (_big) {
					p.drawText(
						QRectF(0., -lineHeight / 2. + style::ConvertScale(11), textWidth(), lineHeight - style::ConvertScale(11)),
						Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
						_lyrics.lines[i].text);
				} else {
				p.drawText(
					QRectF(-textWidth() / 2., -lineHeight / 2. + 10, textWidth(), lineHeight - 10),
					Qt::TextWordWrap | Qt::AlignHCenter | Qt::AlignTop,
					_lyrics.lines[i].text);
				}
				p.restore();
			}
			top += lineHeight;
			if (top > height() + 400) {
				break;
			}
		}
		if (_big) {
			return;
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
	double _plainScroll = 0.;
	qint64 _manualUntil = 0;
	double _phase = 0.;
	int _current = -1;
	bool _scrollReady = false;
	bool _plasma = true;
	bool _playing = false;
	bool _big = false;

};

void LyricsView::updateTimer() {
	const auto animate = isVisible()
		&& QGuiApplication::applicationState() == Qt::ApplicationActive
		&& ((_plasma && CurrentCoverAccent().isValid())
			|| (_playing
				&& _lyrics.found
				&& _lyrics.synced
				&& !_lyrics.lines.empty()))
		&& !PowerSaving::On(PowerSaving::kAnimations);
	if (animate && !_timer.isActive()) {
		_last = _clock.elapsed();
		_timer.start();
	} else if (!animate) {
		_timer.stop();
	}
}

void LyricsView::showEvent(QShowEvent *e) {
	RpWidget::showEvent(e);
	updateTimer();
}

void LyricsView::hideEvent(QHideEvent *e) {
	_timer.stop();
	RpWidget::hideEvent(e);
}

void LyricsView::resizeEvent(QResizeEvent *e) {
	_heights.clear();
	RpWidget::resizeEvent(e);
}

constexpr auto kLyricsPanelWidth = 340;
constexpr auto kLyricsHeaderHeight = 56;

// Колонка справа от чата: название трека и синхронный текст.
class LyricsPanel final : public Ui::RpWidget {
public:
	LyricsPanel(
		not_null<Ui::RpWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<Ui::RpWidget*> anchor)
	: RpWidget(parent.get())
	, _controller(controller)
	, _anchor(anchor)
	, _view(Ui::CreateChild<LyricsView>(this)) {
		_view->setPlasma(Mokagram::Settings::Bool(u"lyrics.plasma"_q));
		_close = Mokagram::Plugins::MakeHeaderButton(this, true, [] {
			Mokagram::Settings::SetBool(u"lyrics.enabled"_q, false);
		});

		// Левый край тянется, как у третьей колонки: ширина общая с ней.
		_resize = Ui::CreateChild<Ui::ResizeArea>(this);
		_resize->addMoveLeftCallback([=](int globalLeft) {
			if (const auto parent = parentWidget()) {
				Core::App().settings().setThirdColumnWidth(
					parent->mapToGlobal(QPoint(parent->width(), 0)).x()
						- globalLeft);
			}
		});
		_resize->addMoveFinishedCallback([=] {
			Core::App().settings().setThirdColumnWidth(std::clamp(
				Core::App().settings().thirdColumnWidth(),
				st::columnMinimalWidthThird,
				st::columnMaximalWidthThird));
			Core::App().saveSettingsDelayed();
		});

		parent->events(
		) | rpl::filter([](not_null<QEvent*> e) {
			return e->type() == QEvent::Resize;
		}) | rpl::on_next([=] {
			updateGeometryFromAnchor();
		}, lifetime());
		anchor->events(
		) | rpl::filter([](not_null<QEvent*> e) {
			return e->type() == QEvent::Resize || e->type() == QEvent::Move;
		}) | rpl::on_next([=] {
			updateGeometryFromAnchor();
		}, lifetime());
		controller->adaptive().changes(
		) | rpl::on_next([=] {
			updateGeometryFromAnchor();
		}, lifetime());
		Core::App().settings().thirdColumnWidthChanges(
		) | rpl::on_next([=](int) {
			updateGeometryFromAnchor();
		}, lifetime());
		anchor->shownValue(
		) | rpl::on_next([=](bool) {
			updateGeometryFromAnchor();
			refresh();
		}, lifetime());
		const auto weak = QPointer<LyricsPanel>(this);
		anchor->lifetime().add([=] {
			if (weak) {
				weak->deleteLater();
			}
		});

		Mokagram::Settings::Changed(
		) | rpl::on_next([=](const QString &key) {
			if (key == u"lyrics.plasma"_q) {
				_view->setPlasma(Mokagram::Settings::Bool(key));
			} else if (key == u"lyrics.enabled"_q
				&& !Mokagram::Settings::Bool(key)) {
				deleteLater();
			}
		}, lifetime());

		const auto instance = Media::Player::instance();
		instance->updatedNotifier(
		) | rpl::on_next([=] {
			refresh();
		}, lifetime());
		QueueState().changes.events(
		) | rpl::on_next([=] {
			refresh();
		}, lifetime());
		CoversChanged() | rpl::on_next([=] {
			_view->update();
		}, lifetime());

		// Нажатие на строку перематывает трек на её время.
		_view->lineClicked(
		) | rpl::on_next([=](qint64 ms) {
			const auto state = instance->getState(AudioMsgId::Type::Song);
			const auto lengthMs = state.length * 1000
				/ std::max<int64>(state.frequency, 1);
			if (lengthMs <= 0) {
				return;
			}
			instance->startSeeking(AudioMsgId::Type::Song);
			instance->finishSeeking(
				AudioMsgId::Type::Song,
				std::clamp(float64(ms) / float64(lengthMs), 0., 1.));
		}, lifetime());

		updateGeometryFromAnchor();
		refresh();
		raise();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		p.fillRect(rect(), st::windowBg);
		p.fillRect(0, 0, st::lineWidth, height(), st::shadowFg);
		const auto left = 18;
		const auto available = width() - left - _close->width() - 12;
		p.setFont(st::semiboldFont);
		p.setPen(st::windowFg);
		p.drawText(
			QRect(left, 8, available, 22),
			Qt::AlignLeft | Qt::AlignVCenter,
			st::semiboldFont->elided(_title, available));
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(left, 30, available, 18),
			Qt::AlignLeft | Qt::AlignVCenter,
			st::normalFont->elided(_artist, available));
	}
	void resizeEvent(QResizeEvent *e) override {
		_view->setGeometry(
			0,
			kLyricsHeaderHeight,
			width(),
			std::max(height() - kLyricsHeaderHeight, 0));
		_close->move(
			width() - _close->width() - 6,
			(kLyricsHeaderHeight - _close->height()) / 2);
		_resize->setGeometry(0, 0, st::historyResizeWidth, height());
		_resize->raise();
		RpWidget::resizeEvent(e);
	}

private:
	// Встаём ровно на место третьей колонки: во всю высоту и её ширины, чат
	// при этом сужается штатно. Если колонку показать нельзя, висим под плеером.
	void updateGeometryFromAnchor() {
		const auto parent = parentWidget();
		if (!parent || !_anchor) {
			return;
		}
		const auto layout = _controller->computeColumnLayout();
		const auto docked = _controller->adaptive().isThreeColumn()
			&& layout.thirdWidth > 0;
		_resize->setVisible(docked);
		if (docked) {
			setGeometry(
				parent->width() - layout.thirdWidth,
				0,
				layout.thirdWidth,
				parent->height());
		} else {
			const auto top = _anchor->mapTo(
				parent,
				QPoint(0, _anchor->height())).y();
			const auto width = std::min(kLyricsPanelWidth, parent->width() / 2);
			setGeometry(
				parent->width() - width,
				top,
				width,
				std::max(parent->height() - top, 0));
		}
		raise();
	}
	void refresh() {
		const auto track = PlayingTrack();
		const auto shown = _anchor
			&& _anchor->isVisible()
			&& !track.id.isEmpty();
		setVisible(shown);
		if (!shown) {
			return;
		}
		if (_title != track.title || _artist != track.artist) {
			_title = track.title;
			_artist = track.artist;
			update();
		}
		const auto instance = Media::Player::instance();
		const auto state = instance->getState(AudioMsgId::Type::Song);
		_view->setPosition(
			state.position * 1000 / std::max<int64>(state.frequency, 1),
			state.state == Media::Player::State::Playing
				|| state.state == Media::Player::State::Starting
				|| state.state == Media::Player::State::Resuming);
		if (_lyricsFor == track.id) {
			return;
		}
		_lyricsFor = track.id;
		_view->setLoading();
		const auto id = track.id;
		FetchLyrics(
			id,
			track.title,
			track.artist,
			track.durationMs,
			crl::guard(this, [=](Lyrics lyrics) {
				if (_lyricsFor == id) {
					_view->setLyrics(std::move(lyrics));
				}
			}));
	}

	const not_null<Window::SessionController*> _controller;
	const QPointer<Ui::RpWidget> _anchor;
	const not_null<LyricsView*> _view;
	Ui::AbstractButton *_close = nullptr;
	Ui::ResizeArea *_resize = nullptr;
	QString _lyricsFor;
	QString _title;
	QString _artist;

};

[[nodiscard]] QPointer<LyricsPanel> &LyricsPanelInstance() {
	static auto result = QPointer<LyricsPanel>();
	return result;
}

// Третью колонку открыли ради панели текста: при закрытии панели закроем и её.
[[nodiscard]] bool &ThirdColumnOpenedForLyrics() {
	static auto result = false;
	return result;
}

// Три цвета для фона полноэкранного плеера: два главных оттенка обложки и их тёмная смесь.
[[nodiscard]] std::array<QColor, 3> CoverPalette(
		const QImage &cover,
		const QColor &accent) {
	constexpr auto kBins = 12;
	double weight[kBins] = {};
	double sumR[kBins] = {}, sumG[kBins] = {}, sumB[kBins] = {};
	if (!cover.isNull()) {
		const auto small = cover.scaled(
			24,
			24,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32);
		for (auto y = 0; y != small.height(); ++y) {
			for (auto x = 0; x != small.width(); ++x) {
				const auto c = QColor(small.pixel(x, y));
				if (c.valueF() < 0.12) {
					continue;
				}
				const auto bin = std::clamp(int(std::max(c.hsvHueF(), 0.f) * kBins), 0, kBins - 1);
				const auto s = c.hsvSaturationF();
				const auto w = (0.05 + s * s) * (0.3 + c.valueF());
				weight[bin] += w;
				sumR[bin] += w * c.redF();
				sumG[bin] += w * c.greenF();
				sumB[bin] += w * c.blueF();
			}
		}
	}
	const auto fixup = [](QColor color, double v) {
		auto h = 0.f, s = 0.f, b = 0.f;
		color.getHsvF(&h, &s, &b);
		return QColor::fromHsvF(
			h < 0 ? 0.f : h,
			s < 0.08f ? s : std::clamp(s, 0.45f, 0.9f),
			float(v));
	};
	const auto binColor = [&](int i) {
		return QColor::fromRgbF(
			sumR[i] / weight[i],
			sumG[i] / weight[i],
			sumB[i] / weight[i]);
	};
	auto best = 0;
	for (auto i = 1; i != kBins; ++i) {
		if (weight[i] > weight[best]) {
			best = i;
		}
	}
	if (weight[best] <= 0.) {
		const auto base = accent.isValid() ? accent : QColor(0x0a, 0x84, 0xff);
		return {
			fixup(base, 0.42),
			fixup(base, 0.62),
			fixup(base, 0.3),
		};
	}
	auto second = -1;
	for (auto i = 0; i != kBins; ++i) {
		const auto distance = std::min((i - best + kBins) % kBins, (best - i + kBins) % kBins);
		if (distance >= 2
			&& weight[i] > weight[best] * 0.12
			&& (second < 0 || weight[i] > weight[second])) {
			second = i;
		}
	}
	const auto first = fixup(binColor(best), 0.55);
	auto other = QColor();
	if (second >= 0) {
		other = fixup(binColor(second), 0.62);
	} else {
		auto h = 0.f, s = 0.f, v = 0.f;
		first.getHsvF(&h, &s, &v);
		other = QColor::fromHsvF(std::fmod(h + 0.08f, 1.f), s, 0.68f);
	}
	return { first, other, fixup(first, 0.28) };
}

constexpr auto kFullscreenSide = 56;

// Полноэкранный плеер: размытый цветной фон, крупный текст песни, тонкая полоса времени.
class FullscreenPlayer final : public Ui::RpWidget {
public:
	FullscreenPlayer(
		not_null<Ui::RpWidget*> parent,
		not_null<Window::SessionController*> controller)
	: RpWidget(parent.get())
	, _controller(controller)
	, _view(Ui::CreateChild<LyricsView>(this))
	, _seek(Ui::CreateChild<SeekBar>(this))
	, _heart(Ui::CreateChild<RoundGlyph>(this, Glyph::Add, 44))
	, _collapse(Ui::CreateChild<RoundGlyph>(this, Glyph::Collapse, 44))
	, _queueToggle(Ui::CreateChild<RoundGlyph>(this, Glyph::Queue, 44))
	, _volume(Ui::CreateChild<SeekBar>(this))
	, _queueScroll(Ui::CreateChild<Ui::ScrollArea>(this, st::defaultScrollArea))
	, _shuffle(Ui::CreateChild<RoundGlyph>(this, Glyph::Shuffle, 44))
	, _previous(Ui::CreateChild<RoundGlyph>(this, Glyph::Previous, 52))
	, _play(Ui::CreateChild<RoundGlyph>(this, Glyph::Play, 68))
	, _next(Ui::CreateChild<RoundGlyph>(this, Glyph::Next, 52))
	, _repeat(Ui::CreateChild<RoundGlyph>(this, Glyph::Repeat, 44)) {
		setFocusPolicy(Qt::StrongFocus);
		_view->setBig(true);
		_seek->setThin(true);
		_volume->setThin(true);
		_volume->setProgress(Core::App().settings().songVolume());
		_volume->moved(
		) | rpl::on_next([=](float64 value) {
			Core::App().settings().setSongVolume(value);
			Core::App().saveSettingsDelayed();
			Media::Player::mixer()->setSongVolume(value);
		}, lifetime());
		Core::App().settings().songVolumeChanges(
		) | rpl::on_next([=](float64 value) {
			_volume->setProgress(value);
			update();
		}, lifetime());
		_queueToggle->setToolTip(QString::fromUtf8(Mokagram::Lang::Tr("Очередь")));
		_queueToggle->setClickedCallback([=] {
			_queueShown = !_queueShown;
			updateQueueVisibility();
		});
		_queueList = _queueScroll->setOwnedWidget(object_ptr<Ui::VerticalLayout>(_queueScroll));
		QueueState().changes.events(
		) | rpl::on_next([=] { rebuildQueue(); }, lifetime());
		for (const auto button : { _heart, _collapse, _queueToggle, _shuffle, _previous, _play, _next, _repeat }) {
			button->setOverlay(true);
		}
		_heart->setToolTip(QString::fromUtf8(Mokagram::Lang::Tr("Нравится")));
		_collapse->setToolTip(QString::fromUtf8(Mokagram::Lang::Tr("Свернуть")));
		_shuffle->setToolTip(QString::fromUtf8(Mokagram::Lang::Tr("Перемешать")));
		_repeat->setToolTip(QString::fromUtf8(Mokagram::Lang::Tr("Повтор")));

		const auto instance = Media::Player::instance();
		_collapse->setClickedCallback([=] { close(); });
		_heart->setClickedCallback([=] {
			const auto &queue = QueueState();
			if (OwnsTrack(instance->current(AudioMsgId::Type::Song))
				&& !queue.current.id.isEmpty()) {
				SetLiked(queue.current, !IsLiked(queue.current.id));
			}
		});
		const auto step = [=](int delta) {
			if (OwnsTrack(instance->current(AudioMsgId::Type::Song))) {
				Step(delta);
			} else if (delta > 0) {
				instance->next(AudioMsgId::Type::Song);
			} else {
				instance->previous(AudioMsgId::Type::Song);
			}
		};
		_previous->setClickedCallback([=] { step(-1); });
		_next->setClickedCallback([=] { step(1); });
		_play->setClickedCallback([=] {
			instance->playPause(AudioMsgId::Type::Song);
		});
		_shuffle->setClickedCallback([=] {
			auto &settings = Core::App().settings();
			settings.setPlayerOrderMode(
				settings.playerOrderMode() == Media::OrderMode::Shuffle
					? Media::OrderMode::Default
					: Media::OrderMode::Shuffle);
			Core::App().saveSettingsDelayed();
		});
		_repeat->setClickedCallback([=] {
			auto &settings = Core::App().settings();
			const auto mode = settings.playerRepeatMode();
			settings.setPlayerRepeatMode(mode == Media::RepeatMode::None
				? Media::RepeatMode::All
				: mode == Media::RepeatMode::All
				? Media::RepeatMode::One
				: Media::RepeatMode::None);
			Core::App().saveSettingsDelayed();
		});
		Core::App().settings().playerOrderModeValue(
		) | rpl::on_next([=] { _shuffle->update(); }, lifetime());
		Core::App().settings().playerRepeatModeValue(
		) | rpl::on_next([=] { _repeat->update(); }, lifetime());
		_seek->sought(
		) | rpl::on_next([=](float64 progress) {
			instance->startSeeking(AudioMsgId::Type::Song);
			instance->finishSeeking(AudioMsgId::Type::Song, progress);
		}, lifetime());
		_view->lineClicked(
		) | rpl::on_next([=](qint64 ms) {
			const auto state = instance->getState(AudioMsgId::Type::Song);
			const auto lengthMs = state.length * 1000
				/ std::max<int64>(state.frequency, 1);
			if (lengthMs <= 0) {
				return;
			}
			instance->startSeeking(AudioMsgId::Type::Song);
			instance->finishSeeking(
				AudioMsgId::Type::Song,
				std::clamp(float64(ms) / float64(lengthMs), 0., 1.));
		}, lifetime());

		parent->events(
		) | rpl::filter([](not_null<QEvent*> e) {
			return e->type() == QEvent::Resize;
		}) | rpl::on_next([=] {
			setGeometry(parentWidget()->rect());
		}, lifetime());
		instance->updatedNotifier(
		) | rpl::on_next([=] { refresh(); }, lifetime());
		QueueState().changes.events(
		) | rpl::on_next([=] { refresh(); }, lifetime());
		CoversChanged() | rpl::on_next([=] { refresh(); }, lifetime());
		LikesChanged() | rpl::on_next([=] { refresh(); }, lifetime());

		setGeometry(parent->rect());
		refresh();
		show();
		raise();
		setFocus();

		// Появление: плавное проявление всего экрана.
		const auto effect = new QGraphicsOpacityEffect(this);
		effect->setOpacity(0.);
		setGraphicsEffect(effect);
		_fade.start([=] {
			const auto value = _fade.value(_closing ? 0. : 1.);
			if (graphicsEffect() == effect) {
				effect->setOpacity(value);
			}
			if (!_fade.animating()) {
				if (_closing) {
					deleteLater();
				} else {
					setGraphicsEffect(nullptr);
				}
			}
		}, 0., 1., 180, anim::easeOutCubic);
	}

	void close() {
		if (_closing) {
			return;
		}
		_closing = true;
		auto effect = new QGraphicsOpacityEffect(this);
		setGraphicsEffect(effect);
		_fade.start([=] {
			effect->setOpacity(_fade.value(0.));
			if (!_fade.animating()) {
				deleteLater();
			}
		}, 1., 0., 180, anim::easeOutCubic);
	}

protected:
	void keyPressEvent(QKeyEvent *e) override {
		if (e->key() == Qt::Key_Escape) {
			close();
		} else {
			RpWidget::keyPressEvent(e);
		}
	}
	void mousePressEvent(QMouseEvent *e) override {
		setFocus();
	}
	struct Layout {
		QRect left;
		QRect right;
		QRect queue;
		bool wide = false;
		int volumeTop = 0;
		QRect cover;
		QRect info;
		int seekTop = 0;
		int controlsTop = 0;
	};
	[[nodiscard]] Layout computeLayout() const {
		const auto s = [](int v) { return style::ConvertScale(v); };
		auto result = Layout();
		const auto margin = s(24);
		const auto gap = s(16);
		const auto cardTop = margin;
		const auto cardHeight = std::max(height() - 2 * margin, 0);
		result.wide = width() >= s(1280);
		const auto leftWidth = std::clamp(int(width() * (result.wide ? 0.32 : 0.42)), s(360), s(780));
		result.left = QRect(margin, cardTop, std::min(leftWidth, width() - 2 * margin), cardHeight);
		auto rightEdge = width() - margin;
		if (result.wide) {
			const auto queueWidth = std::clamp(int(width() * 0.24), s(300), s(400));
			result.queue = QRect(rightEdge - queueWidth, cardTop, queueWidth, cardHeight);
			rightEdge = result.queue.x() - gap;
		}
		result.right = QRect(
			result.left.right() + 1 + gap,
			cardTop,
			std::max(rightEdge - result.left.right() - 1 - gap, 0),
			cardHeight);
		const auto pad = s(24);
		const auto below = s(20) + s(56) + s(14) + _seek->height() + s(18) + s(4) + _play->height() + s(14) + _volume->height();
		const auto side = std::max(
			std::min(result.left.width() - 2 * pad, cardHeight - 2 * pad - below),
			s(120));
		const auto total = side + below;
		const auto top = result.left.y() + std::max((cardHeight - total) / 2, pad / 2);
		const auto x = result.left.x() + (result.left.width() - side) / 2;
		result.cover = QRect(x, top, side, side);
		result.info = QRect(x, top + side + s(20), side, s(56));
		result.seekTop = result.info.bottom() + 1 + s(14);
		result.controlsTop = result.seekTop + _seek->height() + s(18) + s(4);
		result.volumeTop = result.controlsTop + _play->height() + s(14);
		return result;
	}
	[[nodiscard]] QColor accentColor() const {
		const auto mix = _colorAnim.value(1.);
		const auto c = anim::color(_from[1], _to[1], mix);
		auto h = 0.f, sat = 0.f, v = 0.f;
		c.getHsvF(&h, &sat, &v);
		return QColor::fromHsvF(h < 0 ? 0.f : h, std::min(sat, 0.75f), std::max(v, 0.9f));
	}

	void resizeEvent(QResizeEvent *e) override {
		const auto s = [](int v) { return style::ConvertScale(v); };
		const auto layout = computeLayout();
		const auto corner = layout.wide ? layout.queue : layout.right;
		_collapse->move(
			corner.right() + 1 - s(20) - _collapse->width(),
			corner.y() + s(20));
		_queueToggle->move(
			layout.right.right() + 1 - s(20) - _queueToggle->width() * 2 - s(10),
			layout.right.y() + s(20));
		const auto queueArea = layout.wide ? layout.queue : layout.right;
		_queueScroll->setGeometry(
			queueArea.x(),
			queueArea.y() + s(76),
			queueArea.width(),
			std::max(queueArea.height() - s(76) - s(12), 0));
		_queueList->resizeToWidth(_queueScroll->width());
		_volume->setGeometry(
			layout.cover.x() + s(36),
			layout.volumeTop,
			std::max(layout.cover.width() - s(36), 0),
			_volume->height());
		_heart->move(
			layout.info.right() + 1 - _heart->width(),
			layout.info.y() + (layout.info.height() - _heart->height()) / 2);
		_view->setGeometry(
			layout.right.x() + s(40),
			layout.right.y() + s(8),
			std::max(layout.right.width() - s(80), 0),
			std::max(layout.right.height() - s(16), 0));
		_seek->setGeometry(
			layout.cover.x(),
			layout.seekTop,
			layout.cover.width(),
			_seek->height());
		const auto row = { _shuffle, _previous, _play, _next, _repeat };
		auto widths = 0;
		for (const auto button : row) {
			widths += button->width();
		}
		const auto gap = std::clamp((layout.cover.width() - widths) / 4, 0, s(18));
		auto x = layout.cover.x() + (layout.cover.width() - widths - gap * 4) / 2;
		for (const auto button : row) {
			button->move(x, layout.controlsTop + (_play->height() - button->height()) / 2);
			x += button->width() + gap;
		}
		updateQueueVisibility();
		RpWidget::resizeEvent(e);
	}

	// Широкое окно: очередь отдельной карточкой справа. Узкое: кнопка меняет текст на очередь.
	void updateQueueVisibility() {
		const auto layout = computeLayout();
		_queueToggle->setVisible(!layout.wide);
		const auto queueOnly = !layout.wide && _queueShown;
		_queueScroll->setVisible(layout.wide || queueOnly);
		_view->setVisible(!queueOnly);
		_queueScroll->raise();
		_collapse->raise();
		_queueToggle->raise();
		update();
	}
	void rebuildQueue() {
		_queueList->clear();
		const auto &queue = QueueState();
		const auto playingId = PlayingTrack().id;
		const auto inQueue = ranges::contains(queue.tracks, playingId, &Track::id);
		const auto &tracks = inQueue ? queue.tracks : ContextTracks();
		if (!inQueue) {
			// Играет трек из чата: очередь это музыка того же чата.
			const auto current = Media::Player::instance()->current(AudioMsgId::Type::Song);
			const auto peer = current.contextId().peer;
			if (peer && peer != _contextPeer) {
				_contextPeer = peer;
				ContextTracks().clear();
				if (const auto session = QueueState().session.get()) {
					_context = std::make_unique<ChatMusicSearch>(
						session,
						crl::guard(this, [=](std::vector<Track> list, QString) {
							ContextTracks() = std::move(list);
							rebuildQueue();
						}),
						peer);
					_context->search(QString());
				}
			}
		}
		for (auto i = 0; i != int(tracks.size()); ++i) {
			_queueList->add(object_ptr<Row>(
				_queueList,
				_controller,
				tracks[i],
				i,
				inQueue ? RowMode::Queue : RowMode::Context));
		}
		_queueList->resizeToWidth(_queueScroll->width());
		auto at = inQueue ? queue.index : -1;
		for (auto i = 0; !inQueue && i != int(tracks.size()); ++i) {
			if (tracks[i].id == playingId) {
				at = i;
				break;
			}
		}
		if (at >= 0) {
			_queueScroll->scrollToY(std::max((at - 1) * style::ConvertScale(kRowHeight), 0));
		}
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto s = [](int v) { return style::ConvertScale(v); };
		p.fillRect(rect(), QColor(0x1c, 0x1c, 0x1c));
		const auto layout = computeLayout();
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0x28, 0x28, 0x28));
		p.drawRoundedRect(layout.left, s(26), s(26));
		p.drawRoundedRect(layout.right, s(26), s(26));
		if (layout.wide) {
			p.drawRoundedRect(layout.queue, s(26), s(26));
		}
		if (layout.wide || _queueShown) {
			auto headerFont = st::semiboldFont->f;
			headerFont.setPixelSize(s(19));
			p.setFont(headerFont);
			p.setPen(Qt::white);
			const auto area = layout.wide ? layout.queue : layout.right;
			p.drawText(
				QRect(area.x() + s(24), area.y() + s(20), area.width() / 2, s(44)),
				Qt::AlignLeft | Qt::AlignVCenter,
				QString::fromUtf8(Mokagram::Lang::Tr("Очередь")));
		}

		// Громкость: значок слева от полосы.
		{
			const auto volume = Core::App().settings().songVolume();
			const auto &icon = (volume == 0.)
				? st::mediaPlayerVolumeIcon0
				: (volume < 0.66)
				? st::mediaPlayerVolumeIcon1
				: st::mediaPlayerVolumeToggle.icon;
			const auto box = QRect(layout.cover.x(), layout.volumeTop, s(28), _volume->height());
			icon.paintInCenter(p, box, QColor(255, 255, 255, 170));
		}

		// Обложка: сжимается на паузе, при смене трека плавно перетекает в новую.
		const auto playing = _playAnim.value(_isPlaying ? 1. : 0.);
		const auto scale = 0.94 + 0.06 * playing;
		const auto fade = _infoFade.value(1.);
		const auto coverRect = QRectF(layout.cover);
		const auto drawCover = [&](const QPixmap &pixmap, double opacity) {
			if (opacity <= 0.) {
				return;
			}
			p.save();
			p.setOpacity(opacity);
			p.translate(coverRect.center());
			p.scale(scale, scale);
			p.translate(-coverRect.center());
			auto path = QPainterPath();
			path.addRoundedRect(coverRect, s(20), s(20));
			p.setClipPath(path);
			if (pixmap.isNull()) {
				p.fillRect(coverRect, QColor(0x32, 0x32, 0x32));
			} else {
				p.drawPixmap(layout.cover, pixmap);
			}
			p.restore();
		};
		drawCover(_coverWas, 1. - fade);
		drawCover(_cover, fade);

		// Название и исполнитель.
		const auto available = layout.info.width() - (_heart->isHidden() ? 0 : _heart->width() + s(12));
		const auto drawInfo = [&](const QString &title, const QString &artist, double opacity) {
			if (opacity <= 0.) {
				return;
			}
			auto titleFont = st::semiboldFont->f;
			titleFont.setPixelSize(s(21));
			p.setFont(titleFont);
			p.setPen(QColor(255, 255, 255, int(255 * opacity)));
			p.drawText(
				QRect(layout.info.x(), layout.info.y(), available, s(30)),
				Qt::AlignLeft | Qt::AlignVCenter,
				QFontMetrics(titleFont).elidedText(title, Qt::ElideRight, available));
			auto artistFont = st::normalFont->f;
			artistFont.setPixelSize(s(15));
			p.setFont(artistFont);
			p.setPen(QColor(255, 255, 255, int(150 * opacity)));
			p.drawText(
				QRect(layout.info.x(), layout.info.y() + s(32), available, s(22)),
				Qt::AlignLeft | Qt::AlignVCenter,
				QFontMetrics(artistFont).elidedText(artist, Qt::ElideRight, available));
		};
		drawInfo(_wasTitle, _wasArtist, 1. - fade);
		drawInfo(_title, _artist, fade);

		// Время под полосой.
		auto timeFont = st::normalFont->f;
		timeFont.setPixelSize(s(13));
		p.setFont(timeFont);
		p.setPen(QColor(255, 255, 255, 140));
		const auto timesTop = _seek->y() + _seek->height() - s(4);
		p.drawText(
			QRect(layout.cover.x(), timesTop, layout.cover.width() / 2, s(18)),
			Qt::AlignLeft | Qt::AlignVCenter,
			_timeNow);
		p.drawText(
			QRect(layout.cover.center().x(), timesTop, layout.cover.width() / 2, s(18)),
			Qt::AlignRight | Qt::AlignVCenter,
			_timeTotal);
	}

private:
	void refresh() {
		const auto track = PlayingTrack();
		if (track.id.isEmpty()) {
			// Между треками плеер на миг пуст: экран остаётся, закрывает его пользователь.
			return;
		}
		const auto instance = Media::Player::instance();
		const auto current = instance->current(AudioMsgId::Type::Song);
		const auto state = instance->getState(AudioMsgId::Type::Song);
		const auto frequency = std::max<int64>(state.frequency, 1);

		// Обложка могла догрузиться позже трека.
		const auto cover = CoverForPlayback(current);
		const auto coverKey = cover.cacheKey();
		const auto changed = (_title != track.title)
			|| (_artist != track.artist)
			|| (_coverKey != coverKey);
		if (changed) {
			const auto first = _title.isEmpty() && _artist.isEmpty() && _cover.isNull();
			if (!first) {
				_wasTitle = _title;
				_wasArtist = _artist;
				_coverWas = _cover;
				_infoFade.start([=] { update(); }, 0., 1., 320, anim::easeOutCubic);
			}
			_title = track.title;
			_artist = track.artist;
			_coverKey = coverKey;
			const auto size = 512;
			_cover = cover.isNull()
				? QPixmap()
				: QPixmap::fromImage(cover.scaled(
					size,
					size,
					Qt::KeepAspectRatioByExpanding,
					Qt::SmoothTransformation));
			if (!_cover.isNull()) {
			}
			const auto palette = CoverPalette(cover, CurrentCoverAccent());
			const auto mix = _colorAnim.value(1.);
			for (auto i = 0; i != 3; ++i) {
				_from[i] = _ready ? anim::color(_from[i], _to[i], mix) : palette[i];
				_to[i] = palette[i];
			}
			_ready = true;
			_colorAnim.start([=] { _seek->setFillColor(accentColor()); update(); }, 0., 1., 1200, anim::easeOutCubic);
			_seek->setFillColor(accentColor());
			update();
		}
		const auto playing = state.state == Media::Player::State::Playing
			|| state.state == Media::Player::State::Starting
			|| state.state == Media::Player::State::Resuming;
		if (_isPlaying != playing) {
			_isPlaying = playing;
			_playAnim.start([=] { update(); }, playing ? 0. : 1., playing ? 1. : 0., 180, anim::easeOutCubic);
		}
		_play->setPlaying(playing);
		const auto own = OwnsTrack(current);
		_heart->setVisible(own);
		_heart->setLiked(own && IsLiked(track.id));
		_seek->setProgress(state.length > 0
			? state.position / float64(state.length)
			: 0.);
		const auto fmt = [](int64 seconds) {
			return u"%1:%2"_q.arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
		};
		const auto now = fmt(state.position / frequency);
		const auto total = fmt(state.length / frequency);
		if (_timeNow != now || _timeTotal != total) {
			_timeNow = now;
			_timeTotal = total;
			update();
		}
		_view->setPosition(state.position * 1000 / frequency, playing);
		if (_queueFor != track.id) {
			_queueFor = track.id;
			rebuildQueue();
		}
		if (_lyricsFor != track.id) {
			_lyricsFor = track.id;
			_view->setLoading();
			const auto id = track.id;
			FetchLyrics(
				id,
				track.title,
				track.artist,
				track.durationMs,
				crl::guard(this, [=](Lyrics lyrics) {
					if (_lyricsFor == id) {
						_view->setLyrics(std::move(lyrics));
					}
				}));
		}
	}

	const not_null<Window::SessionController*> _controller;
	const not_null<LyricsView*> _view;
	const not_null<SeekBar*> _seek;
	const not_null<RoundGlyph*> _heart;
	const not_null<RoundGlyph*> _collapse;
	const not_null<RoundGlyph*> _queueToggle;
	const not_null<SeekBar*> _volume;
	const not_null<Ui::ScrollArea*> _queueScroll;
	Ui::VerticalLayout *_queueList = nullptr;
	std::unique_ptr<ChatMusicSearch> _context;
	PeerId _contextPeer;
	QString _queueFor;
	bool _queueShown = false;
	const not_null<RoundGlyph*> _shuffle;
	const not_null<RoundGlyph*> _previous;
	const not_null<RoundGlyph*> _play;
	const not_null<RoundGlyph*> _next;
	const not_null<RoundGlyph*> _repeat;

	Ui::Animations::Simple _fade;
	Ui::Animations::Simple _colorAnim;
	Ui::Animations::Simple _infoFade;
	Ui::Animations::Simple _playAnim;
	std::array<QColor, 3> _from;
	std::array<QColor, 3> _to;
	QPixmap _cover;
	QPixmap _coverWas;
	qint64 _coverKey = 0;
	QString _title;
	QString _artist;
	QString _wasTitle;
	QString _wasArtist;
	QString _timeNow = u"0:00"_q;
	QString _timeTotal = u"0:00"_q;
	QString _lyricsFor;
	bool _isPlaying = true;
	bool _closing = false;
	bool _ready = false;

};

[[nodiscard]] QPointer<FullscreenPlayer> &FullscreenInstance() {
	static auto result = QPointer<FullscreenPlayer>();
	return result;
}

// Есть ли текст у играющего трека (для состояния кнопки в панели плеера).
struct LyricsAvailability {
	QString id;
	bool found = false;
	bool started = false;
	rpl::event_stream<> changes;
	rpl::lifetime lifetime;
};

[[nodiscard]] LyricsAvailability &Availability() {
	static auto result = new LyricsAvailability();
	return *result;
}

void RefreshAvailability() {
	auto &state = Availability();
	const auto track = PlayingTrack();
	if (state.id == track.id) {
		return;
	}
	state.id = track.id;
	state.found = false;
	state.changes.fire({});
	if (track.id.isEmpty()) {
		return;
	}
	const auto id = track.id;
	FetchLyrics(
		id,
		track.title,
		track.artist,
		track.durationMs,
		[=](Lyrics lyrics) {
			auto &state = Availability();
			if (state.id == id && state.found != lyrics.found) {
				state.found = lyrics.found;
				state.changes.fire({});
			}
		});
}

} // namespace

void SyncLyricsPanel(
		not_null<Window::SessionController*> controller,
		not_null<Ui::RpWidget*> anchor) {
	auto &panel = LyricsPanelInstance();
	if (!Mokagram::Settings::Bool(u"lyrics.enabled"_q)) {
		if (panel) {
			panel->deleteLater();
			panel = nullptr;
		}
		if (base::take(ThirdColumnOpenedForLyrics())) {
			controller->closeThirdSection();
		}
		return;
	}
	const auto parent = not_null<Ui::RpWidget*>(controller->content());
	if (panel && panel->parentWidget() != parent.get()) {
		panel->deleteLater();
		panel = nullptr;
	}
	if (!panel) {
		panel = Ui::CreateChild<LyricsPanel>(parent.get(), controller, anchor);
	}
}

bool LyricsAvailable() {
	return Availability().found;
}

rpl::producer<> LyricsAvailabilityChanges() {
	auto &state = Availability();
	if (!state.started) {
		state.started = true;
		Media::Player::instance()->updatedNotifier(
		) | rpl::on_next(RefreshAvailability, state.lifetime);
		QueueState().changes.events(
		) | rpl::on_next(RefreshAvailability, state.lifetime);
		RefreshAvailability();
	}
	return state.changes.events();
}

void ToggleLyricsPanel(
		not_null<Window::SessionController*> controller,
		not_null<Ui::RpWidget*> anchor) {
	// Панель стоит на месте третьей колонки: если её нет, открываем (как кнопка
	// «Информация» в шапке чата) и при закрытии панели закроем обратно.
	if (!Mokagram::Settings::Bool(u"lyrics.enabled"_q)
		&& !controller->adaptive().isThreeColumn()
		&& controller->canShowThirdSection()) {
		Core::App().settings().setThirdSectionInfoEnabled(true);
		Core::App().saveSettingsDelayed();
		controller->resizeForThirdSection();
		controller->updateColumnLayout();
		ThirdColumnOpenedForLyrics() = true;
	}
	Mokagram::Settings::SetBool(
		u"lyrics.enabled"_q,
		!Mokagram::Settings::Bool(u"lyrics.enabled"_q));
	SyncLyricsPanel(controller, anchor);
}

void ShowFullscreenPlayer(not_null<Window::SessionController*> controller) {
	BindController(controller);
	auto &instance = FullscreenInstance();
	if (instance) {
		return;
	}
	if (PlayingTrack().id.isEmpty()) {
		return;
	}
	instance = Ui::CreateChild<FullscreenPlayer>(
		not_null<Ui::RpWidget*>(controller->widget()).get(),
		controller);
}

void DebugGrabFullscreen(const QString &path) {
	if (const auto widget = FullscreenInstance().data()) {
		widget->grab().save(path);
	}
}

QImage CoverForPlayback(const AudioMsgId &current) {
	const auto document = current.audio();
	if (!document) {
		return {};
	} else if (OwnsTrack(current)) {
		return CoverForDocument(document->id);
	}
	if (const auto media = document->activeMediaView()) {
		if (const auto image = media->thumbnail()) {
			return image->original();
		} else if (const auto image = media->thumbnailInline()) {
			return image->original();
		}
	}
	return {};
}

QColor CurrentCoverAccent() {
	if (!Mokagram::Settings::Bool(u"player.cover_colors"_q)) {
		return QColor();
	}
	const auto current = Media::Player::instance()->current(AudioMsgId::Type::Song);
	if (!current.audio()) {
		return QColor();
	}
	static auto cachedKey = qint64();
	static auto cached = QColor();
	const auto cover = CoverForPlayback(current);
	if (cover.isNull()) {
		return QColor();
	}
	if (cachedKey != cover.cacheKey() || !cached.isValid()) {
		cachedKey = cover.cacheKey();
		cached = ComputeAccent(cover);
	}
	return cached;
}

bool OwnsTrack(const AudioMsgId &id) {
	return id.audio()
		&& (id.audio()->id & 0xFFFFFF0000000000ULL) == 0x4D4F4B0000000000ULL;
}

bool UsesQueue(const AudioMsgId &id) {
	const auto &queue = QueueState();
	if (queue.currentId.isEmpty() || !id.audio()
		|| &id.audio()->session() != queue.session.get()) {
		return false;
	}
	if (OwnsTrack(id)) {
		return !queue.currentId.startsWith(u"telegram:"_q)
			&& id.audio()->id == DocumentIdForTrack(queue.currentId);
	}
	const auto i = queue.messages.find(queue.currentId);
	return i != queue.messages.end()
		&& id.contextId() == i->second;
}

bool NextAvailable() {
	const auto &queue = QueueState();
	const auto &settings = Core::App().settings();
	return !queue.tracks.empty()
		&& (queue.index + 1 < int(queue.tracks.size())
			|| (queue.stream && StreamEnabled())
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

[[nodiscard]] bool FeedbackAllowed() {
	const auto &queue = QueueState();
	return !queue.current.id.isEmpty()
		&& !IsTelegramTrack(queue.current)
		&& OwnsTrack(Media::Player::instance()->current(AudioMsgId::Type::Song));
}

// «Больше такого»: усиливает вкус и пересобирает предстоящие авто-треки.
void MoreLikeThis() {
	if (!FeedbackAllowed()) {
		return;
	}
	auto &queue = QueueState();
	BoostTrack(queue.current);
	if (queue.stream && StreamEnabled()) {
		DropUpcoming([](const Track &) { return true; }, queue.index + 1, true);
		++queue.streamEpoch;
		queue.refilling = false;
		RefillStream();
	}
	if (const auto controller = queue.controller.get()) {
		controller->showToast(QString::fromUtf8(Mokagram::Lang::Tr("Учту: больше такого")));
	}
}

// «Не нравится автор»: в чёрный список, из очереди убираем, переключаем дальше.
void DislikeArtist() {
	if (!FeedbackAllowed()) {
		return;
	}
	auto &queue = QueueState();
	const auto track = queue.current;
	BlockArtist(track);
	DropUpcoming([=](const Track &t) {
		return IsArtistBlocked(t);
	}, queue.index, false);
	if (const auto controller = queue.controller.get()) {
		controller->showToast(QString::fromUtf8(Mokagram::Lang::Tr("Автор скрыт: ")) + track.artist);
	}
	if (NextAvailable()) {
		Step(1);
	}
}

void Step(int delta) {
	auto &queue = QueueState();
	const auto count = int(queue.tracks.size());
	if (!count) {
		return;
	}
	auto target = queue.index + delta;
	if (delta > 0 && queue.stream && StreamEnabled()) {
		if (target >= count) {
			queue.waiting = true;
			RefillStream();
			return;
		}
	} else if (delta > 0
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
	const auto current = Media::Player::instance()->current(AudioMsgId::Type::Song);
	if (!OwnsTrack(current)) {
		return false;
	}
	const auto track = PlayingTrack();
	title = track.title;
	artist = track.artist;
	return true;
}

void ShowCurrentTrackMenu(
		not_null<Window::SessionController*> controller,
		QPoint globalPosition) {
	BindController(controller);
	const auto track = PlayingTrack();
	if (track.id.isEmpty()) {
		return;
	}
	QueueState().controller = base::make_weak(controller);
	static auto menu = base::unique_qptr<Ui::PopupMenu>();
	QWidget *parent = controller->widget().get();
	menu = base::make_unique_q<Ui::PopupMenu>(
		parent,
		st::popupMenuWithIcons);
	const auto add = [&](const QString &text, Fn<void()> callback, const style::icon *icon) {
		menu->addAction(text, std::move(callback), icon);
	};
	const auto instance = Media::Player::instance();
	add(QString::fromUtf8(Mokagram::Lang::Tr("Пауза / играть")), [=] {
		instance->playPause(AudioMsgId::Type::Song);
	}, &st::menuIconSoundOn);
	if (UsesQueue(instance->current(AudioMsgId::Type::Song))
		&& PreviousAvailable()) {
		add(QString::fromUtf8(Mokagram::Lang::Tr("Предыдущий")), [] { Step(-1); }, &st::menuIconAbove);
	}
	if (UsesQueue(instance->current(AudioMsgId::Type::Song))
		&& NextAvailable()) {
		add(QString::fromUtf8(Mokagram::Lang::Tr("Следующий")), [] { Step(1); }, &st::menuIconBelow);
	}
	menu->addSeparator();
	if (!IsTelegramTrack(track)) {
		add(IsLiked(track.id)
			? QString::fromUtf8(Mokagram::Lang::Tr("Убрать из избранного"))
			: QString::fromUtf8(Mokagram::Lang::Tr("В избранное")), [=] {
			SetLiked(track, !IsLiked(track.id));
		}, &st::menuIconFave);
		add(QString::fromUtf8(Mokagram::Lang::Tr("Скачать")), [=] { SaveToDisk(track); }, &st::menuIconDownload);
	}
	if (CanForwardTrack(track)) {
		add(QString::fromUtf8(Mokagram::Lang::Tr("Переслать")), [=] {
			ForwardTrack(controller, track);
		}, &st::menuIconForward);
	}
	if (TrackMessage(track)) {
		add(QString::fromUtf8(Mokagram::Lang::Tr("Показать в чате")), [=] {
			if (const auto message = TrackMessage(track)) {
				controller->showPeerHistory(message->history(), {}, message->id);
			}
		}, &st::menuIconChats);
	} else if (!IsTelegramTrack(track)) {
		add(QString::fromUtf8(Mokagram::Lang::Tr("Добавить в профиль")), [=] { AddToProfile(controller, track); }, &st::menuIconProfile);
		add(QString::fromUtf8(Mokagram::Lang::Tr("Копировать ссылку")), [=] {
			QGuiApplication::clipboard()->setText(track.permalink);
			controller->showToast(QString::fromUtf8(Mokagram::Lang::Tr("Ссылка скопирована")));
		}, &st::menuIconLink);
	}
	menu->addSeparator();
	add(QString::fromUtf8(Mokagram::Lang::Tr("Поиск музыки")), [=] { ShowMusicBox(controller); }, &st::menuIconSearch);
	add(QString::fromUtf8(Mokagram::Lang::Tr("Очередь")), [=] { ShowPlayerBox(controller); }, &st::menuIconChats);
	menu->popup(globalPosition);
}

void ShowPlayerBox(not_null<Window::SessionController*> controller) {
	BindController(controller);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		Mokagram::Plugins::RoundedWindow(box);
		const auto content = box->verticalLayout();
		const auto instance = Media::Player::instance();
		content->events(
		) | rpl::filter([](not_null<QEvent*> e) {
			return e->type() == QEvent::ContextMenu;
		}) | rpl::on_next([=](not_null<QEvent*> e) {
			e->accept();
			ShowCurrentTrackMenu(controller, QCursor::pos());
		}, box->lifetime());

		// Смена трека: старая обложка с названием плавно растворяется в новой.
		struct InfoFade {
			Ui::Animations::Simple animation;
			QPixmap was;
			QPixmap last;
			QColor tintFrom;
			QColor tintTo;
			Ui::Animations::Simple tintAnimation;
		};
		const auto fade = box->lifetime().make_state<InfoFade>();

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
			auto top = fade->tintAnimation.animating()
				? anim::color(
					fade->tintFrom,
					fade->tintTo,
					fade->tintAnimation.value(1.))
				: accent;
			top.setAlpha(80);
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
			QString::fromUtf8(Mokagram::Lang::Tr("Сейчас играет")),
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
		const auto coverRect = [=] {
			const auto side = std::min(info->width() - 48, 300);
			return QRect((info->width() - side) / 2, 8, side, side);
		};
		info->widthValue(
		) | rpl::on_next([=](int width) {
			info->resize(width, coverRect().height()
				+ st::mokagramMusicPlayerInfoTextHeight + 12);
		}, info->lifetime());
		const auto paintInfo = [=](QPainter &p) {
			auto hq = PainterHighQualityEnabler(p);
			const auto art = coverRect();
			const auto current = instance->current(AudioMsgId::Type::Song);
			const auto cover = CoverForPlayback(current);
			auto path = QPainterPath();
			path.addRoundedRect(art, 8, 8);
			if (!cover.isNull()) {
				p.save();
				p.setClipPath(path);
				const auto crop = std::min(cover.width(), cover.height());
				p.drawImage(art, cover, QRect(
					(cover.width() - crop) / 2,
					(cover.height() - crop) / 2,
					crop,
					crop));
				p.restore();
			} else {
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowBgOver);
				p.drawPath(path);
				st::menuIconSoundOn.paintInCenter(p, art);
			}
			const auto track = PlayingTrack();
			auto big = st::semiboldFont->f;
			big.setPixelSize(20);
			p.setFont(big);
			p.setPen(st::windowFg);
			const auto textWidth = info->width() - 48 - 48
				- (OwnsTrack(current) ? 120 : 0);
			const auto metrics = QFontMetrics(big);
			p.drawText(
				QRect(24, art.bottom() + 28, textWidth, 30),
				Qt::AlignLeft | Qt::AlignVCenter,
				metrics.elidedText(track.title, Qt::ElideRight, textWidth));
		};
		info->paintOn([=](QPainter &p) {
			if (fade->animation.animating() && !fade->was.isNull()) {
				p.drawPixmap(0, 0, fade->was);
				p.setOpacity(fade->animation.value(1.));
			}
			paintInfo(p);
		});
		const auto renderInfo = [=] {
			const auto ratio = style::DevicePixelRatio();
			auto result = QPixmap(info->size() * ratio);
			result.setDevicePixelRatio(ratio);
			result.fill(Qt::transparent);
			auto p = QPainter(&result);
			paintInfo(p);
			p.end();
			return result;
		};

		const auto artistButton = Ui::CreateChild<Ui::AbstractButton>(info);
		const auto artistHint = QString::fromUtf8(
			Mokagram::Lang::Tr("Найти исполнителя на SoundCloud"));
		artistButton->setToolTip(artistHint);
		artistButton->setAccessibleName(artistHint);
		artistButton->paintOn([=](QPainter &p) {
			auto font = st::normalFont->f;
			font.setPixelSize(16);
			const auto active = artistButton->isOver() || artistButton->isDown();
			font.setUnderline(active);
			p.setFont(font);
			p.setPen(active
				? st::windowFg
				: st::windowSubTextFg);
			p.drawText(
				artistButton->rect(),
				Qt::AlignLeft | Qt::AlignVCenter,
				QFontMetrics(font).elidedText(
					PlayingTrack().artist,
					Qt::ElideRight,
					artistButton->width()));
		});
		const auto updateArtistButton = [=] {
			const auto current = instance->current(AudioMsgId::Type::Song);
			const auto textWidth = info->width() - 48 - 48
				- (OwnsTrack(current) ? 120 : 0);
			auto font = st::normalFont->f;
			font.setPixelSize(16);
			const auto artistWidth = std::min(
				textWidth,
				QFontMetrics(font).horizontalAdvance(PlayingTrack().artist) + 2);
			artistButton->setGeometry(
				24,
				coverRect().bottom() + 56,
				artistWidth,
				28);
			const auto hasArtist = !PlayingTrack().artist.trimmed().isEmpty();
			artistButton->setDisabled(!hasArtist);
			artistButton->setPointerCursor(hasArtist);
			artistButton->update();
		};
		info->widthValue(
		) | rpl::on_next(updateArtistButton, artistButton->lifetime());
		artistButton->setClickedCallback([=] {
			const auto artist = PlayingTrack().artist.trimmed();
			if (artist.isEmpty()) {
				return;
			}
			QueueState().telegramSearch = false;
			QueueState().tab = 0;
			box->closeBox();
			ShowMusicBox(controller, artist);
		});

		const auto heart = Ui::CreateChild<RoundGlyph>(info, Glyph::Add, 40);
		info->widthValue(
		) | rpl::on_next([=](int width) {
			heart->move(width - 24 - 40 - 40, coverRect().bottom() + 34);
		}, heart->lifetime());
		const auto more = Ui::CreateChild<RoundGlyph>(info, Glyph::More, 40);
		const auto block = Ui::CreateChild<RoundGlyph>(info, Glyph::Block, 40);
		more->setToolTip(QString::fromUtf8(Mokagram::Lang::Tr("Больше такого")));
		block->setToolTip(QString::fromUtf8(Mokagram::Lang::Tr("Не нравится автор")));
		info->widthValue(
		) | rpl::on_next([=](int width) {
			more->move(width - 24 - 40 - 40 - 40, coverRect().bottom() + 34);
			block->move(width - 24 - 40 - 40 - 80, coverRect().bottom() + 34);
		}, more->lifetime());
		more->setClickedCallback([=] { MoreLikeThis(); });
		block->setClickedCallback([=] { DislikeArtist(); });

		// Перемотка и время.
		const auto seek = content->add(
			object_ptr<SeekBar>(content),
			QMargins(16, 8, 16, 0));
		const auto times = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(24, 0, 24, 0));
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
			QMargins(16, 12, 16, 12));
		controls->resize(controls->width(), 80);
		const auto previous = Ui::CreateChild<RoundGlyph>(controls, Glyph::Previous, 44);
		const auto play = Ui::CreateChild<RoundGlyph>(controls, Glyph::Play, 72);
		const auto next = Ui::CreateChild<RoundGlyph>(controls, Glyph::Next, 44);
		const auto shuffle = Ui::CreateChild<RoundGlyph>(controls, Glyph::Shuffle, 44);
		const auto repeat = Ui::CreateChild<RoundGlyph>(controls, Glyph::Repeat, 44);
		previous->setToolTip(QString::fromUtf8(Mokagram::Lang::Tr("Предыдущий")));
		play->setToolTip(QString::fromUtf8(Mokagram::Lang::Tr("Пауза / играть")));
		next->setToolTip(QString::fromUtf8(Mokagram::Lang::Tr("Следующий")));
		shuffle->setToolTip(QString::fromUtf8(Mokagram::Lang::Tr("Перемешать")));
		repeat->setToolTip(QString::fromUtf8(Mokagram::Lang::Tr("Повтор")));
		controls->widthValue(
		) | rpl::on_next([=](int width) {
			const auto center = width / 2;
			const auto step = (width - 44) / 4;
			play->move(center - 36, 4);
			previous->move(center - step - 22, 18);
			next->move(center + step - 22, 18);
			shuffle->move(0, 18);
			repeat->move(width - 44, 18);
		}, controls->lifetime());
		shuffle->setClickedCallback([=] {
			auto &settings = Core::App().settings();
			settings.setPlayerOrderMode(
				settings.playerOrderMode() == Media::OrderMode::Shuffle
					? Media::OrderMode::Default
					: Media::OrderMode::Shuffle);
			Core::App().saveSettingsDelayed();
		});
		repeat->setClickedCallback([=] {
			auto &settings = Core::App().settings();
			const auto mode = settings.playerRepeatMode();
			settings.setPlayerRepeatMode(mode == Media::RepeatMode::None
				? Media::RepeatMode::All
				: mode == Media::RepeatMode::All
				? Media::RepeatMode::One
				: Media::RepeatMode::None);
			Core::App().saveSettingsDelayed();
		});
		Core::App().settings().playerOrderModeValue(
		) | rpl::on_next([=] { shuffle->update(); }, shuffle->lifetime());
		Core::App().settings().playerRepeatModeValue(
		) | rpl::on_next([=] { repeat->update(); }, repeat->lifetime());

		const auto share = Ui::CreateChild<Ui::AbstractButton>(info);
		share->resize(40, 40);
		share->setPointerCursor(true);
		const auto shareHint = QString::fromUtf8(Mokagram::Lang::Tr("Переслать"));
		share->setToolTip(shareHint);
		share->setAccessibleName(shareHint);
		share->paintOn([=](QPainter &p) {
			auto hq = PainterHighQualityEnabler(p);
			if (share->isOver() || share->isDown()) {
				p.setPen(Qt::NoPen);
				p.setBrush(st::windowBgOver);
				p.drawEllipse(share->rect());
			}
			st::menuIconForward.paintInCenter(p, share->rect());
		});
		info->widthValue(
		) | rpl::on_next([=](int width) {
			share->move(width - 24 - share->width(), coverRect().bottom() + 34);
		}, share->lifetime());
		share->setClickedCallback([=] {
			const auto track = PlayingTrack();
			if (!track.id.isEmpty() && CanForwardTrack(track)) {
				ForwardTrack(controller, track);
			}
		});

		const auto step = [=](int delta) {
			if (OwnsTrack(instance->current(AudioMsgId::Type::Song))) {
				Step(delta);
			} else if (delta > 0) {
				instance->next(AudioMsgId::Type::Song);
			} else {
				instance->previous(AudioMsgId::Type::Song);
			}
		};
		heart->setClickedCallback([=] {
			const auto &queue = QueueState();
			if (OwnsTrack(instance->current(AudioMsgId::Type::Song))
				&& !queue.current.id.isEmpty()) {
				SetLiked(queue.current, !IsLiked(queue.current.id));
				heart->animateLike(IsLiked(queue.current.id));
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

		struct State {
			AudioMsgId current;
			std::shared_ptr<Data::DocumentMedia> media;
			rpl::lifetime downloadLifetime;
			qint64 coverKey = 0;
		};
		const auto playback = box->lifetime().make_state<State>();
		const auto refresh = [=] {
			const auto current = instance->current(AudioMsgId::Type::Song);
			const auto trackChanged = playback->current != current;
			if (trackChanged) {
				playback->current = current;
				playback->downloadLifetime.destroy();
				playback->media = nullptr;
				const auto document = current.audio();
				if (document && !OwnsTrack(current)) {
					playback->media = document->createMediaView();
					document->session().downloaderTaskFinished(
					) | rpl::on_next([=] {
						info->update();
						tint->update();
					}, playback->downloadLifetime);
					playback->media->thumbnailWanted(current.contextId());
				}
			}
			const auto state = instance->getState(AudioMsgId::Type::Song);
			const auto frequency = std::max<int64>(state.frequency, 1);
			const auto length = state.length;
			seek->setProgress(length > 0 ? state.position / float64(length) : 0.);
			const auto fmt = [](int64 seconds) {
				return u"%1:%2"_q.arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
			};
			const auto text = std::pair{
				fmt(state.position / frequency),
				fmt(length / frequency),
			};
			if (*position != text) {
				*position = text;
				times->update();
			}
			const auto coverKey = CoverForPlayback(current).cacheKey();
			if (trackChanged || playback->coverKey != coverKey) {
				playback->coverKey = coverKey;
				if (!fade->last.isNull()
					&& !PowerSaving::On(PowerSaving::kAnimations)) {
					fade->was = fade->last;
					fade->animation.start(
						[=] { info->update(); },
						0.,
						1.,
						kCoverFadeDuration,
						anim::easeOutCubic);
					const auto accent = CurrentCoverAccent();
					if (accent.isValid() && fade->tintTo.isValid()) {
						fade->tintFrom = fade->tintTo;
						fade->tintTo = accent;
						fade->tintAnimation.start(
							[=] { tint->update(); },
							0.,
							1.,
							kCoverFadeDuration,
							anim::easeOutCubic);
					}
				}
				if (CurrentCoverAccent().isValid()) {
					fade->tintTo = CurrentCoverAccent();
				}
				fade->last = renderInfo();
				info->update();
				tint->update();
			}
			const auto own = OwnsTrack(current);
			updateArtistButton();
			const auto track = PlayingTrack();
			share->setVisible(!track.id.isEmpty());
			share->setDisabled(!CanForwardTrack(track));
			heart->setVisible(own);
			more->setVisible(own);
			block->setVisible(own);
			heart->setLiked(own && IsLiked(PlayingTrack().id));
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

		// Очередь: живой список, меняется при добавлении, перемещении и смене трека.
		Ui::AddSubsectionTitle(content, rpl::single(QString::fromUtf8(Mokagram::Lang::Tr("Очередь"))));
		const auto queueList = content->add(object_ptr<Ui::VerticalLayout>(content));
		const auto empty = content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				QString::fromUtf8(Mokagram::Lang::Tr("Очередь пуста. Найдите музыку и выберите «Играть следующим» или «В конец очереди» в меню трека.")),
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
	BindController(controller);
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
			QString::fromUtf8(Mokagram::Lang::Tr("Музыка")),
			st::boxTitle);
		const auto queueButton = Ui::CreateChild<Ui::RoundButton>(
			header,
			rpl::single(QString::fromUtf8(Mokagram::Lang::Tr("Очередь"))),
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
			result.placeholderScale = 0.;
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
			rpl::single(QString::fromUtf8(Mokagram::Lang::Tr("Поиск на SoundCloud"))));
		searchWrap->widthValue(
		) | rpl::on_next([=](int width) {
			input->setGeometry(18, 0, width - 36, 44);
		}, searchWrap->lifetime());
		const auto sources = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(16, 0, 16, 0));
		sources->resize(sources->width(), 48);
		const auto sourceSwitch = Ui::CreateChild<SourceSwitch>(
			sources,
			u"SoundCloud"_q,
			QString::fromUtf8(Mokagram::Lang::Tr("Из чатов")));
		sources->widthValue(
		) | rpl::on_next([=](int width) {
			sourceSwitch->setGeometry(0, 4, width, 40);
		}, sources->lifetime());
		const auto chooseChat = content->add(
			object_ptr<Ui::RoundButton>(content,
				rpl::single(QString::fromUtf8(Mokagram::Lang::Tr("Выбрать чат"))),
				st::defaultLightButton),
			QMargins(16, 0, 16, 4),
			style::al_justify);
		chooseChat->resize(chooseChat->width(), 36);

		// Вкладки: поиск, лайкнутые, подборка.
		const auto tabs = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(16, 4, 16, 6));
		tabs->resize(tabs->width(), 36);
		const auto tabSearch = Ui::CreateChild<TabPill>(tabs, QString::fromUtf8(Mokagram::Lang::Tr("Поиск")));
		const auto tabLiked = Ui::CreateChild<TabPill>(tabs, QString::fromUtf8(Mokagram::Lang::Tr("Лайкнутые")));
		const auto tabForYou = Ui::CreateChild<TabPill>(tabs, QString::fromUtf8(Mokagram::Lang::Tr("Для вас")));
		tabs->widthValue(
		) | rpl::on_next([=](int width) {
			const auto gap = 8;
			const auto each = (width - 2 * gap) / 3;
			tabSearch->setGeometry(0, 0, QueueState().telegramSearch ? width : each, tabs->height());
			tabLiked->setGeometry(each + gap, 0, each, 36);
			tabForYou->setGeometry(2 * (each + gap), 0, width - 2 * (each + gap), 36);
		}, tabs->lifetime());

		const auto list = content->add(object_ptr<Ui::VerticalLayout>(content));
		Ui::AddSkip(content, 10);

		// Смена раздела: старое содержимое гаснет и уезжает в сторону,
		// новое проявляется и въезжает с другой стороны.
		struct Transition {
			Ui::Animations::Simple animation;
			QPixmap was;
			QPixmap now;
			int top = 0;
			int direction = 0;
			bool changing = false;
		};
		const auto transition = box->lifetime().make_state<Transition>();
		const auto overlay = Ui::CreateChild<Ui::RpWidget>(content);
		overlay->hide();
		const auto transitionRect = [=] {
			const auto bottom = list->y() + list->height();
			return QRect(
				0,
				transition->top,
				content->width(),
				std::max(bottom - transition->top, 1));
		};
		const auto grabNow = [=] {
			overlay->hide();
			const auto rect = transitionRect();
			transition->now = Ui::GrabWidget(content, rect);
			overlay->setGeometry(rect);
			overlay->show();
			overlay->raise();
		};
		overlay->paintRequest(
		) | rpl::on_next([=] {
			auto p = QPainter(overlay);
			const auto progress = transition->animation.value(1.);
			const auto shift = 28. * transition->direction;
			p.fillRect(overlay->rect(), st::boxBg);
			p.setOpacity(1. - progress);
			p.drawPixmap(int(base::SafeRound(-shift * progress)), 0, transition->was);
			p.setOpacity(progress);
			p.drawPixmap(int(base::SafeRound(shift * (1. - progress))), 0, transition->now);
		}, overlay->lifetime());
		const auto animateChange = [=](int top, int direction, Fn<void()> change) {
			if (transition->changing) {
				change();
				return;
			}
			overlay->hide();
			transition->top = top;
			auto was = Ui::GrabWidget(content, transitionRect());
			transition->changing = true;
			change();
			transition->changing = false;
			transition->was = std::move(was);
			transition->direction = direction;
			grabNow();
			transition->animation.start([=] {
				overlay->update();
				if (!transition->animation.animating()) {
					overlay->hide();
					transition->was = transition->now = QPixmap();
				}
			}, 0., 1., kSwitchDuration, anim::easeOutCubic);
		};
		// Если список обновился прямо во время перехода, подменяем новый кадр.
		const auto refreshTransition = [=] {
			if (!transition->changing && transition->animation.animating()) {
				grabNow();
			}
		};
		const auto statusText = std::make_shared<QString>();
		const auto statusLoading = std::make_shared<bool>(false);
		const auto statusSticker = std::make_shared<bool>(false);
		const auto refreshForYou = std::make_shared<Fn<void()>>();
		struct SearchState {
			std::unique_ptr<ChatMusicSearch> chats;
			std::unique_ptr<ChatMusicSearch> extraChat;
			int generation = 0;
		};
		const auto searchState = box->lifetime().make_state<SearchState>();
		const auto runSearch = std::make_shared<Fn<void()>>();

		// Правая кнопка по стикеру: сменить на свой или вернуть стандартный.
		struct HintStickerState {
			base::unique_qptr<Ui::PopupMenu> menu;
			base::unique_qptr<ChatHelpers::TabbedPanel> panel;
		};
		const auto hintSticker = box->lifetime().make_state<HintStickerState>();
		const auto chooseHintSticker = [=](QPoint global) {
			const auto outer = box->getDelegate()->outerContainer();
			if (!outer) {
				return;
			}
			if (!hintSticker->panel) {
				using Selector = ChatHelpers::TabbedSelector;
				hintSticker->panel = base::make_unique_q<ChatHelpers::TabbedPanel>(
					outer.data(),
					controller,
					object_ptr<Selector>(
						nullptr,
						controller->uiShow(),
						Window::GifPauseReason::Layer,
						Selector::Mode::StickersOnly));
				const auto panel = hintSticker->panel.get();
				panel->setDropDown(false);
				panel->setDesiredHeightValues(
					1.,
					st::emojiPanMinHeight / 2,
					st::emojiPanMinHeight);
				panel->hide();
				panel->selector()->setCurrentPeer(controller->session().user());
				panel->selector()->fileChosen(
				) | rpl::on_next([=](ChatHelpers::FileChosen data) {
					SaveHintSticker(data.document);
					panel->hideAnimated();
				}, panel->lifetime());
			}
			const auto panel = hintSticker->panel.get();
			const auto local = outer->mapFromGlobal(global);
			panel->moveBottomRight(local.y(), local.x() + panel->width() / 2);
			panel->showAnimated();
		};
		const auto showHintMenu = [=](QPoint global) {
			hintSticker->menu = base::make_unique_q<Ui::PopupMenu>(
				box,
				st::popupMenuWithIcons);
			const auto menu = hintSticker->menu.get();
			menu->addAction(
				QString::fromUtf8(Mokagram::Lang::Tr("Сменить стикер")),
				[=] { chooseHintSticker(global); },
				&st::menuIconStickers);
			if (!Mokagram::Settings::String(
					QString::fromLatin1(kHintStickerKey)).isEmpty()) {
				menu->addAction(
					QString::fromUtf8(Mokagram::Lang::Tr("Вернуть стандартный")),
					[] { SaveHintSticker(nullptr); },
					&st::menuIconRestore);
			}
			menu->popup(global);
		};

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
							(*refreshForYou)();
						}),
					QMargins(16, 4, 16, 10));
			}
			if (*statusSticker) {
				list->add(object_ptr<StickerHint>(
					list,
					&controller->session(),
					showHintMenu));
			} else if (!statusText->isEmpty() && *statusLoading) {
				list->add(object_ptr<LoadingView>(list, *statusText));
			} else if (!statusText->isEmpty()) {
				list->add(
					object_ptr<Ui::FlatLabel>(list, *statusText, st::boxLabel),
					QMargins(24, 8, 24, 8));
			}
			for (auto i = 0; i != int(queue.results.size()); ++i) {
				list->add(object_ptr<Row>(list, controller, queue.results[i], i));
			}
			const auto activeChatSearch = queue.selectedChat
				? searchState->extraChat.get()
				: searchState->chats.get();
			if (queue.tab == 0 && queue.telegramSearch
				&& activeChatSearch && activeChatSearch->hasMore()) {
				const auto more = list->add(
					object_ptr<Ui::RoundButton>(
						list,
						rpl::single(QString::fromUtf8(Mokagram::Lang::Tr("Показать ещё"))),
						st::defaultLightButton),
					QMargins(24, 8, 24, 8));
				more->setClickedCallback([=] {
					const auto active = QueueState().selectedChat
						? searchState->extraChat.get()
						: searchState->chats.get();
					if (active) {
						active->searchMore();
					}
					more->setDisabled(true);
				});
			}
			refreshTransition();
		};
		const auto showStatus = [=](const QString &text, bool loading = false, bool sticker = false) {
			*statusText = text;
			*statusLoading = loading;
			*statusSticker = sticker;
			fill();
		};
		searchState->chats = std::make_unique<ChatMusicSearch>(
			&controller->session(),
			[=](std::vector<Track> tracks, QString error) {
				auto &queue = QueueState();
				if (!queue.telegramSearch) {
					return;
				}
				queue.searchResults = std::move(tracks);
				if (queue.tab == 0) {
					queue.results = queue.searchResults;
					showStatus(!error.isEmpty()
						? error
						: queue.results.empty()
						? QString::fromUtf8(Mokagram::Lang::Tr("Ничего не найдено"))
						: QString());
				}
			});
		const auto buildForYou = [=] {
			showStatus(QString::fromUtf8(Mokagram::Lang::Tr("Подбираю музыку под ваш вкус…")), true);
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
			if (queue.telegramSearch) {
				tab = 0;
			}
			queue.tab = tab;
			sources->setVisible(tab == 0);
			sources->resize(sources->width(), (tab == 0) ? 48 : 0);
			chooseChat->setVisible(tab == 0 && queue.telegramSearch);
			chooseChat->resize(chooseChat->width(), queue.telegramSearch ? 36 : 0);
			tabs->resize(tabs->width(), queue.telegramSearch ? 0 : 36);
			tabLiked->setVisible(!queue.telegramSearch);
			tabForYou->setVisible(!queue.telegramSearch);
			tabSearch->setActive(tab == 0);
			tabLiked->setActive(tab == 1);
			tabForYou->setActive(tab == 2);
			if (tab == 0) {
				queue.results = queue.searchResults;
				showStatus(QString(), false, queue.searchResults.empty());
			} else if (tab == 1) {
				queue.results = LikedTracks();
				showStatus(queue.results.empty()
					? QString::fromUtf8(Mokagram::Lang::Tr("Пока пусто. Нажмите сердечко у трека, и он появится здесь."))
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
		const auto switchTab = [=](int tab) {
			const auto was = QueueState().tab;
			if (was == tab) {
				return;
			}
			animateChange(
				searchWrap->y() + searchWrap->height(),
				(tab > was) ? 1 : -1,
				[=] { setTab(tab); });
		};
		tabSearch->setClickedCallback([=] { switchTab(0); });
		tabLiked->setClickedCallback([=] { switchTab(1); });
		tabForYou->setClickedCallback([=] { switchTab(2); });
		LikesChanged(
		) | rpl::filter([=] {
			return QueueState().tab == 1;
		}) | rpl::on_next([=] {
			auto &queue = QueueState();
			queue.results = LikedTracks();
			showStatus(queue.results.empty()
				? QString::fromUtf8(Mokagram::Lang::Tr("Пока пусто. Нажмите сердечко у трека, и он появится здесь."))
				: QString());
			fill();
		}, box->lifetime());

		const auto search = [=] {
			const auto query = input->getLastText().trimmed();
			auto &queue = QueueState();
			if (query.isEmpty() && !queue.telegramSearch) {
				return;
			}
			if (queue.tab != 0) {
				switchTab(0);
			}
			const auto generation = ++searchState->generation;
			searchState->chats->cancel();
			queue.lastQuery = query;
			queue.searchResults.clear();
			queue.results.clear();
			showStatus(QString::fromUtf8(Mokagram::Lang::Tr("Ищу…")), true);
			if (queue.telegramSearch) {
				if (queue.selectedChat) {
					searchState->chats->cancel();
					searchState->extraChat = std::make_unique<ChatMusicSearch>(
						&controller->session(),
						[=](std::vector<Track> tracks, QString error) {
							auto &queue = QueueState();
							if (!queue.telegramSearch || queue.tab != 0) {
								return;
							}
							queue.searchResults = std::move(tracks);
							queue.results = queue.searchResults;
							showStatus(!error.isEmpty()
								? error
								: queue.results.empty()
								? QString::fromUtf8(Mokagram::Lang::Tr("Ничего не найдено"))
								: QString());
						},
						queue.selectedChat);
					searchState->extraChat->search(query);
				} else {
					searchState->chats->search(query);
				}
				return;
			}
			Search(query, 0, crl::guard(box, [=](SearchResult result) {
				if (generation != searchState->generation) {
					return;
				}
				auto &queue = QueueState();
				queue.searchResults = std::move(result.tracks);
				if (queue.tab == 0) {
					queue.results = queue.searchResults;
					showStatus(!result.error.isEmpty()
						? result.error
						: queue.results.empty()
						? QString::fromUtf8(Mokagram::Lang::Tr("Ничего не найдено"))
						: QString());
				}
			}));
		};
		*runSearch = search;
		chooseChat->setClickedCallback([=] {
			Window::ShowChooseRecipientBox(
				controller,
				[=](not_null<Data::Thread*> thread) {
					QueueState().selectedChat = thread->owningHistory()->peer->id;
					const auto peer = controller->session().data().peer(QueueState().selectedChat);
					chooseChat->setText(rpl::single(
						QString::fromUtf8(Mokagram::Lang::Tr("Чат: %1"))
						.arg(peer->name())));
					if (*runSearch) {
						(*runSearch)();
					}
					return true;
				},
				rpl::single(QString::fromUtf8(Mokagram::Lang::Tr("Выберите чат с музыкой"))));
		});
		const auto updateSource = [=] {
			const auto chats = QueueState().telegramSearch;
			sourceSwitch->setChats(chats);
			chooseChat->setVisible(chats && QueueState().tab == 0);
			chooseChat->resize(chooseChat->width(), chats ? 36 : 0);
			tabs->resize(tabs->width(), chats ? 0 : 36);
			if (QueueState().selectedChat) {
				if (const auto peer = controller->session().data().peer(QueueState().selectedChat)) {
					chooseChat->setText(rpl::single(
						QString::fromUtf8(Mokagram::Lang::Tr("Чат: %1"))
						.arg(peer->name())));
				}
			}
			tabLiked->setVisible(!chats);
			tabForYou->setVisible(!chats);
			tabs->update();
			if (chats) {
				tabSearch->setGeometry(0, 0, tabs->width(), 36);
			} else {
				const auto gap = 8;
				const auto each = (tabs->width() - 2 * gap) / 3;
				tabSearch->setGeometry(0, 0, each, 36);
				tabLiked->setGeometry(each + gap, 0, each, 36);
				tabForYou->setGeometry(2 * (each + gap), 0, tabs->width() - 2 * (each + gap), 36);
			}
			input->setPlaceholder(rpl::single(input->getLastText().isEmpty()
				? QString::fromUtf8(Mokagram::Lang::Tr(chats
					? "Поиск музыки в чатах и группах"
					: "Поиск на SoundCloud"))
				: QString()));
		};
		const auto setSource = [=](bool chats) {
			if (QueueState().telegramSearch == chats) {
				return;
			}
			++searchState->generation;
			searchState->chats->cancel();
			if (searchState->extraChat) {
				searchState->extraChat->cancel();
			}
			QueueState().telegramSearch = chats;
			QueueState().searchResults.clear();
			QueueState().results.clear();
			updateSource();
			setTab(0);
			search();
		};
		sourceSwitch->setChangedCallback([=](bool chats) {
			// Сам переключатель анимируется отдельно, переход идёт под ним.
			animateChange(
				sources->y() + sources->height(),
				chats ? 1 : -1,
				[=] { setSource(chats); });
		});
		// Поиск по мере набора: запускаем после короткой паузы между буквами.
		const auto typingTimer = box->lifetime().make_state<base::Timer>();
		typingTimer->setCallback([=] {
			const auto query = input->getLastText().trimmed();
			if (query.size() >= 2 && query != QueueState().lastQuery) {
				search();
			}
		});
		input->changes() | rpl::on_next([=] {
			updateSource();
			typingTimer->callOnce(kTypingDelay);
		}, input->lifetime());
		input->submits() | rpl::on_next([=] {
			typingTimer->cancel();
			search();
		}, input->lifetime());
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
			if (QueueState().tab == 0 && QueueState().telegramSearch) {
				search();
			}
		}
		updateSource();
		sourceSwitch->finishAnimating();
	}));
}

} // namespace Mokagram::Music
