#include "mokagram/update/mokagram_update.h"
#include "mokagram/lang/mokagram_lang.h"

#include "core/application.h"
#include "mokagram/settings/mokagram_settings.h"
#include "settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/show.h"
#include "window/window_controller.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <QtCore/QDir>
#include <QtCore/QProcessEnvironment>

#ifndef _WIN32
#include <lzma.h>
#endif
#include <openssl/evp.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <cstdio>
#include <limits>
#include <memory>
#include <optional>

namespace Mokagram::Update {
namespace {

constexpr auto kBaseUrl
	= "https://up.moka.wtf/";
constexpr auto kManifestName = "update.json";
constexpr auto kPlatform = "linux-x64";
constexpr auto kFirstCheckDelay = 15 * 1000;
constexpr auto kCheckInterval = 6 * 60 * 60 * 1000;
constexpr auto kManifestTimeout = 15 * 1000;
constexpr auto kDecodeChunk = 1024 * 1024;
constexpr auto kAutoKey = "update.auto";
constexpr auto kCheckKey = "update.check";
constexpr auto kSkippedKey = "update.skipped";

// Public half of the mokaGram update key. The private half never leaves
// the publisher machine: ~/.config/mokagram/update-key.pem (chmod 600).
constexpr uchar kPublicKey[32] = {
	0x60, 0x54, 0xcc, 0x0f, 0xbd, 0x46, 0xda, 0xbc,
	0xbc, 0x0f, 0xf1, 0x7c, 0x30, 0xa9, 0x0b, 0xe6,
	0xfe, 0x6f, 0xbf, 0x22, 0x8e, 0xe9, 0x55, 0x25,
	0xde, 0x0d, 0xca, 0x1c, 0x3b, 0x2f, 0x3e, 0xeb,
};

// Limits: a lying server must not be able to fill memory or disk.
constexpr auto kMaxManifestBytes = qint64(64 * 1024);
constexpr auto kMaxPackedBytes = qint64(400) * 1024 * 1024;
constexpr auto kMaxUnpackedBytes = qint64(1536) * 1024 * 1024;
constexpr auto kDownloadTimeout = 60 * 1000;

struct Manifest {
	int version = 0;
	QString title;
	QString notes;
	QString file;
	qint64 size = 0;
	QByteArray sha256;
	QByteArray signature;
};

enum class Stage {
	Idle,
	Checking,
	Downloading,
	Installing,
};

struct State {
	Stage stage = Stage::Idle;
	int installed = 0;
};

[[nodiscard]] State &Data() {
	static auto result = State();
	return result;
}

[[nodiscard]] QNetworkAccessManager *Manager() {
	static const auto result = new QNetworkAccessManager();
	return result;
}

[[nodiscard]] std::shared_ptr<Ui::Show> ActiveShow() {
	if (const auto window = Core::App().activePrimaryWindow()) {
		return window->uiShow();
	}
	return nullptr;
}

void Toast(const QString &text) {
	if (const auto show = ActiveShow()) {
		show->showToast(text);
	}
}

#ifndef _WIN32
// Обновления ставятся только в установленный клиент. Рабочая сборка из
// папки разработки не должна перезаписывать сама себя.
[[nodiscard]] bool UpdatesAllowed() {
	if (qEnvironmentVariableIsSet("MOKAGRAM_NO_UPDATE")
		|| qEnvironmentVariableIsSet("MOKAGRAM_DEBUG_CMD")) {
		return false;
	}
	const auto dir = QDir::cleanPath(cExeDir());
	return !dir.contains(u"mokagram-build"_q)
		&& !dir.contains(u"/dev/"_q)
		&& QFileInfo(dir).isWritable();
}

void HardenRequest(QNetworkRequest &request, int timeout) {
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setTransferTimeout(timeout);
	request.setRawHeader("User-Agent", "mokaGram-updater");
}

[[nodiscard]] QString TargetPath() {
	return cExeDir() + cExeName();
}

[[nodiscard]] QString TemporaryPath() {
	return cExeDir() + u".mokagram-update"_q;
}

[[nodiscard]] QByteArray SignedMessage(const Manifest &manifest) {
	return "mokagram-update\n"
		+ QByteArray(kPlatform) + '\n'
		+ QByteArray::number(manifest.version) + '\n'
		+ manifest.sha256.toHex() + '\n'
		+ QByteArray::number(manifest.size) + '\n';
}

#endif // !_WIN32

[[nodiscard]] bool Verify(
		const QByteArray &message,
		const QByteArray &signature) {
	const auto key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(
		EVP_PKEY_new_raw_public_key(
			EVP_PKEY_ED25519,
			nullptr,
			kPublicKey,
			sizeof(kPublicKey)),
		&EVP_PKEY_free);
	const auto context = std::unique_ptr<
		EVP_MD_CTX,
		decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
	if (!key
		|| !context
		|| EVP_DigestVerifyInit(
			context.get(),
			nullptr,
			nullptr,
			nullptr,
			key.get()) != 1) {
		return false;
	}
	return EVP_DigestVerify(
		context.get(),
		reinterpret_cast<const uchar*>(signature.constData()),
		size_t(signature.size()),
		reinterpret_cast<const uchar*>(message.constData()),
		size_t(message.size())) == 1;
}

#ifndef _WIN32
[[nodiscard]] bool SignatureValid(const Manifest &manifest) {
	return Verify(SignedMessage(manifest), manifest.signature);
}

[[nodiscard]] std::optional<Manifest> ParseManifest(const QByteArray &data) {
	const auto document = QJsonDocument::fromJson(data);
	if (!document.isObject()) {
		return std::nullopt;
	}
	const auto root = document.object();
	const auto build = root.value(QLatin1String(kPlatform)).toObject();
	auto result = Manifest{
		.version = root.value(u"version"_q).toInt(),
		.title = root.value(u"title"_q).toString(),
		.notes = root.value(u"notes"_q).toString(),
		.file = build.value(u"file"_q).toString(),
		.size = qint64(build.value(u"size"_q).toDouble()),
		.sha256 = QByteArray::fromHex(
			build.value(u"sha256"_q).toString().toLatin1()),
		.signature = QByteArray::fromBase64(
			build.value(u"signature"_q).toString().toLatin1()),
	};
	static const auto kFileName = QRegularExpression(
		u"^[A-Za-z0-9._-]+$"_q);
	if (result.version <= 0
		|| result.size <= 0
		|| result.size > kMaxPackedBytes
		|| result.sha256.size() != 32
		|| !kFileName.match(result.file).hasMatch()
		|| !SignatureValid(result)) {
		return std::nullopt;
	}
	if (result.title.isEmpty()) {
		result.title = QString::number(result.version);
	}
	return result;
}

[[nodiscard]] bool Unpack(const QByteArray &packed, const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	auto stream = lzma_stream LZMA_STREAM_INIT;
	if (lzma_stream_decoder(
			&stream,
			std::numeric_limits<uint64_t>::max(),
			LZMA_CONCATENATED) != LZMA_OK) {
		return false;
	}
	auto buffer = QByteArray(kDecodeChunk, Qt::Uninitialized);
	stream.next_in = reinterpret_cast<const uint8_t*>(packed.constData());
	stream.avail_in = size_t(packed.size());
	auto result = LZMA_OK;
	auto total = qint64(0);
	auto header = QByteArray();
	while (result == LZMA_OK) {
		stream.next_out = reinterpret_cast<uint8_t*>(buffer.data());
		stream.avail_out = size_t(buffer.size());
		result = lzma_code(&stream, LZMA_FINISH);
		const auto written = buffer.size() - qint64(stream.avail_out);
		if (header.size() < 4) {
			header.append(buffer.constData(), std::min<qint64>(written, 4 - header.size()));
		}
		total += written;
		if ((result != LZMA_OK && result != LZMA_STREAM_END)
			|| total > kMaxUnpackedBytes
			|| file.write(buffer.constData(), written) != written) {
			result = LZMA_DATA_ERROR;
		}
	}
	lzma_end(&stream);
	// Установленный файл должен быть исполняемым ELF.
	const auto elf = (header.size() == 4)
		&& (header[0] == 0x7f) && (header[1] == 'E')
		&& (header[2] == 'L') && (header[3] == 'F');
	const auto flushed = file.flush();
	if (flushed) {
		::fsync(file.handle());
	}
	return (result == LZMA_STREAM_END) && elf && flushed;
}

[[nodiscard]] QString Install(const QByteArray &packed) {
	const auto target = TargetPath();
	const auto temporary = TemporaryPath();
	if (!Unpack(packed, temporary)) {
		QFile::remove(temporary);
		return QString::fromUtf8(Mokagram::Lang::Tr("не удалось распаковать файл обновления."));
	}
	QFile::setPermissions(temporary, QFile(target).permissions()
		| QFileDevice::ExeOwner
		| QFileDevice::ReadOwner
		| QFileDevice::WriteOwner);
	// Прежнюю версию оставляем рядом: если новая не запустится, её можно
	// вернуть, переименовав .prev обратно.
	const auto previous = target + u".prev"_q;
	QFile::remove(previous);
	// Жёсткая ссылка: мгновенно и без копирования сотен мегабайт.
	if (::link(
			QFile::encodeName(target).constData(),
			QFile::encodeName(previous).constData()) != 0) {
		QFile::copy(target, previous);
	}
	if (std::rename(
			QFile::encodeName(temporary).constData(),
			QFile::encodeName(target).constData()) != 0) {
		QFile::remove(temporary);
		return QString::fromUtf8(Mokagram::Lang::Tr("не удалось заменить ")) + target + u"."_q;
	}
	return QString();
}

void ShowRestart(const Manifest &manifest) {
	const auto show = ActiveShow();
	if (!show) {
		return;
	}
	show->showBox(Ui::MakeConfirmBox({
		.text = QString::fromUtf8("mokaGram ")
			+ manifest.title
			+ QString::fromUtf8(
				Mokagram::Lang::Tr(" установлен. Новая версия заработает после перезапуска.")),
		.confirmed = [] {
			cSetRestarting(true);
			Core::Quit();
		},
		.confirmText = QString::fromUtf8(Mokagram::Lang::Tr("Перезапустить")),
		.cancelText = QString::fromUtf8(Mokagram::Lang::Tr("Позже")),
	}));
}

void Download(const Manifest &manifest) {
	auto &state = Data();
	if (state.stage != Stage::Idle) {
		return;
	}
	if (!QFileInfo(cExeDir()).isWritable()) {
		Toast(QString::fromUtf8(Mokagram::Lang::Tr("Нет прав на запись в "))
			+ cExeDir()
			+ QString::fromUtf8(Mokagram::Lang::Tr(", обновите mokaGram вручную.")));
		return;
	}
	state.stage = Stage::Downloading;
	Toast(QString::fromUtf8(Mokagram::Lang::Tr("Скачиваю mokaGram ")) + manifest.title + u"..."_q);
	auto request = QNetworkRequest(
		QUrl(QString::fromLatin1(kBaseUrl) + manifest.file));
	request.setAttribute(
		QNetworkRequest::CacheLoadControlAttribute,
		QNetworkRequest::AlwaysNetwork);
	HardenRequest(request, kDownloadTimeout);
	const auto reply = Manager()->get(request);
	// Сервер не может прислать больше, чем обещано в подписанном манифесте.
	QObject::connect(reply, &QNetworkReply::downloadProgress, [=](
			qint64 received,
			qint64) {
		if (received > manifest.size) {
			reply->abort();
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto packed = (reply->error() == QNetworkReply::NoError)
			? reply->readAll()
			: QByteArray();
		const auto intact = (packed.size() == manifest.size)
			&& (QCryptographicHash::hash(
				packed,
				QCryptographicHash::Sha256) == manifest.sha256);
		if (!intact) {
			Data().stage = Stage::Idle;
			Toast(QString::fromUtf8(
				Mokagram::Lang::Tr("Обновление скачалось с ошибкой, попробуйте позже.")));
			return;
		}
		Data().stage = Stage::Installing;
		crl::async([=] {
			const auto error = Install(packed);
			crl::on_main([=] {
				auto &state = Data();
				state.stage = Stage::Idle;
				if (!error.isEmpty()) {
					Toast(QString::fromUtf8(Mokagram::Lang::Tr("Обновление не установлено: "))
						+ error);
					return;
				}
				state.installed = manifest.version;
				ShowRestart(manifest);
			});
		});
	});
}

void Offer(const Manifest &manifest, bool manual) {
	const auto show = ActiveShow();
	if (!show) {
		return;
	}
	auto text = QString::fromUtf8(Mokagram::Lang::Tr("Вышел mokaGram ")) + manifest.title + u"."_q;
	if (!manifest.notes.isEmpty()) {
		text += u"\n\n"_q + manifest.notes;
	}
	show->showBox(Ui::MakeConfirmBox({
		.text = text,
		.confirmed = [=](Fn<void()> close) {
			close();
			Download(manifest);
		},
		.cancelled = [=](Fn<void()> close) {
			if (!manual) {
				Settings::SetInt(
					QString::fromLatin1(kSkippedKey),
					manifest.version);
			}
			close();
		},
		.confirmText = QString::fromUtf8(Mokagram::Lang::Tr("Обновить")),
		.cancelText = QString::fromUtf8(Mokagram::Lang::Tr("Позже")),
	}));
}

void Check(bool manual) {
	auto &state = Data();
	if (!UpdatesAllowed()) {
		if (manual) {
			Toast(QString::fromUtf8(
				Mokagram::Lang::Tr("В этой сборке обновления выключены.")));
		}
		return;
	}
	if (state.stage != Stage::Idle) {
		if (manual) {
			Toast(QString::fromUtf8(Mokagram::Lang::Tr("Обновление уже скачивается.")));
		}
		return;
	}
	state.stage = Stage::Checking;
	auto request = QNetworkRequest(QUrl(
		QString::fromLatin1(kBaseUrl) + QString::fromLatin1(kManifestName)));
	request.setAttribute(
		QNetworkRequest::CacheLoadControlAttribute,
		QNetworkRequest::AlwaysNetwork);
	HardenRequest(request, kManifestTimeout);
	const auto reply = Manager()->get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		Data().stage = Stage::Idle;
		const auto data = (reply->error() == QNetworkReply::NoError)
			? reply->read(kMaxManifestBytes + 1)
			: QByteArray();
		const auto manifest = (data.isEmpty() || data.size() > kMaxManifestBytes)
			? std::nullopt
			: ParseManifest(data);
		if (!manifest) {
			if (manual) {
				Toast(QString::fromUtf8(Mokagram::Lang::Tr("Не удалось проверить обновления.")));
			}
			return;
		}
		const auto current = std::max(kVersion, Data().installed);
		if (manifest->version <= current) {
			if (manual) {
				Toast((Data().installed > kVersion)
					? QString::fromUtf8(
						Mokagram::Lang::Tr("Обновление уже установлено, перезапустите mokaGram."))
					: QString::fromUtf8(Mokagram::Lang::Tr("У вас последняя версия mokaGram.")));
			}
			return;
		}
		const auto skipped = Settings::Int(QString::fromLatin1(kSkippedKey));
		if (!manual && manifest->version <= skipped) {
			return;
		}
		Offer(*manifest, manual);
	});
}

void AutoCheck() {
	if (Settings::Bool(QString::fromLatin1(kAutoKey))) {
		Check(false);
	}
}

#endif // !_WIN32

} // namespace

bool VerifyDetached(
		const QByteArray &message,
		const QByteArray &signature) {
	return Verify(message, signature);
}

void RegisterSettings() {
#ifdef _WIN32
	return;
#else
	const auto section = QString::fromUtf8(Mokagram::Lang::Tr("Обновления"));
	Settings::Register({
		.key = QString::fromLatin1(kAutoKey),
		.section = section,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Проверять обновления")),
		.description = QString::fromUtf8(
			Mokagram::Lang::Tr("При запуске и раз в 6 часов смотреть, вышла ли новая версия.")),
		.kind = Settings::Kind::Bool,
		.defaultValue = true,
		.category = u"other"_q,
		.sectionIcon = u"update"_q,
		.sectionNote = QString::fromUtf8(
			Mokagram::Lang::Tr("Обновления приходят с up.moka.wtf и ставятся, только если подпись совпала.")),
	});
	Settings::Register({
		.key = QString::fromLatin1(kCheckKey),
		.section = section,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Проверить сейчас")),
		.kind = Settings::Kind::String,
		.defaultValue = QString::fromUtf8(Mokagram::Lang::Tr("Версия "))
			+ QString::fromLatin1(kVersionTitle),
		.category = u"other"_q,
		.sectionIcon = u"update"_q,
		.action = CheckNow,
	});
	Settings::Register({
		.key = QString::fromLatin1(kSkippedKey),
		.section = section,
		.title = QString::fromLatin1(kSkippedKey),
		.kind = Settings::Kind::Int,
		.defaultValue = 0,
		.minValue = 0,
		.maxValue = std::numeric_limits<int>::max(),
		.category = u"other"_q,
		.hidden = true,
	});
#endif
}

void Start() {
#ifdef _WIN32
	return;
#else
	QFile::remove(TemporaryPath());
	if (!UpdatesAllowed()) {
		// Рабочая сборка: ни проверок, ни запросов в сеть.
		return;
	}
	QTimer::singleShot(kFirstCheckDelay, AutoCheck);
	static const auto timer = new QTimer();
	timer->setInterval(kCheckInterval);
	QObject::connect(timer, &QTimer::timeout, AutoCheck);
	timer->start();
#endif
}

void CheckNow() {
#ifdef _WIN32
	return;
#else
	Check(true);
#endif
}

} // namespace Mokagram::Update
