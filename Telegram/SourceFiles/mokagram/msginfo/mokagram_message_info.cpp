// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 297a43d6
// Mokagram: окно «Информация о сообщении» (ID, даты, пересылка, вложение, кодировщик голосового).
#include "mokagram/msginfo/mokagram_message_info.h"
#include "mokagram/lang/mokagram_lang.h"
#include "mokagram/msginfo/mokagram_media_fingerprint.h"
#include "mokagram/misc/mokagram_datereg.h"

#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "ui/layers/generic_box.h"
#include "mokagram/plugins/mokagram_plugin_install.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QBuffer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include <vector>

namespace Mokagram::MessageInfo {
namespace {

constexpr auto kFingerprintDownloadLimit = int64(32 * 1024 * 1024);

[[nodiscard]] QString U(const char *text) {
	return QString::fromUtf8(text);
}

[[nodiscard]] QString FullDate(TimeId date) {
	return QDateTime::fromSecsSinceEpoch(date).toString(
		u"d.MM.yyyy HH:mm:ss"_q);
}

[[nodiscard]] QString Size(int64 bytes) {
	if (bytes >= 1024 * 1024) {
		return QString::number(bytes / (1024. * 1024.), 'f', 2) + U(Mokagram::Lang::Tr(" МБ"));
	} else if (bytes >= 1024) {
		return QString::number(bytes / 1024., 'f', 1) + U(Mokagram::Lang::Tr(" КБ"));
	}
	return QString::number(bytes) + U(Mokagram::Lang::Tr(" Б"));
}

// Читает заголовок Ogg Opus: строка кодировщика и частота записи.
// Telegram не хранит устройство записи, поэтому это только догадка по файлу.
[[nodiscard]] QString VoiceHint(const QByteArray &bytes) {
	if (bytes.isEmpty()) {
		return QString();
	}
	auto result = QStringList();
	const auto head = bytes.indexOf("OpusHead");
	if (head >= 0 && bytes.size() >= head + 16) {
		const auto at = reinterpret_cast<const uchar*>(bytes.constData())
			+ head + 12;
		const auto rate = quint32(at[0])
			| (quint32(at[1]) << 8)
			| (quint32(at[2]) << 16)
			| (quint32(at[3]) << 24);
		if (rate > 0) {
			result.push_back(U(Mokagram::Lang::Tr("частота микрофона "))
				+ QString::number(rate) + U(Mokagram::Lang::Tr(" Гц")));
		}
	}
	const auto tags = bytes.indexOf("OpusTags");
	if (tags >= 0 && bytes.size() >= tags + 12) {
		const auto at = reinterpret_cast<const uchar*>(bytes.constData())
			+ tags + 8;
		const auto length = quint32(at[0])
			| (quint32(at[1]) << 8)
			| (quint32(at[2]) << 16)
			| (quint32(at[3]) << 24);
		if (length > 0 && length < 200 && tags + 12 + int(length) <= bytes.size()) {
			result.push_back(U(Mokagram::Lang::Tr("кодировщик "))
				+ QString::fromUtf8(bytes.mid(tags + 12, length)));
		}
	}
	return result.join(u", "_q);
}

struct Row {
	QString label;
	QString value;
};

[[nodiscard]] std::vector<Row> Collect(not_null<HistoryItem*> item) {
	auto rows = std::vector<Row>();
	const auto add = [&](const QString &label, const QString &value) {
		if (!value.isEmpty()) {
			rows.push_back({ label, value });
		}
	};
	add(U(Mokagram::Lang::Tr("ID сообщения")), QString::number(item->id.bare));
	add(U(Mokagram::Lang::Tr("Чат")), item->history()->peer->name()
		+ u" ("_q + QString::number(item->history()->peer->id.value) + u")"_q);
	add(U(Mokagram::Lang::Tr("Отправлено")), FullDate(item->date()));
	if (const auto from = item->from()) {
		add(U(Mokagram::Lang::Tr("Регистрация автора")), Mokagram::Misc::RegdateText(from));
	}
	if (const auto edited = item->Get<HistoryMessageEdited>()) {
		if (edited->date) {
			add(U(Mokagram::Lang::Tr("Изменено")), FullDate(edited->date));
		}
	}
	if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
		auto from = forwarded->originalSender
			? forwarded->originalSender->name()
			: forwarded->originalHiddenSenderInfo
			? forwarded->originalHiddenSenderInfo->name
			: QString();
		if (!forwarded->originalPostAuthor.isEmpty()) {
			from += u" ("_q + forwarded->originalPostAuthor + u")"_q;
		}
		add(U(Mokagram::Lang::Tr("Переслано от")), from);
		if (forwarded->originalDate) {
			add(U(Mokagram::Lang::Tr("Исходное время")), FullDate(forwarded->originalDate));
		}
		if (forwarded->originalId) {
			add(U(Mokagram::Lang::Tr("ID оригинала")), QString::number(forwarded->originalId.bare));
		}
	}
	if (const auto media = item->media()) {
		if (const auto document = media->document()) {
			add(U(Mokagram::Lang::Tr("Файл")), document->filename().isEmpty()
				? document->mimeString()
				: document->filename());
			add(U(Mokagram::Lang::Tr("Тип")), document->mimeString());
			add(U(Mokagram::Lang::Tr("Размер")), Size(document->size));
			if (document->isVoiceMessage()
				|| document->isVideoMessage()
				|| document->isSong()) {
				add(U(Mokagram::Lang::Tr("Длительность")), QString::number(
					document->duration() / 1000.,
					'f',
					1) + U(Mokagram::Lang::Tr(" с")));
			}
			if (document->isVoiceMessage() || document->isVideoMessage()) {
				auto bytes = QByteArray();
				const auto view = document->createMediaView();
				if (view->loaded()) {
					bytes = view->bytes();
				} else if (document->size <= kFingerprintDownloadLimit) {
					// Небольшой файл догружаем для чтения метаданных.
					document->save(item->fullId(), QString());
				}
				if (bytes.isEmpty()) {
					const auto path = document->filepath(true);
					auto file = QFile(path);
					if (!path.isEmpty() && file.open(QIODevice::ReadOnly)) {
						bytes = file.read(256 * 1024);
					}
				}
				auto platform = QString();
				if (document->isVoiceMessage()) {
					platform = VoicePlatform(bytes);
					add(U(Mokagram::Lang::Tr("Голосовое (по заголовку файла)")), VoiceHint(bytes));
				} else {
					auto file = QFile(document->filepath(true));
					if (file.open(QIODevice::ReadOnly)) platform = RoundVideoPlatform(file);
					else if (view->loaded()) {
						auto buffer = QBuffer(&bytes);
						buffer.open(QIODevice::ReadOnly);
						platform = RoundVideoPlatform(buffer);
					}
				}
				add(U(Mokagram::Lang::Tr("Отправлено с… (предположение)")), platform.isEmpty()
					? view->loaded() ? U(Mokagram::Lang::Tr("Не удалось определить"))
					: document->size <= kFingerprintDownloadLimit
					? U(Mokagram::Lang::Tr("Скачиваю файл, окно обновится само…"))
					: U(Mokagram::Lang::Tr("Скачайте файл для определения"))
					: platform);
			}
		} else if (const auto photo = media->photo()) {
			add(U(Mokagram::Lang::Tr("Фото")), QString::number(photo->id));
		}
	}
	add(U(Mokagram::Lang::Tr("Просмотры")), item->hasViews()
		? QString::number(item->viewsCount())
		: QString());
	return rows;
}

} // namespace

void Show(
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	const auto rows = Collect(item);
	// Подписка живёт вместе с окном, закрытие отменяет обновление.
	const auto pending = std::make_shared<rpl::lifetime>();
	if (const auto media = item->media()) {
		if (const auto document = media->document()) {
			if ((document->isVoiceMessage() || document->isVideoMessage())
				&& document->size <= kFingerprintDownloadLimit) {
				const auto view = document->createMediaView();
				if (!view->loaded()) {
					const auto weak = base::make_weak(controller.get());
					const auto id = item->fullId();
					document->session().downloaderTaskFinished(
					) | rpl::filter([=] {
						return view->loaded(true);
					}) | rpl::take(1) | rpl::on_next([=] {
						const auto strong = weak.get();
						if (!strong) {
							return;
						}
						if (const auto again = strong->session().data().message(id)) {
							strong->hideLayer();
							Show(strong, again);
						}
					}, *pending);
				}
			}
		}
	}
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->lifetime().add([=] { pending->destroy(); });
		Plugins::RoundedWindow(box);
		const auto content = box->verticalLayout();
		const auto header = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(10, 10, 10, 0));
		header->resize(header->width(), 40);
		const auto close = Plugins::MakeHeaderButton(
			header,
			true,
			[=] { box->closeBox(); });
		const auto title = Ui::CreateChild<Ui::FlatLabel>(
			header,
			U(Mokagram::Lang::Tr("Информация о сообщении")),
			st::boxTitle);
		header->widthValue() | rpl::on_next([=](int width) {
			close->moveToLeft(0, 0);
			title->moveToLeft((width - title->width()) / 2, 6);
		}, header->lifetime());
		Ui::AddSkip(content, 8);

		// Карточка со строками, как в окне плагинов.
		const auto card = content->add(
			object_ptr<Ui::VerticalLayout>(content),
			QMargins(14, 4, 14, 0));
		auto text = QString();
		for (const auto &row : rows) {
			text += row.label + U(": ") + row.value + u"\n"_q;
			card->add(
				object_ptr<Ui::FlatLabel>(
					card,
					row.label,
					st::boxDividerLabel),
				QMargins(16, 10, 16, 0));
			card->add(
				object_ptr<Ui::FlatLabel>(
					card,
					row.value,
					st::boxLabel),
				QMargins(16, 2, 16, 0));
		}
		Ui::AddSkip(card, 12);
		card->paintOn([=](QPainter &p) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBg);
			p.drawRoundedRect(card->rect(), 22, 22);
		});

		static const auto stCopy = [] {
			auto result = st::defaultActiveButton;
			result.height = 44;
			result.textTop = 12;
			result.radius = 22;
			result.fullRadius = true;
			return result;
		}();
		const auto row = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(14, 12, 14, 14));
		row->resize(row->width(), stCopy.height);
		const auto copy = Ui::CreateChild<Ui::RoundButton>(
			row,
			rpl::single(U(Mokagram::Lang::Tr("Копировать"))),
			stCopy);
		row->widthValue() | rpl::on_next([=](int width) {
			copy->setFullWidth(width);
			copy->moveToLeft(0, 0);
		}, row->lifetime());
		copy->setClickedCallback([=] {
			QGuiApplication::clipboard()->setText(text.trimmed());
			controller->showToast(U(Mokagram::Lang::Tr("Скопировано")));
		});
	}));
}

} // namespace Mokagram::MessageInfo
