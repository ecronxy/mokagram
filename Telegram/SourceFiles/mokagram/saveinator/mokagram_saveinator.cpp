// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 4af056ac
// Mokagram: скачивание медиа по ссылке из сообщения через API Saveinator.
#include "mokagram/saveinator/mokagram_saveinator.h"
#include "mokagram/lang/mokagram_lang.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/flat_map.h"
#include "base/event_filter.h"
#include "base/flat_set.h"
#include "base/weak_ptr.h"
#include "data/data_chat_participant_status.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_web_page.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/controls/history_view_webpage_processor.h"
#include "main/main_session.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/chat/attach/attach_send_files_way.h"
#include "mokagram/settings/mokagram_settings.h"
#include "mokagram/ui/mokagram_box_parts.h"
#include "ui/effects/animations.h"
#include "ui/layers/generic_box.h"
#include "ui/image/image.h"
#include "ui/painter.h"
#include "ui/text/format_values.h"
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
#include <QtGui/QImage>
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
constexpr auto kSpotifyPreviewKey = "saveinator.spotify_preview";
constexpr auto kTranslateKey = "saveinator.translate_lang";
constexpr auto kApiUrl = "https://saveinator-hooks.xdshka.party";
constexpr auto kApiKey = "SmPzzuTM6en_vn7AtgUKcLvB_ObB7QqafyGuBGuxl1E";
constexpr auto kLimit = 10;
constexpr auto kTimeout = 5 * 60 * 1000;
constexpr auto kUserAgent = "Saveinator-mokaGram/1.0";
constexpr auto kPollDelay = 2 * 1000;
constexpr auto kMaxPollFailures = 3;

// Первые проверки частые: короткие ролики готовятся за секунду-две.
[[nodiscard]] int PollDelay(int polls) {
	return (polls < 3) ? 700 : (polls < 8) ? 1200 : kPollDelay;
}
constexpr auto kRequestTimeout = 30 * 1000;
constexpr auto kMaxJsonSize = 64 * 1024;
constexpr auto kMaxFileSize = qint64(2) * 1024 * 1024 * 1024;
constexpr auto kMaxCoverSize = qint64(5) * 1024 * 1024;

constexpr auto kMediaValues = std::array{
	"auto",
	"video",
	"audio",
	"photos",
	"article",
};
constexpr auto kArticleMedia = 4;
constexpr auto kHeightValues = std::array{ 0, 1080, 720, 480, 360 };

struct Choice {
	QString media;
	int maxHeight = 0;
	bool preview = false;
	QString translate;
};

[[nodiscard]] QStringList MediaTitles() {
	return {
		QString::fromUtf8(Mokagram::Lang::Tr("Всё, что есть")),
		QString::fromUtf8(Mokagram::Lang::Tr("Только видео")),
		QString::fromUtf8(Mokagram::Lang::Tr("Только звук")),
		QString::fromUtf8(Mokagram::Lang::Tr("Только фото")),
		QString::fromUtf8(Mokagram::Lang::Tr("Только статья")),
	};
}

[[nodiscard]] QStringList QualityTitles() {
	return {
		QString::fromUtf8(Mokagram::Lang::Tr("Лучшее")),
		u"1080p"_q,
		u"720p"_q,
		u"480p"_q,
		u"360p"_q,
	};
}

[[nodiscard]] bool MediaHasVideo(int media) {
	return (media == 0) || (media == 1);
}

// Текст есть только у «Всё» (пост X) и у статьи: перевод имеет смысл для них.
[[nodiscard]] bool MediaHasText(int media) {
	return (media == 0) || (media == kArticleMedia);
}

// Перевод на один из основных языков, независимо от языка интерфейса.
// Индекс 0 — без перевода.
constexpr auto kTranslateValues = std::array{ "", "ru", "en" };

[[nodiscard]] QStringList TranslateTitles() {
	return {
		QString::fromUtf8(Mokagram::Lang::Tr("Оригинал")),
		u"Русский"_q,
		u"English"_q,
	};
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
	QString cover;
	QString coverCaption;
	QPointer<QNetworkReply> reply;
	QPointer<QTimer> deadline;
	int polls = 0;
	int pollFailures = 0;
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
		return QString::fromUtf8(Mokagram::Lang::Tr("Сервис скачивания занят. Попробуйте чуть позже."));
	} else if (code == u"unsupported_url"_q) {
		return QString::fromUtf8(Mokagram::Lang::Tr("Эта ссылка пока не поддерживается."));
	} else if (code == u"invalid_media"_q) {
		return QString::fromUtf8(
			Mokagram::Lang::Tr("Сервер скачивания пока не поддерживает этот формат."));
	} else if (code == u"no_media"_q) {
		return QString::fromUtf8(Mokagram::Lang::Tr("Медиа для скачивания не найдено."));
	} else if (code == u"too_large"_q) {
		return QString::fromUtf8(
			Mokagram::Lang::Tr("Медиафайл превышает ограничение размера на сервере."));
	} else if (code == u"timeout"_q) {
		return QString::fromUtf8(
			Mokagram::Lang::Tr("Скачивание заняло слишком много времени. Попробуйте позже."));
	} else if (code == u"not_found"_q) {
		return QString::fromUtf8(
			Mokagram::Lang::Tr("Срок хранения истёк или результат недоступен."));
	}
	return QString::fromUtf8(Mokagram::Lang::Tr("Не удалось скачать медиа по этой ссылке."));
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
		Fn<void(QJsonObject)> done,
		Fn<void()> retry = nullptr) {
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
		} else if (retry
			&& (status == 0 || status >= 500)
			&& ++job->pollFailures <= kMaxPollFailures) {
			// Сеть моргнула или сервер занят: пробуем ещё раз, а не бросаем загрузку.
			retry();
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
	if (!job->paths.isEmpty() || !job->cover.isEmpty()) {
		const auto error = Data::AnyFileRestrictionError(peer);
		if (error || !Data::CanSendAnyOf(
				peer,
				Data::FilesSendRestrictions(),
				false)) {
			if (error) {
				Data::ShowSendErrorToast(controller, peer, error);
			} else {
				controller->showToast(QString::fromUtf8(
					Mokagram::Lang::Tr("В этот чат нельзя отправлять файлы.")));
			}
			return;
		}
	}
	if (!job->cover.isEmpty()) {
		// Карточка релиза идёт первой, как в боте: обложка с названием и ссылкой.
		auto cover = Storage::PrepareMediaList(
			QStringList{ job->cover },
			st::sendMediaPreviewSize,
			history->session().premium());
		if (cover.error == Ui::PreparedList::Error::None
			&& cover.files.size() == 1) {
			cover.files.front().caption = TextWithTags{
				job->coverCaption.isEmpty()
					? job->link
					: (job->coverCaption + '\n' + job->link),
			};
			api.sendFiles(
				std::move(cover),
				SendMediaType::Photo,
				nullptr,
				*job->action);
		}
	}
	if (!job->paths.isEmpty()) {
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

[[nodiscard]] QNetworkRequest PlainRequest(const QUrl &url) {
	auto result = QNetworkRequest(url);
	result.setRawHeader("User-Agent", kUserAgent);
	result.setTransferTimeout(kRequestTimeout);
	return result;
}

[[nodiscard]] bool TrustedCover(const QUrl &url) {
	const auto host = url.host().toLower();
	return url.isValid()
		&& (url.scheme() == u"https"_q)
		&& (host.endsWith(u".spotifycdn.com"_q) || host.endsWith(u".scdn.co"_q));
}

// oEmbed отдаёт обложку 300px; тот же снимок 640px отличается только префиксом.
[[nodiscard]] QUrl LargeCover(QUrl url) {
	url.setPath(url.path().replace(
		u"/ab67616d00001e02"_q,
		u"/ab67616d0000b273"_q));
	return url;
}

void FetchCover(
		const std::shared_ptr<Job> &job,
		const QUrl &url,
		const QString &title) {
	const auto reply = Manager()->get(PlainRequest(url));
	job->reply = reply;
	QObject::connect(reply, &QNetworkReply::downloadProgress, [=](
			qint64 received,
			qint64 total) {
		if (received > kMaxCoverSize || total > kMaxCoverSize) {
			reply->abort();
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		if (job->finished || job->reply != reply) {
			return;
		}
		job->reply = nullptr;
		const auto bytes = reply->read(kMaxCoverSize + 1);
		const auto type = reply->header(
			QNetworkRequest::ContentTypeHeader).toString();
		const auto extension = ExtensionFor(type.section(';', 0, 0).trimmed());
		const auto path = job->directory + u"/cover."_q + extension;
		if (reply->error() == QNetworkReply::NoError
			&& bytes.size() <= kMaxCoverSize
			&& !extension.isEmpty()
			&& !QImage::fromData(bytes).isNull()) {
			auto file = QFile(path);
			if (file.open(QIODevice::WriteOnly)
				&& file.write(bytes) == bytes.size()) {
				job->cover = path;
				job->coverCaption = title;
			}
		}
		// Без обложки треки всё равно уходят в чат.
		Deliver(job);
	});
}

void FetchPreview(const std::shared_ptr<Job> &job) {
	const auto url = QUrl(
		u"https://open.spotify.com/oembed?url="_q
		+ QString::fromLatin1(QUrl::toPercentEncoding(job->link)));
	const auto reply = Manager()->get(PlainRequest(url));
	job->reply = reply;
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		if (job->finished || job->reply != reply) {
			return;
		}
		job->reply = nullptr;
		const auto object = QJsonDocument::fromJson(
			reply->read(kMaxJsonSize)).object();
		const auto image = QUrl(object.value(u"thumbnail_url"_q).toString());
		if (reply->error() != QNetworkReply::NoError || !TrustedCover(image)) {
			Deliver(job);
			return;
		}
		FetchCover(
			job,
			LargeCover(image),
			object.value(u"title"_q).toString().trimmed());
	});
}

void DownloadNext(const std::shared_ptr<Job> &job) {
	const auto index = job->next++;
	if (index == int(job->files.size())) {
		if (job->choice.preview) {
			FetchPreview(job);
		} else {
			Deliver(job);
		}
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
		const auto poll = std::make_shared<Fn<void()>>();
		*poll = [=] {
			if (job->finished) {
				return;
			}
			const auto path = u"/api/v1/downloads/"_q + job->id;
			SendJson(job, Manager()->get(MakeRequest(job, path)), [=](
					QJsonObject object) {
				job->pollFailures = 0;
				Poll(job, object);
			}, [=] {
				QTimer::singleShot(1500, [=] { (*poll)(); });
			});
		};
		QTimer::singleShot(PollDelay(job->polls++), [=] { (*poll)(); });
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
			QString::fromUtf8(Mokagram::Lang::Tr("Эта ссылка уже скачивается.")));
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
		Mokagram::Lang::Tr("Скачиваю… Отправлю в чат, как только будет готово.")));

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
	if (!choice.translate.isEmpty()) {
		fields.insert(u"translate"_q, choice.translate);
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
		return QString::fromUtf8(Mokagram::Lang::Tr("Яндекс Музыка"));
	}
	return QString();
}

// Что придёт в чат при выбранном формате: подсказка под переключателем.
[[nodiscard]] QString FormatHint(int media) {
	switch (media) {
	case 0: return QString::fromUtf8(Mokagram::Lang::Tr("Видео, фото и звук — всё, что найдётся в посте."));
	case 1: return QString::fromUtf8(Mokagram::Lang::Tr("Только видео, без отдельных фото."));
	case 2: return QString::fromUtf8(Mokagram::Lang::Tr("Звуковая дорожка придёт аудиофайлом."));
	case 3: return QString::fromUtf8(Mokagram::Lang::Tr("Только картинки из поста."));
	case kArticleMedia: return QString::fromUtf8(Mokagram::Lang::Tr("Пост или тред соберётся в статью Telegraph."));
	}
	return QString();
}

class Segments final : public Ui::RpWidget {
public:
	Segments(QWidget *parent, QStringList titles, int active)
	: RpWidget(parent)
	, _titles(std::move(titles))
	, _active(std::clamp(active, 0, int(_titles.size()) - 1))
	, _shown(_active.current()) {
		setCursor(style::cur_pointer);
		setMouseTracking(true);
		_active.changes() | rpl::on_next([=](int index) {
			_slide.start(
				[=] { update(); },
				_shown,
				index,
				kSlideDuration,
				anim::easeOutCubic);
			_shown = index;
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
		const auto insets = QMarginsF(kInset, kInset, kInset, kInset);
		if (_hovered >= 0 && _hovered != _active.current()) {
			const auto hover = segment(_hovered).marginsRemoved(insets);
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(hover, hover.height() / 2., hover.height() / 2.);
		}
		const auto active = segment(
			_slide.value(_active.current())).marginsRemoved(insets);
		p.setBrush(st::activeButtonBg);
		p.drawRoundedRect(active, active.height() / 2., active.height() / 2.);
		for (auto i = 0; i != int(_titles.size()); ++i) {
			const auto selected = (i == _active.current());
			p.setFont(selected ? st::semiboldFont : st::normalFont);
			p.setPen(selected ? st::activeButtonFg : st::windowFg);
			p.drawText(segment(i), Qt::AlignCenter, _titles[i]);
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override {
		setHovered(indexAt(e->pos()));
	}

	void leaveEventHook(QEvent *e) override {
		setHovered(-1);
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
	static constexpr auto kSlideDuration = crl::time(180);

	void setHovered(int index) {
		if (_hovered != index) {
			_hovered = index;
			update();
		}
	}

	[[nodiscard]] QRectF segment(float64 index) const {
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
	int _shown = 0;
	Ui::Animations::Simple _slide;
	int _hovered = -1;
	int _pressed = -1;

};

// Карточка ссылки перед скачиванием по превью Telegram, пока грузится — скелетон.
class PreviewCard final : public Ui::RpWidget {
public:
	PreviewCard(
		QWidget *parent,
		not_null<Main::Session*> session,
		const QString &link,
		const QString &platform,
		const QString &shown,
		WebPageData *known)
	: RpWidget(parent)
	, _name(platform.isEmpty() ? QUrl(link).host() : platform)
	, _shown(shown)
	, _footer(platform.isEmpty() ? shown : (platform + u" · "_q + shown)) {
		if (known && !known->failed) {
			setPage(known);
		} else {
			_resolver = std::make_unique<HistoryView::Controls::WebpageResolver>(
				session);
			_resolver->resolved() | rpl::filter(
				rpl::mappers::_1 == link
			) | rpl::on_next([=] {
				const auto page = _resolver->lookup(link);
				setPage(page ? *page : nullptr);
			}, lifetime());
			_resolver->request(link);
		}
		session->data().webPageUpdates(
		) | rpl::filter([=](not_null<WebPageData*> page) {
			return (_page == page.get());
		}) | rpl::on_next([=] {
			setPage(_page);
		}, lifetime());
		session->downloaderTaskFinished(
		) | rpl::filter([=] {
			return _photo || _document;
		}) | rpl::on_next([=] {
			update();
		}, lifetime());
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return kHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBg);
		p.drawRoundedRect(rect(), kRadius, kRadius);

		const auto thumb = QRect(kPadding, kPadding, kThumb, kThumb);
		const auto withThumb = _loading || _photo || _document;
		if (withThumb) {
			paintThumb(p, thumb);
		}
		const auto left = withThumb ? (thumb.right() + 1 + kPadding) : kSide;
		const auto width = this->width() - left - kSide;
		const auto line = st::normalFont->height;
		const auto footer = _failed ? QString() : _footer;
		const auto lines = footer.isEmpty() ? 2 : 3;
		const auto top = (height() - lines * line - (lines - 1) * kLineSkip) / 2;
		if (_loading) {
			p.setBrush(st::windowBgOver);
			const auto bar = [&](int y, int w) {
				const auto h = line - 6;
				p.drawRoundedRect(left, y + 3, w, h, h / 2., h / 2.);
			};
			bar(top, width * 2 / 3);
			bar(top + line + kLineSkip, width * 5 / 6);
		} else {
			p.setFont(st::semiboldFont);
			p.setPen(st::windowFg);
			p.drawText(
				left,
				top + st::semiboldFont->ascent,
				st::semiboldFont->elided(_title, width));
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				left,
				top + line + kLineSkip + st::normalFont->ascent,
				st::normalFont->elided(_description, width));
		}
		if (!footer.isEmpty()) {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				left,
				top + 2 * (line + kLineSkip) + st::normalFont->ascent,
				st::normalFont->elided(footer, width));
		}
	}

private:
	static constexpr auto kHeight = 76;
	static constexpr auto kPadding = 8;
	static constexpr auto kSide = 14;
	static constexpr auto kThumb = kHeight - 2 * kPadding;
	static constexpr auto kRadius = 16.;
	static constexpr auto kThumbRadius = 11.;
	static constexpr auto kLineSkip = 1;

	void setPage(WebPageData *page) {
		_page = page;
		if (page && page->pendingTill > 0) {
			// Сервер ещё собирает превью и пришлёт его обновлением.
			update();
			return;
		}
		_loading = false;
		_failed = !page || page->failed;
		if (_failed) {
			_title = _name;
			_description = _shown;
			_photo = nullptr;
			_document = nullptr;
			update();
			return;
		}
		_title = !page->title.isEmpty()
			? page->title
			: !page->author.isEmpty()
			? page->author
			: page->displayedSiteName();
		_description = page->description.text.simplified();
		if (_description.isEmpty()) {
			_description = page->author;
		}
		_duration = page->duration;
		_photo = nullptr;
		_document = nullptr;
		const auto origin = Data::FileOriginWebPage{ page->url };
		if (page->photo) {
			_photo = page->photo->createMediaView();
			_photo->wanted(Data::PhotoSize::Thumbnail, origin);
		} else if (page->document && page->document->hasThumbnail()) {
			_document = page->document->createMediaView();
			_document->thumbnailWanted(origin);
		}
		update();
	}

	[[nodiscard]] Image *image() const {
		if (_photo) {
			if (const auto large = _photo->image(Data::PhotoSize::Thumbnail)) {
				return large;
			} else if (const auto small = _photo->image(Data::PhotoSize::Small)) {
				return small;
			}
			return _photo->thumbnailInline();
		} else if (_document) {
			if (const auto thumbnail = _document->thumbnail()) {
				return thumbnail;
			}
			return _document->thumbnailInline();
		}
		return nullptr;
	}

	void paintThumb(QPainter &p, QRect thumb) {
		auto path = QPainterPath();
		path.addRoundedRect(QRectF(thumb), kThumbRadius, kThumbRadius);
		const auto source = image();
		if (!source) {
			p.setBrush(st::windowBgOver);
			p.drawPath(path);
			return;
		}
		const auto ratio = style::DevicePixelRatio();
		const auto original = source->original();
		if (_cachedFrom != original.cacheKey()) {
			_cachedFrom = original.cacheKey();
			_cached = original.scaled(
				thumb.size() * ratio,
				Qt::KeepAspectRatioByExpanding,
				Qt::SmoothTransformation);
			_cached = _cached.copy(
				(_cached.width() - thumb.width() * ratio) / 2,
				(_cached.height() - thumb.height() * ratio) / 2,
				thumb.width() * ratio,
				thumb.height() * ratio);
			_cached.setDevicePixelRatio(ratio);
		}
		p.save();
		p.setClipPath(path);
		p.drawImage(thumb, _cached);
		p.restore();
		if (_duration > 0) {
			const auto text = Ui::FormatDurationText(_duration);
			const auto font = st::normalFont;
			const auto w = font->width(text) + 8;
			const auto h = font->height;
			const auto badge = QRect(
				thumb.right() + 1 - w - 4,
				thumb.bottom() + 1 - h - 4,
				w,
				h);
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0, 0, 0, 140));
			p.drawRoundedRect(badge, h / 2., h / 2.);
			p.setFont(font);
			p.setPen(QColor(255, 255, 255));
			p.drawText(badge, Qt::AlignCenter, text);
		}
	}

	const QString _name;
	const QString _shown;
	const QString _footer;
	std::unique_ptr<HistoryView::Controls::WebpageResolver> _resolver;
	WebPageData *_page = nullptr;
	std::shared_ptr<Data::PhotoMedia> _photo;
	std::shared_ptr<Data::DocumentMedia> _document;
	QString _title;
	QString _description;
	int _duration = 0;
	QImage _cached;
	qint64 _cachedFrom = 0;
	bool _loading = true;
	bool _failed = false;

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
		not_null<Main::Session*> session,
		const QString &link,
		WebPageData *known,
		Fn<void(Choice)> done,
		Fn<void()> sendAsText) {
	BoxParts::RoundLayer(box);
	box->setStyle(BoxParts::PillButtonsBox());
	box->setTitle(rpl::single(QString::fromUtf8(Mokagram::Lang::Tr("Скачать по ссылке"))));
	const auto content = box->verticalLayout();

	const auto url = QUrl(link);
	auto host = url.host().toLower();
	if (host.startsWith(u"www."_q)) {
		host = host.mid(4);
	}
	const auto platform = PlatformName(host);
	const auto shown = host + url.path();
	content->add(
		object_ptr<PreviewCard>(
			content,
			session,
			link,
			platform,
			shown,
			known),
		QMargins(BoxParts::kOuterSide, 0, BoxParts::kOuterSide, 0));

	const auto videoOnly = (platform == u"YouTube"_q)
		|| (platform == u"Twitch"_q)
		|| (platform == u"VK"_q);
	// Статью Telegraph из треда сервер собирает только для X.
	const auto values = IsMusicLink(link)
		? std::vector<int>{ 2 }
		: videoOnly
		? std::vector<int>{ 1, 2 }
		: (platform == u"X"_q)
		? std::vector<int>{ 0, 1, 2, 3, kArticleMedia }
		: std::vector<int>{ 0, 1, 2, 3 };
	auto titles = QStringList();
	const auto names = std::array{
		Mokagram::Lang::Tr("Всё"),
		Mokagram::Lang::Tr("Видео"),
		Mokagram::Lang::Tr("Звук"),
		Mokagram::Lang::Tr("Фото"),
		Mokagram::Lang::Tr("Статья"),
	};
	for (const auto value : values) {
		titles.push_back(QString::fromUtf8(names[value]));
	}
	const auto saved = Settings::Int(QString::fromLatin1(kMediaKey));
	const auto i = ranges::find(values, saved);
	const auto media = (values.size() > 1)
		? AddSegments(
			content,
			QString::fromUtf8(Mokagram::Lang::Tr("Формат")),
			titles,
			(i != values.end()) ? int(i - values.begin()) : 0).get()
		: nullptr;
	const auto mediaValue = [=] {
		return values[media ? media->current() : 0];
	};
	if (media) {
		static const auto hintSt = [] {
			auto result = st::boxDividerLabel;
			result.minWidth = 1;
			return result;
		}();
		content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				media->value() | rpl::map([=](int index) {
					return FormatHint(values[index]);
				}),
				hintSt),
			QMargins(
				BoxParts::kOuterSide + 8,
				8,
				BoxParts::kOuterSide + 8,
				0));
	}

	const auto wrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)));
	const auto quality = AddSegments(
		wrap->entity(),
		QString::fromUtf8(Mokagram::Lang::Tr("Качество")),
		{
			QString::fromUtf8(Mokagram::Lang::Tr("Макс")),
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

	const auto translateWrap = (platform == u"X"_q)
		? content->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				content,
				object_ptr<Ui::VerticalLayout>(content)))
		: nullptr;
	const auto translate = translateWrap
		? AddSegments(
			translateWrap->entity(),
			QString::fromUtf8(Mokagram::Lang::Tr("Перевод")),
			TranslateTitles(),
			Settings::Int(QString::fromLatin1(kTranslateKey))).get()
		: nullptr;
	if (translateWrap) {
		translateWrap->toggleOn(media
			? (media->value() | rpl::map([=](int index) {
				return MediaHasText(values[index]);
			})) | rpl::type_erased
			: rpl::single(MediaHasText(values.front())));
		translateWrap->finishAnimating();
	}

	const auto preview = (platform == u"Spotify"_q)
		? AddSegments(
			content,
			QString::fromUtf8(Mokagram::Lang::Tr("Превью")),
			{
				QString::fromUtf8(Mokagram::Lang::Tr("С обложкой")),
				QString::fromUtf8(Mokagram::Lang::Tr("Только треки")),
			},
			Settings::Bool(QString::fromLatin1(kSpotifyPreviewKey)) ? 0 : 1).get()
		: nullptr;
	Ui::AddSkip(content, BoxParts::kOuterSide);

	box->addButton(rpl::single(QString::fromUtf8(Mokagram::Lang::Tr("Скачать"))), [=] {
		Settings::SetInt(QString::fromLatin1(kMediaKey), mediaValue());
		Settings::SetInt(QString::fromLatin1(kQualityKey), quality->current());
		auto choice = MakeChoice(mediaValue(), quality->current());
		if (translate) {
			const auto index = std::clamp(
				translate->current(),
				0,
				int(kTranslateValues.size()) - 1);
			Settings::SetInt(QString::fromLatin1(kTranslateKey), index);
			if (MediaHasText(mediaValue())) {
				choice.translate = QString::fromLatin1(kTranslateValues[index]);
			}
		}
		if (preview) {
			choice.preview = (preview->current() == 0);
			Settings::SetBool(
				QString::fromLatin1(kSpotifyPreviewKey),
				choice.preview);
		}
		box->closeBox();
		done(choice);
	}, BoxParts::PillActive());
	base::install_event_filter(box, [=](not_null<QEvent*> event) {
		if (event->type() == QEvent::KeyPress) {
			const auto key = static_cast<QKeyEvent*>(event.get())->key();
			if (key == Qt::Key_Enter || key == Qt::Key_Return) {
				box->triggerButton(0);
				return base::EventFilterResult::Cancel;
			}
		}
		return base::EventFilterResult::Continue;
	});
	if (sendAsText) {
		box->addLeftButton(
			rpl::single(QString::fromUtf8(Mokagram::Lang::Tr("Отправить ссылкой"))),
			[=] {
				box->closeBox();
				SendingAsText = true;
				sendAsText();
				SendingAsText = false;
			});
	} else {
		box->addButton(rpl::single(QString::fromUtf8(Mokagram::Lang::Tr("Отмена"))), [=] {
			box->closeBox();
		});
	}
}

} // namespace

void RegisterSettings() {
	const auto section = QString::fromUtf8(Mokagram::Lang::Tr("Скачивание по ссылкам"));
	const auto add = [&](Settings::Entry entry) {
		entry.section = section;
		entry.category = u"chats"_q;
		entry.sectionIcon = u"update"_q;
		entry.sectionNote = QString::fromUtf8(
			Mokagram::Lang::Tr("Нажмите правой кнопкой на сообщение со ссылкой на YouTube, TikTok, X, Instagram, Reddit, Pinterest, VK, Spotify, SoundCloud или Яндекс Музыку: отправьте ссылку или выберите «Скачать по ссылке» в меню сообщения. Клиент спросит формат и сам отправит готовое видео, фото или трек в чат."));
		Settings::Register(std::move(entry));
	};
	add({
		.key = QString::fromLatin1(kEnabledKey),
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Скачивание видео и музыки по ссылкам")),
		.description = QString::fromUtf8(
			Mokagram::Lang::Tr("Пункт «Скачать по ссылке» в меню сообщений.")),
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
	add({
		.key = QString::fromLatin1(kTranslateKey),
		.title = QString::fromLatin1(kTranslateKey),
		.kind = Settings::Kind::Choice,
		.defaultValue = 0,
		.choices = TranslateTitles(),
		.hidden = true,
	});
	add({
		.key = QString::fromLatin1(kSpotifyPreviewKey),
		.title = QString::fromLatin1(kSpotifyPreviewKey),
		.kind = Settings::Kind::Bool,
		.defaultValue = true,
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
	menu->addAction(QString::fromUtf8(Mokagram::Lang::Tr("Скачать по ссылке")), [=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto target = strong->session().data().message(itemId);
		const auto media = target ? target->media() : nullptr;
		const auto known = media ? media->webpage() : nullptr;
		const auto same = known
			&& (Normalized(known->url) == link
				|| Normalized(known->displayUrl) == link);
		strong->show(Box(
			ChooseBox,
			&strong->session(),
			link,
			same ? known : nullptr,
			[=](Choice choice) {
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
			},
			nullptr));
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
	controller->show(Box(ChooseBox, &controller->session(), link, nullptr, [=](Choice choice) {
		if (const auto window = weak.get()) {
			clearField();
			Download(window, action, link, choice);
		}
	}, sendAsText));
	return true;
}

} // namespace Mokagram::Saveinator
