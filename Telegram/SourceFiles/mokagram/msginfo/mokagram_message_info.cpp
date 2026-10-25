// Mokagram: окно «Информация о сообщении» (ID, даты, пересылка, вложение, кодировщик голосового).
#include "mokagram/msginfo/mokagram_message_info.h"

#include "base/unixtime.h"
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
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include <vector>

namespace Mokagram::MessageInfo {
namespace {

[[nodiscard]] QString U(const char *text) {
	return QString::fromUtf8(text);
}

[[nodiscard]] QString FullDate(TimeId date) {
	return QDateTime::fromSecsSinceEpoch(date).toString(
		u"d.MM.yyyy HH:mm:ss"_q);
}

[[nodiscard]] QString Size(int64 bytes) {
	if (bytes >= 1024 * 1024) {
		return QString::number(bytes / (1024. * 1024.), 'f', 2) + U(" МБ");
	} else if (bytes >= 1024) {
		return QString::number(bytes / 1024., 'f', 1) + U(" КБ");
	}
	return QString::number(bytes) + U(" Б");
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
			result.push_back(U("частота микрофона ")
				+ QString::number(rate) + U(" Гц"));
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
			result.push_back(U("кодировщик ")
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
	add(U("ID сообщения"), QString::number(item->id.bare));
	add(U("Чат"), item->history()->peer->name()
		+ u" ("_q + QString::number(item->history()->peer->id.value) + u")"_q);
	add(U("Отправлено"), FullDate(item->date()));
	if (const auto edited = item->Get<HistoryMessageEdited>()) {
		if (edited->date) {
			add(U("Изменено"), FullDate(edited->date));
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
		add(U("Переслано от"), from);
		if (forwarded->originalDate) {
			add(U("Исходное время"), FullDate(forwarded->originalDate));
		}
		if (forwarded->originalId) {
			add(U("ID оригинала"), QString::number(forwarded->originalId.bare));
		}
	}
	if (const auto media = item->media()) {
		if (const auto document = media->document()) {
			add(U("Файл"), document->filename().isEmpty()
				? document->mimeString()
				: document->filename());
			add(U("Тип"), document->mimeString());
			add(U("Размер"), Size(document->size));
			if (document->isVoiceMessage()
				|| document->isVideoMessage()
				|| document->isSong()) {
				add(U("Длительность"), QString::number(
					document->duration() / 1000.,
					'f',
					1) + U(" с"));
			}
			if (document->isVoiceMessage()) {
				auto bytes = QByteArray();
				const auto view = document->createMediaView();
				if (view->loaded()) {
					bytes = view->bytes();
				}
				if (bytes.isEmpty()) {
					const auto path = document->filepath(true);
					auto file = QFile(path);
					if (!path.isEmpty() && file.open(QIODevice::ReadOnly)) {
						bytes = file.read(4096);
					}
				}
				const auto hint = VoiceHint(bytes);
				add(U("Голосовое (по заголовку файла)"), hint.isEmpty()
					? U("скачай голосовое и открой снова")
					: hint);
			}
		} else if (const auto photo = media->photo()) {
			add(U("Фото"), QString::number(photo->id));
		}
	}
	add(U("Просмотры"), item->hasViews()
		? QString::number(item->viewsCount())
		: QString());
	return rows;
}

} // namespace

void Show(
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	const auto rows = Collect(item);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
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
			U("Информация о сообщении"),
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
			rpl::single(U("Копировать")),
			stCopy);
		row->widthValue() | rpl::on_next([=](int width) {
			copy->setFullWidth(width);
			copy->moveToLeft(0, 0);
		}, row->lifetime());
		copy->setClickedCallback([=] {
			QGuiApplication::clipboard()->setText(text.trimmed());
			controller->showToast(U("Скопировано"));
		});
	}));
}

} // namespace Mokagram::MessageInfo
