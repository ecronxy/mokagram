// Mokagram: .show, скачивание видео и картинок по ссылке прямо в клиенте (yt-dlp).
#include "mokagram/downloader/mokagram_downloader.h"

#include "core/application.h"
#include "history/history.h"
#include "main/main_session.h"
#include "mainwidget.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "window/window_session_controller.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Mokagram::Downloader {
namespace {

constexpr auto kReleaseUrl
	= "https://github.com/yt-dlp/yt-dlp/releases/latest/download/";
constexpr auto kBinaryName = "yt-dlp_linux";
constexpr auto kMaxBinaryBytes = qint64(80) * 1024 * 1024;
constexpr auto kRunTimeout = 10 * 60 * 1000;

[[nodiscard]] QString U(const char *text) {
	return QString::fromUtf8(text);
}

[[nodiscard]] QString OwnBinary() {
	return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
		+ u"/yt-dlp"_q;
}

// Свой скачанный бинарник, иначе системный yt-dlp из PATH.
[[nodiscard]] QString FindBinary() {
	const auto own = OwnBinary();
	if (QFileInfo(own).isExecutable()) {
		return own;
	}
	return QStandardPaths::findExecutable(u"yt-dlp"_q);
}

[[nodiscard]] QNetworkAccessManager *Manager() {
	static const auto result = new QNetworkAccessManager();
	return result;
}

[[nodiscard]] QNetworkReply *Get(const QString &url, qint64 limit) {
	auto request = QNetworkRequest(QUrl(url));
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setTransferTimeout(60 * 1000);
	request.setRawHeader("User-Agent", "mokaGram");
	const auto reply = Manager()->get(request);
	QObject::connect(reply, &QNetworkReply::downloadProgress, [=](
			qint64 received,
			qint64) {
		if (received > limit) {
			reply->abort();
		}
	});
	return reply;
}

// Скачивает yt-dlp с GitHub и сверяет хеш с файлом SHA2-256SUMS релиза.
void InstallBinary(
		not_null<Window::SessionController*> controller,
		Fn<void()> done) {
	const auto weak = base::make_weak(controller.get());
	controller->showToast(U("Скачиваю yt-dlp..."));
	const auto sums = Get(
		QString::fromLatin1(kReleaseUrl) + u"SHA2-256SUMS"_q,
		256 * 1024);
	QObject::connect(sums, &QNetworkReply::finished, [=] {
		sums->deleteLater();
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		auto expected = QByteArray();
		for (const auto &line : sums->readAll().split('\n')) {
			const auto parts = line.simplified().split(' ');
			if (parts.size() == 2
				&& parts[1].trimmed() == QByteArray(kBinaryName)) {
				expected = QByteArray::fromHex(parts[0]);
			}
		}
		if (sums->error() != QNetworkReply::NoError
			|| expected.size() != 32) {
			strong->showToast(U("Не удалось получить хеш yt-dlp"));
			return;
		}
		const auto binary = Get(
			QString::fromLatin1(kReleaseUrl) + QString::fromLatin1(kBinaryName),
			kMaxBinaryBytes);
		QObject::connect(binary, &QNetworkReply::finished, [=] {
			binary->deleteLater();
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			const auto bytes = (binary->error() == QNetworkReply::NoError)
				? binary->readAll()
				: QByteArray();
			if (bytes.isEmpty()
				|| QCryptographicHash::hash(
					bytes,
					QCryptographicHash::Sha256) != expected) {
				strong->showToast(U("yt-dlp скачался с ошибкой"));
				return;
			}
			const auto path = OwnBinary();
			QDir().mkpath(QFileInfo(path).absolutePath());
			auto file = QFile(path);
			if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
				|| file.write(bytes) != bytes.size()) {
				strong->showToast(U("Не удалось сохранить yt-dlp"));
				return;
			}
			file.close();
			file.setPermissions(
				QFileDevice::ReadOwner | QFileDevice::WriteOwner
				| QFileDevice::ExeOwner);
			done();
		});
	});
}

[[nodiscard]] QStringList MediaFiles(const QString &folder) {
	auto result = QStringList();
	const auto list = QDir(folder).entryInfoList(
		QDir::Files,
		QDir::Name);
	for (const auto &info : list) {
		const auto ext = info.suffix().toLower();
		if (ext == u"mp4"_q || ext == u"webm"_q || ext == u"mkv"_q
			|| ext == u"mov"_q || ext == u"jpg"_q || ext == u"jpeg"_q
			|| ext == u"png"_q || ext == u"webp"_q || ext == u"gif"_q
			|| ext == u"mp3"_q || ext == u"m4a"_q) {
			result.push_back(info.absoluteFilePath());
		}
	}
	return result;
}

// Понятная причина вместо сырой ошибки yt-dlp: длинный текст в подсказку не влезает.
[[nodiscard]] QString ShortError(const QByteArray &stderrText) {
	auto lines = QString::fromUtf8(stderrText).split('\n', Qt::SkipEmptyParts);
	while (!lines.isEmpty()) {
		const auto line = lines.takeLast().trimmed();
		if (!line.startsWith(u"ERROR:"_q)) {
			continue;
		}
		if (line.contains(u"blocked"_q, Qt::CaseInsensitive)
			|| line.contains(u"region"_q, Qt::CaseInsensitive)
			|| line.contains(u"geo"_q, Qt::CaseInsensitive)) {
			return U("сайт блокирует твой IP или регион");
		}
		if (line.contains(u"unreachable"_q, Qt::CaseInsensitive)
			|| line.contains(u"Failed to establish"_q, Qt::CaseInsensitive)
			|| line.contains(u"timed out"_q, Qt::CaseInsensitive)) {
			return U("сайт не открывается с этого подключения");
		}
		if (line.contains(u"Unsupported URL"_q, Qt::CaseInsensitive)) {
			return U("эта ссылка не поддерживается");
		}
		if (line.contains(u"private"_q, Qt::CaseInsensitive)
			|| line.contains(u"login"_q, Qt::CaseInsensitive)) {
			return U("пост закрыт или нужен вход");
		}
		if (line.contains(u"not found"_q, Qt::CaseInsensitive)
			|| line.contains(u"404"_q)) {
			return U("пост не найден");
		}
		return QString();
	}
	return QString();
}

void Run(
		not_null<Window::SessionController*> controller,
		not_null< ::History*> history,
		const QString &binary,
		const QString &url) {
	const auto folder = QStandardPaths::writableLocation(
		QStandardPaths::CacheLocation)
		+ u"/show/"_q
		+ QString::number(QDateTime::currentMSecsSinceEpoch());
	QDir().mkpath(folder);
	const auto weak = base::make_weak(controller.get());
	const auto historyWeak = base::make_weak(history.get());
	controller->showToast(U("Скачиваю..."));

	const auto process = new QProcess();
	process->setProgram(binary);
	// Ссылка идёт после «--»: её нельзя принять за параметр yt-dlp.
	process->setArguments({
		u"--ignore-config"_q,
		u"--no-playlist"_q,
		u"--no-warnings"_q,
		u"--max-filesize"_q, u"1900M"_q,
		u"-S"_q, u"vcodec:h264,res:1080,ext:mp4:m4a"_q,
		u"--merge-output-format"_q, u"mp4"_q,
		u"-P"_q, folder,
		u"-o"_q, u"%(id)s_%(autonumber)s.%(ext)s"_q,
		u"--"_q, url,
	});
	QObject::connect(process, &QProcess::finished, [=](
			int code,
			QProcess::ExitStatus status) {
		const auto errors = process->readAllStandardError();
		process->deleteLater();
		const auto strong = weak.get();
		const auto target = historyWeak.get();
		if (!strong || !target) {
			return;
		}
		const auto files = MediaFiles(folder);
		if (status != QProcess::NormalExit || code != 0 || files.isEmpty()) {
			const auto reason = ShortError(errors);
			strong->showToast(reason.isEmpty()
				? U("Не получилось скачать по этой ссылке")
				: (U("Не скачалось: ") + reason));
			return;
		}
		strong->content()->sendPaths(target, files);
	});
	QObject::connect(process, &QProcess::errorOccurred, [=](
			QProcess::ProcessError error) {
		if (error != QProcess::FailedToStart) {
			return;
		}
		process->deleteLater();
		if (const auto strong = weak.get()) {
			strong->showToast(U("Не удалось запустить yt-dlp"));
		}
	});
	QTimer::singleShot(kRunTimeout, process, [=] {
		if (process->state() != QProcess::NotRunning) {
			process->kill();
		}
	});
	process->start();
}

} // namespace

QString FindUrl(const QString &text) {
	static const auto re = QRegularExpression(
		u"https?://[^\\s<>\"']+"_q,
		QRegularExpression::CaseInsensitiveOption);
	const auto match = re.match(text);
	return match.hasMatch() ? match.captured(0) : QString();
}

void Show(
		not_null<Window::SessionController*> controller,
		not_null< ::History*> history,
		const QString &url) {
	if (url.isEmpty()) {
		controller->showToast(
			U("Напиши .show и ссылку или ответь командой на сообщение со ссылкой"));
		return;
	}
	const auto binary = FindBinary();
	if (!binary.isEmpty()) {
		Run(controller, history, binary, url);
		return;
	}
	const auto weak = base::make_weak(controller.get());
	const auto historyWeak = base::make_weak(history.get());
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		Ui::ConfirmBox(box, {
			.text = U("Для .show нужен yt-dlp (около 35 МБ). Скачать его "
				"с GitHub, с проверкой хеша?"),
			.confirmed = [=](Fn<void()> close) {
				close();
				const auto strong = weak.get();
				if (!strong) {
					return;
				}
				InstallBinary(strong, [=] {
					const auto strong = weak.get();
					const auto target = historyWeak.get();
					const auto path = FindBinary();
					if (strong && target && !path.isEmpty()) {
						Run(strong, target, path, url);
					}
				});
			},
			.confirmText = U("Скачать"),
		});
	}));
}

} // namespace Mokagram::Downloader
