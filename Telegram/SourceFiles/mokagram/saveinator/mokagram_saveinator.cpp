// Mokagram: скачивание медиа по ссылке из сообщения через API Saveinator.
#include "mokagram/saveinator/mokagram_saveinator.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "base/weak_ptr.h"
#include "data/data_chat_participant_status.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/chat/attach/attach_send_files_way.h"
#include "mokagram/settings/mokagram_settings.h"
#include "mokagram/ui/mokagram_box_parts.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/rp_widget.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <array>
#include <memory>
#include <optional>

namespace Mokagram::Saveinator {
namespace {

constexpr auto kEnabledKey = "saveinator.enabled";
constexpr auto kMediaKey = "saveinator.media";
constexpr auto kQualityKey = "saveinator.quality";
constexpr auto kApiUrl = "https://saveinator-hooks.xdshka.party";
constexpr auto kApiKey = "SmPzzuTM6en_vn7AtgUKcLvB_ObB7QqafyGuBGuxl1E";
constexpr auto kLimit = 10;
constexpr auto kTimeout = 5 * 60 * 1000;
constexpr auto kUserAgent = "Saveinator-mokaGram/1.0";
constexpr auto kPollDelay = 2 * 1000;
constexpr auto kRequestTimeout = 30 * 1000;
constexpr auto kMaxJsonSize = 64 * 1024;
constexpr auto kMaxFileSize = qint64(2) * 1024 * 1024 * 1024;

constexpr auto kMediaValues = std::array{ "auto", "video", "audio", "photos" };
constexpr auto kHeightValues = std::array{ 0, 1080, 720, 480, 360 };

struct Choice {
	QString media;
	int maxHeight = 0;
};

[[nodiscard]] QStringList MediaTitles() {
	return {
		QString::fromUtf8("Всё, что есть"),
		QString::fromUtf8("Только видео"),
		QString::fromUtf8("Только звук"),
		QString::fromUtf8("Только фото"),
	};
}

[[nodiscard]] QStringList QualityTitles() {
	return {
		QString::fromUtf8("Лучшее"),
		u"1080p"_q,
		u"720p"_q,
		u"480p"_q,
		u"360p"_q,
	};
}

[[nodiscard]] bool MediaHasVideo(int media) {
	return (media == 0) || (media == 1);
}

[[nodiscard]] Choice MakeChoice(int media, int quality) {
	media = std::clamp(media, 0, int(kMediaValues.size()) - 1);
	quality = std::clamp(quality, 0, int(kHeightValues.size()) - 1);
	return {
		.media = QString::fromLatin1(kMediaValues[media]),
		.maxHeight = MediaHasVideo(media) ? kHeightValues[quality] : 0,
	};
}

struct File {
	QString url;
	QString contentType;
	QString extension;
	qint64 size = 0;
};

struct Job {
	base::weak_ptr<Window::SessionController> controller;
	base::weak_ptr<History> history;
	std::optional<Api::SendAction> action;
	QString link;
	Choice choice;
	QString base;
	QByteArray key;
	QString id;
	QString directory;
	std::vector<File> files;
	int next = 0;
	QStringList paths;
	QString text;
	QPointer<QNetworkReply> reply;
	QPointer<QTimer> deadline;
	bool finished = false;
};

[[nodiscard]] base::flat_set<QString> &Active() {
	static auto result = base::flat_set<QString>();
	return result;
}

auto SendingAsText = false;

[[nodiscard]] QNetworkAccessManager *Manager() {
	static const auto result = new QNetworkAccessManager();
	return result;
}

[[nodiscard]] QString Root() {
	return QDir::tempPath() + u"/mokagram-saveinator"_q;
}

[[nodiscard]] QString ErrorText(const QString &code) {
	if (code == u"capacity_exceeded"_q
		|| code == u"shutting_down"_q) {
		return QString::fromUtf8("Сервис скачивания занят. Попробуйте чуть позже.");
	} else if (code == u"unsupported_url"_q) {
		return QString::fromUtf8("Эта ссылка пока не поддерживается.");
	} else if (code == u"no_media"_q) {
		return QString::fromUtf8("Медиа для скачивания не найдено.");
	} else if (code == u"too_large"_q) {
		return QString::fromUtf8(
			"Медиафайл превышает ограничение размера на сервере.");
	} else if (code == u"timeout"_q) {
		return QString::fromUtf8(
			"Скачивание заняло слишком много времени. Попробуйте позже.");
	} else if (code == u"not_found"_q) {
		return QString::fromUtf8(
			"Срок хранения истёк или результат недоступен.");
	}
	return QString::fromUtf8("Не удалось скачать медиа по этой ссылке.");
}

[[nodiscard]] QString ExtensionFor(const QString &contentType) {
	static const auto map = base::flat_map<QString, QString>{
		{ u"image/jpeg"_q, u"jpg"_q },
		{ u"image/png"_q, u"png"_q },
		{ u"image/webp"_q, u"webp"_q },
		{ u"image/gif"_q, u"gif"_q },
		{ u"video/mp4"_q, u"mp4"_q },
		{ u"video/webm"_q, u"webm"_q },
		{ u"video/quicktime"_q, u"mov"_q },
		{ u"video/x-matroska"_q, u"mkv"_q },
		{ u"audio/mpeg"_q, u"mp3"_q },
		{ u"audio/mp4"_q, u"m4a"_q },
		{ u"audio/ogg"_q, u"ogg"_q },
		{ u"audio/wav"_q, u"wav"_q },
		{ u"audio/flac"_q, u"flac"_q },
		{ u"text/plain"_q, u"txt"_q },
	};
	const auto i = map.find(contentType);
	return (i != map.end()) ? i->second : QString();
}

[[nodiscard]] bool SupportedHost(const QString &host) {
	static const auto hosts = QStringList{
		u"youtube.com"_q, u"youtu.be"_q, u"tiktok.com"_q, u"x.com"_q,
		u"twitter.com"_q, u"instagram.com"_q, u"instagr.am"_q,
		u"reddit.com"_q, u"redd.it"_q, u"pin.it"_q, u"spotify.com"_q,
		u"soundcloud.com"_q, u"vk.com"_q, u"vk.ru"_q, u"vkontakte.ru"_q,
		u"twitch.tv"_q,
	};
	for (const auto &known : hosts) {
		if (host == known || host.endsWith('.' + known)) {
			return true;
		}
	}
	return host.startsWith(u"music.yandex."_q)
		|| host.split('.').contains(u"pinterest"_q);
}

[[nodiscard]] QString Normalized(QString link) {
	while (!link.isEmpty() && u".,;:!?)]}"_q.contains(link.back())) {
		link.chop(1);
	}
	if (!link.contains(u"://"_q)) {
		link = u"https://"_q + link;
	}
	const auto url = QUrl(link, QUrl::StrictMode);
	const auto scheme = url.scheme().toLower();
	return (url.isValid()
		&& (scheme == u"https"_q || scheme == u"http"_q)
		&& SupportedHost(url.host().toLower()))
		? link
		: QString();
}

[[nodiscard]] QNetworkRequest MakeRequest(
		const std::shared_ptr<Job> &job,
		const QString &path) {
	auto result = QNetworkRequest(QUrl(job->base + path));
	result.setRawHeader("Authorization", "Bearer " + job->key);
	result.setRawHeader("User-Agent", kUserAgent);
	result.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::ManualRedirectPolicy);
	result.setTransferTimeout(kRequestTimeout);
	return result;
}

void Toast(const std::shared_ptr<Job> &job, const QString &text) {
	if (const auto controller = job->controller.get()) {
		controller->showToast(text);
	}
}

void Finish(const std::shared_ptr<Job> &job, const QString &error) {
	if (job->finished) {
		return;
	}
	job->finished = true;
	Active().remove(job->link);
	if (job->deadline) {
		job->deadline->deleteLater();
	}
	if (const auto reply = job->reply.data()) {
		job->reply = nullptr;
		reply->abort();
		reply->deleteLater();
	}
	if (!error.isEmpty()) {
		if (!job->directory.isEmpty()) {
			QDir(job->directory).removeRecursively();
		}
		Toast(job, ErrorText(error));
	}
}

[[nodiscard]] QString HttpError(not_null<QNetworkReply*> reply) {
	const auto status = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (status == 401) {
		return u"unauthorized"_q;
	} else if (status == 404) {
		return u"not_found"_q;
	} else if (status == 429) {
		return u"capacity_exceeded"_q;
	} else if (reply->error() == QNetworkReply::OperationCanceledError
		|| reply->error() == QNetworkReply::TimeoutError) {
		return u"timeout"_q;
	}
	return u"failed"_q;
}

void SendJson(
		const std::shared_ptr<Job> &job,
		QNetworkReply *reply,
		Fn<void(QJsonObject)> done) {
	job->reply = reply;
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		if (job->finished || job->reply != reply) {
			return;
		}
		job->reply = nullptr;
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto body = reply->read(kMaxJsonSize + 1);
		const auto object = QJsonDocument::fromJson(body).object();
		if (status == 200 || status == 202) {
			if (body.size() > kMaxJsonSize || object.isEmpty()) {
				Finish(job, u"failed"_q);
			} else {
				done(object);
			}
		} else if (status == 400 && object.contains(u"error"_q)) {
			Finish(job, object.value(u"error"_q).toString());
		} else {
			Finish(job, HttpError(reply));
		}
	});
}

void Deliver(const std::shared_ptr<Job> &job) {
	Finish(job, QString());
	const auto controller = job->controller.get();
	const auto history = job->history.get();
	if (!controller || !history) {
		return;
	} else if (job->paths.isEmpty() && job->text.isEmpty()) {
		controller->showToast(ErrorText(u"no_media"_q));
		return;
	}
	const auto peer = history->peer;
	auto &api = history->session().api();
	if (!job->paths.isEmpty()) {
		const auto error = Data::AnyFileRestrictionError(peer);
		if (error || !Data::CanSendAnyOf(
				peer,
				Data::FilesSendRestrictions(),
				false)) {
			if (error) {
				Data::ShowSendErrorToast(controller, peer, error);
			} else {
				controller->showToast(QString::fromUtf8(
					"В этот чат нельзя отправлять файлы."));
			}
			return;
		}
		auto list = Storage::PrepareMediaList(
			job->paths,
			st::sendMediaPreviewSize,
			history->session().premium());
		if (list.error != Ui::PreparedList::Error::None) {
			controller->showToast(ErrorText(
				(list.error == Ui::PreparedList::Error::TooLargeFile)
					? u"too_large"_q
					: u"failed"_q));
			return;
		}
		auto groups = Ui::DivideByGroups(
			std::move(list),
			Ui::SendFilesWay(),
			false);
		for (auto &group : groups) {
			const auto album = (group.type != Ui::AlbumType::None)
				? std::make_shared<SendingAlbum>()
				: nullptr;
			api.sendFiles(
				std::move(group.list),
				SendMediaType::Photo,
				album,
				*job->action);
		}
	}
	if (!job->text.isEmpty()) {
		auto message = Api::MessageToSend(*job->action);
		message.textWithTags = TextWithTags{ job->text };
		api.sendMessage(std::move(message));
	}
}

void DownloadNext(const std::shared_ptr<Job> &job) {
	const auto index = job->next++;
	if (index == int(job->files.size())) {
		Deliver(job);
		return;
	}
	const auto &file = job->files[index];
	const auto path = job->directory
		+ u"/media_%1.%2"_q.arg(index).arg(file.extension);
	const auto output = std::make_shared<QFile>(path);
	if (!output->open(QIODevice::WriteOnly)) {
		Finish(job, u"failed"_q);
		return;
	}
	const auto reply = Manager()->get(MakeRequest(job, file.url));
	const auto received = std::make_shared<qint64>(0);
	const auto consume = [=] {
		const auto chunk = reply->readAll();
		*received += chunk.size();
		if (*received > file.size) {
			Finish(job, u"too_large"_q);
		} else if (output->write(chunk) != chunk.size()) {
			Finish(job, u"failed"_q);
		}
		return !job->finished;
	};
	job->reply = reply;
	QObject::connect(reply, &QNetworkReply::readyRead, [=] {
		if (!job->finished && job->reply == reply) {
			consume();
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		if (job->finished || job->reply != reply || !consume()) {
			output->close();
			return;
		}
		output->close();
		job->reply = nullptr;
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto type = reply->header(
			QNetworkRequest::ContentTypeHeader).toString();
		if (status != 200) {
			Finish(job, HttpError(reply));
			return;
		} else if (*received != file.size
			|| type.section(';', 0, 0).trimmed() != file.contentType) {
			Finish(job, u"failed"_q);
			return;
		}
		if (file.contentType == u"text/plain"_q) {
			QFile text(path);
			if (text.open(QIODevice::ReadOnly)) {
				job->text = QString::fromUtf8(text.readAll()).trimmed();
			}
		} else {
			job->paths.push_back(path);
		}
		DownloadNext(job);
	});
}

void Ready(const std::shared_ptr<Job> &job, const QJsonObject &object) {
	const auto files = object.value(u"files"_q).toArray();
	if (files.isEmpty() || files.size() > kLimit) {
		Finish(job, u"no_media"_q);
		return;
	}
	const auto prefix = u"/api/v1/downloads/%1/files/"_q.arg(job->id);
	auto texts = 0;
	for (const auto &value : files) {
		const auto item = value.toObject();
		const auto url = item.value(u"url"_q).toString();
		const auto index = url.mid(prefix.size());
		auto file = File{
			.url = url,
			.contentType = item.value(u"content_type"_q).toString(),
			.size = qint64(item.value(u"size"_q).toDouble()),
		};
		file.extension = ExtensionFor(file.contentType);
		if (!url.startsWith(prefix)
			|| index.isEmpty()
			|| !QRegularExpression(u"^\\d+$"_q).match(index).hasMatch()
			|| file.extension.isEmpty()) {
			Finish(job, u"failed"_q);
			return;
		} else if (file.size <= 0 || file.size > kMaxFileSize) {
			Finish(job, u"too_large"_q);
			return;
		}
		texts += (file.contentType == u"text/plain"_q) ? 1 : 0;
		job->files.push_back(std::move(file));
	}
	if (texts > 1) {
		Finish(job, u"failed"_q);
		return;
	}
	job->directory = Root() + '/' + job->id;
	if (!QDir().mkpath(job->directory)) {
		Finish(job, u"failed"_q);
		return;
	}
	DownloadNext(job);
}

void Poll(const std::shared_ptr<Job> &job, const QJsonObject &object) {
	const auto status = object.value(u"status"_q).toString();
	if (status == u"ready"_q) {
		Ready(job, object);
	} else if (status == u"queued"_q || status == u"running"_q) {
		QTimer::singleShot(kPollDelay, [=] {
			if (job->finished) {
				return;
			}
			const auto path = u"/api/v1/downloads/"_q + job->id;
			SendJson(job, Manager()->get(MakeRequest(job, path)), [=](
					QJsonObject object) {
				Poll(job, object);
			});
		});
	} else {
		const auto error = object.value(u"error"_q).toString();
		Finish(job, error.isEmpty() ? u"failed"_q : error);
	}
}

void Download(
		not_null<Window::SessionController*> controller,
		const Api::SendAction &action,
		const QString &link,
		Choice choice) {
	if (Active().contains(link)) {
		controller->showToast(
			QString::fromUtf8("Эта ссылка уже скачивается."));
		return;
	}
	const auto job = std::make_shared<Job>(Job{
		.controller = base::make_weak(controller),
		.history = base::make_weak(action.history.get()),
		.action = action,
		.link = link,
		.choice = choice,
		.base = QString::fromLatin1(kApiUrl),
		.key = QByteArray(kApiKey),
	});
	Active().emplace(link);
	controller->showToast(QString::fromUtf8(
		"Скачиваю… Отправлю в чат, как только будет готово."));

	const auto deadline = new QTimer();
	deadline->setSingleShot(true);
	QObject::connect(deadline, &QTimer::timeout, [=] {
		Finish(job, u"timeout"_q);
	});
	job->deadline = deadline;
	deadline->start(kTimeout);

	auto request = MakeRequest(job, u"/api/v1/downloads"_q);
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);
	auto fields = QJsonObject{
		{ u"url"_q, link },
		{ u"media"_q, choice.media },
		{ u"limit"_q, kLimit },
	};
	if (choice.maxHeight > 0) {
		fields.insert(u"max_height"_q, choice.maxHeight);
	}
	const auto body = QJsonDocument(fields).toJson(QJsonDocument::Compact);
	SendJson(job, Manager()->post(request, body), [=](QJsonObject object) {
		job->id = object.value(u"id"_q).toString();
		const auto valid = QRegularExpression(
			u"^[a-f0-9-]{36}$"_q).match(job->id).hasMatch();
		if (!valid) {
			Finish(job, u"failed"_q);
		} else {
			Poll(job, object);
		}
	});
}

[[nodiscard]] bool IsMusicLink(const QString &link) {
	const auto url = QUrl(link);
	const auto host = url.host().toLower();
	const auto is = [&](const QString &known) {
		return (host == known) || host.endsWith('.' + known);
	};
	return is(u"spotify.com"_q)
		|| is(u"soundcloud.com"_q)
		|| host.startsWith(u"music.yandex."_q)
		|| ((is(u"vk.com"_q) || is(u"vk.ru"_q) || is(u"vkontakte.ru"_q))
			&& url.path().startsWith(u"/audio"_q));
}

[[nodiscard]] QString PlatformName(const QString &host) {
	const auto is = [&](std::initializer_list<const char*> known) {
		for (const auto name : known) {
			const auto value = QString::fromLatin1(name);
			if (host == value || host.endsWith('.' + value)) {
				return true;
			}
		}
		return false;
	};
	if (is({ "youtube.com", "youtu.be" })) {
		return u"YouTube"_q;
	} else if (is({ "tiktok.com" })) {
		return u"TikTok"_q;
	} else if (is({ "x.com", "twitter.com" })) {
		return u"X"_q;
	} else if (is({ "instagram.com", "instagr.am" })) {
		return u"Instagram"_q;
	} else if (is({ "reddit.com", "redd.it" })) {
		return u"Reddit"_q;
	} else if (is({ "pin.it" }) || host.split('.').contains(u"pinterest"_q)) {
		return u"Pinterest"_q;
	} else if (is({ "vk.com", "vk.ru", "vkontakte.ru" })) {
		return u"VK"_q;
	} else if (is({ "twitch.tv" })) {
		return u"Twitch"_q;
	} else if (is({ "spotify.com" })) {
		return u"Spotify"_q;
	} else if (is({ "soundcloud.com" })) {
		return u"SoundCloud"_q;
	} else if (host.startsWith(u"music.yandex."_q)) {
		return QString::fromUtf8("Яндекс Музыка");
	}
	return QString();
}

class Segments final : public Ui::RpWidget {
public:
	Segments(QWidget *parent, QStringList titles, int active)
	: RpWidget(parent)
	, _titles(std::move(titles))
	, _active(std::clamp(active, 0, int(_titles.size()) - 1)) {
		setCursor(style::cur_pointer);
		_active.changes() | rpl::on_next([=] {
			update();
		}, lifetime());
	}

	[[nodiscard]] int current() const {
		return _active.current();
	}
	[[nodiscard]] rpl::producer<int> value() const {
		return _active.value();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return kHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto full = QRectF(rect());
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBg);
		p.drawRoundedRect(full, full.height() / 2., full.height() / 2.);
		const auto active = segment(_active.current()).marginsRemoved(
			QMarginsF(kInset, kInset, kInset, kInset));
		p.setBrush(st::activeButtonBg);
		p.drawRoundedRect(active, active.height() / 2., active.height() / 2.);
		for (auto i = 0; i != int(_titles.size()); ++i) {
			const auto selected = (i == _active.current());
			p.setFont(selected ? st::semiboldFont : st::normalFont);
			p.setPen(selected ? st::activeButtonFg : st::windowFg);
			p.drawText(segment(i), Qt::AlignCenter, _titles[i]);
		}
	}

	void mousePressEvent(QMouseEvent *e) override {
		_pressed = indexAt(e->pos());
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		const auto index = indexAt(e->pos());
		if (index >= 0 && index == _pressed) {
			_active = index;
		}
		_pressed = -1;
	}

private:
	static constexpr auto kHeight = 40;
	static constexpr auto kInset = 3.;

	[[nodiscard]] QRectF segment(int index) const {
		const auto part = width() / float64(_titles.size());
		return QRectF(part * index, 0, part, height());
	}

	[[nodiscard]] int indexAt(QPoint point) const {
		if (!rect().contains(point) || _titles.isEmpty()) {
			return -1;
		}
		return std::min(
			int(point.x() * _titles.size() / std::max(width(), 1)),
			int(_titles.size()) - 1);
	}

	QStringList _titles;
	rpl::variable<int> _active;
	int _pressed = -1;

};

[[nodiscard]] not_null<Segments*> AddSegments(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		QStringList titles,
		int active) {
	static const auto titleSt = [] {
		auto result = st::boxDividerLabel;
		result.minWidth = 1;
		return result;
	}();
	using BoxParts::kOuterSide;
	container->add(
		object_ptr<Ui::FlatLabel>(container, title, titleSt),
		QMargins(kOuterSide + 8, 14, kOuterSide + 8, 6));
	return container->add(
		object_ptr<Segments>(container, std::move(titles), active),
		QMargins(kOuterSide, 0, kOuterSide, 0));
}

void ChooseBox(
		not_null<Ui::GenericBox*> box,
		const QString &link,
		Fn<void(Choice)> done,
		Fn<void()> sendAsText) {
	BoxParts::RoundLayer(box);
	box->setStyle(BoxParts::PillButtonsBox());
	box->setTitle(rpl::single(QString::fromUtf8("Скачать по ссылке")));
	const auto content = box->verticalLayout();

	const auto url = QUrl(link);
	auto host = url.host().toLower();
	if (host.startsWith(u"www."_q)) {
		host = host.mid(4);
	}
	const auto platform = PlatformName(host);
	const auto shown = host + url.path();
	static const auto linkSt = [] {
		auto result = st::boxDividerLabel;
		result.minWidth = 1;
		result.maxHeight = st::boxDividerLabel.style.font->height;
		return result;
	}();
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			platform.isEmpty() ? shown : (platform + u" · "_q + shown),
			linkSt),
		QMargins(BoxParts::kOuterSide + 8, 0, BoxParts::kOuterSide + 8, 0));

	const auto videoOnly = (platform == u"YouTube"_q)
		|| (platform == u"Twitch"_q)
		|| (platform == u"VK"_q);
	const auto values = IsMusicLink(link)
		? std::vector<int>{ 2 }
		: videoOnly
		? std::vector<int>{ 1, 2 }
		: std::vector<int>{ 0, 1, 2, 3 };
	auto titles = QStringList();
	const auto names = std::array{ "Всё", "Видео", "Звук", "Фото" };
	for (const auto value : values) {
		titles.push_back(QString::fromUtf8(names[value]));
	}
	const auto saved = Settings::Int(QString::fromLatin1(kMediaKey));
	const auto i = ranges::find(values, saved);
	const auto media = (values.size() > 1)
		? AddSegments(
			content,
			QString::fromUtf8("Формат"),
			titles,
			(i != values.end()) ? int(i - values.begin()) : 0).get()
		: nullptr;
	const auto mediaValue = [=] {
		return values[media ? media->current() : 0];
	};

	const auto wrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)));
	const auto quality = AddSegments(
		wrap->entity(),
		QString::fromUtf8("Качество"),
		{
			QString::fromUtf8("Макс"),
			u"1080"_q,
			u"720"_q,
			u"480"_q,
			u"360"_q,
		},
		Settings::Int(QString::fromLatin1(kQualityKey)));
	wrap->toggleOn(media
		? (media->value() | rpl::map([=](int index) {
			return MediaHasVideo(values[index]);
		})) | rpl::type_erased
		: rpl::single(MediaHasVideo(values.front())));
	wrap->finishAnimating();
	Ui::AddSkip(content, BoxParts::kOuterSide);

	box->addButton(rpl::single(QString::fromUtf8("Скачать")), [=] {
		Settings::SetInt(QString::fromLatin1(kMediaKey), mediaValue());
		Settings::SetInt(QString::fromLatin1(kQualityKey), quality->current());
		const auto choice = MakeChoice(mediaValue(), quality->current());
		box->closeBox();
		done(choice);
	}, BoxParts::PillActive());
	if (sendAsText) {
		box->addLeftButton(
			rpl::single(QString::fromUtf8("Ссылкой")),
			[=] {
				box->closeBox();
				SendingAsText = true;
				sendAsText();
				SendingAsText = false;
			});
	} else {
		box->addButton(rpl::single(QString::fromUtf8("Отмена")), [=] {
			box->closeBox();
		});
	}
}

} // namespace

void RegisterSettings() {
	const auto section = QString::fromUtf8("Скачивание по ссылкам");
	const auto add = [&](Settings::Entry entry) {
		entry.section = section;
		entry.category = u"chats"_q;
		entry.sectionIcon = u"update"_q;
		entry.sectionNote = QString::fromUtf8(
			"Нажмите правой кнопкой на сообщение со ссылкой на YouTube, "
			"TikTok, X, Instagram, Reddit, Pinterest, VK, Spotify, "
			"SoundCloud или Яндекс Музыку: отправьте ссылку или выберите "
			"«Скачать по ссылке» в меню сообщения. Клиент спросит формат "
			"и сам отправит готовое видео, фото или трек в чат.");
		Settings::Register(std::move(entry));
	};
	add({
		.key = QString::fromLatin1(kEnabledKey),
		.title = QString::fromUtf8("Скачивание видео и музыки по ссылкам"),
		.description = QString::fromUtf8(
			"Пункт «Скачать по ссылке» в меню сообщений."),
		.kind = Settings::Kind::Bool,
		.defaultValue = true,
	});
	add({
		.key = QString::fromLatin1(kMediaKey),
		.title = QString::fromLatin1(kMediaKey),
		.kind = Settings::Kind::Choice,
		.defaultValue = 0,
		.choices = MediaTitles(),
		.hidden = true,
	});
	add({
		.key = QString::fromLatin1(kQualityKey),
		.title = QString::fromLatin1(kQualityKey),
		.kind = Settings::Kind::Choice,
		.defaultValue = 2,
		.choices = QualityTitles(),
		.hidden = true,
	});
}

void Start() {
	QDir(Root()).removeRecursively();
}

QString FindLink(not_null<HistoryItem*> item) {
	const auto &text = item->originalText();
	for (const auto &entity : text.entities) {
		const auto raw = (entity.type() == EntityType::CustomUrl)
			? entity.data()
			: (entity.type() == EntityType::Url)
			? text.text.mid(entity.offset(), entity.length())
			: QString();
		if (const auto link = Normalized(raw); !link.isEmpty()) {
			return link;
		}
	}
	return QString();
}

void AddMessageAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	if (!Settings::Bool(QString::fromLatin1(kEnabledKey))) {
		return;
	}
	const auto link = FindLink(item);
	if (link.isEmpty()) {
		return;
	}
	const auto weak = base::make_weak(controller);
	const auto itemId = item->fullId();
	menu->addAction(QString::fromUtf8("Скачать по ссылке"), [=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		strong->show(Box(ChooseBox, link, [=](Choice choice) {
			const auto window = weak.get();
			const auto target = window
				? window->session().data().message(itemId)
				: nullptr;
			if (!target) {
				return;
			}
			auto action = Api::SendAction(target->history());
			action.replyTo = FullReplyTo{
				.messageId = itemId,
				.topicRootId = target->topicRootId(),
			};
			Download(window, action, link, choice);
		}, nullptr));
	}, &st::menuIconDownload);
}

bool OfferSendDownload(
		not_null<Window::SessionController*> controller,
		const Api::SendAction &action,
		const QString &text,
		Fn<void()> sendAsText,
		Fn<void()> clearField) {
	const auto trimmed = text.trimmed();
	if (SendingAsText
		|| !Settings::Bool(QString::fromLatin1(kEnabledKey))
		|| trimmed.isEmpty()
		|| trimmed.contains(QRegularExpression(u"\\s"_q))) {
		return false;
	}
	const auto link = Normalized(trimmed);
	if (link.isEmpty()) {
		return false;
	}
	const auto weak = base::make_weak(controller);
	controller->show(Box(ChooseBox, link, [=](Choice choice) {
		if (const auto window = weak.get()) {
			clearField();
			Download(window, action, link, choice);
		}
	}, sendAsText));
	return true;
}

} // namespace Mokagram::Saveinator
