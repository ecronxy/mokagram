#include "mokagram/music/mokagram_music_ui.h"

#include "mokagram/music/mokagram_soundcloud.h"

#include "base/unixtime.h"
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

#include <QtCore/QFileInfo>
#include <QtCore/QPointer>

namespace Mokagram::Music {
namespace {

constexpr auto kRowHeight = 64;
constexpr auto kArtSize = 48;

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

class Row final : public Ui::AbstractButton {
public:
	Row(QWidget *parent, const Track &track, int index)
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
	}

private:
	void paint() {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto playing = (QueueState().currentId == _track.id);
		const auto over = isOver() || isDown();
		if (playing || over) {
			p.setPen(Qt::NoPen);
			p.setBrush(playing ? st::windowBgOver : st::windowBgOver);
			p.drawRoundedRect(rect().marginsRemoved({ 8, 2, 8, 2 }), 12, 12);
		}
		const auto art = QRect(16, (height() - kArtSize) / 2, kArtSize, kArtSize);
		auto path = QPainterPath();
		path.addRoundedRect(art, 10, 10);
		if (!_art.isNull()) {
			p.save();
			p.setClipPath(path);
			p.drawImage(art, _art);
			p.restore();
		} else {
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawPath(path);
		}
		const auto left = art.right() + 14;
		const auto right = width() - 16;
		const auto durationWidth = st::normalFont->width(FormatDuration(_track.durationMs));
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(right - durationWidth, 0, durationWidth, height()),
			Qt::AlignVCenter | Qt::AlignRight,
			FormatDuration(_track.durationMs));
		const auto textWidth = right - durationWidth - 12 - left;
		p.setFont(st::semiboldFont);
		p.setPen(playing ? st::windowActiveTextFg : st::windowFg);
		p.drawText(
			QRect(left, height() / 2 - 22, textWidth, 22),
			Qt::AlignVCenter | Qt::AlignLeft,
			st::semiboldFont->elided(_track.title, textWidth));
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(left, height() / 2 + 1, textWidth, 20),
			Qt::AlignVCenter | Qt::AlignLeft,
			st::normalFont->elided(_track.artist, textWidth));
	}

	Track _track;
	QImage _art;

};

} // namespace

void ShowMusicBox(
		not_null<Window::SessionController*> controller,
		const QString &initialQuery) {
	QueueState().controller = base::make_weak(controller);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(QString::fromUtf8("Музыка")));
		box->setWidth(st::boxWideWidth);
		const auto content = box->verticalLayout();
		Ui::AddSkip(content);
		const auto input = content->add(
			object_ptr<Ui::InputField>(
				content,
				st::defaultInputField,
				rpl::single(QString::fromUtf8("Поиск на SoundCloud"))),
			st::boxPadding);
		const auto status = content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				QString::fromUtf8("Введите название или исполнителя и нажмите Enter"),
				st::boxLabel),
			st::boxPadding);
		const auto list = content->add(object_ptr<Ui::VerticalLayout>(content));
		Ui::AddSkip(content);

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
				auto &queue = QueueState();
				queue.tracks = result.tracks;
				for (auto i = 0; i != int(result.tracks.size()); ++i) {
					list->add(object_ptr<Row>(list, result.tracks[i], i));
				}
			}));
		};
		input->submits() | rpl::on_next(search, input->lifetime());
		input->setFocusFast();
		if (!initialQuery.isEmpty()) {
			input->setText(initialQuery);
			search();
		}
		box->addButton(rpl::single(QString::fromUtf8("Закрыть")), [=] { box->closeBox(); });
	}));
}

} // namespace Mokagram::Music
