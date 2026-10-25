#include "mokagram/scam/mokagram_scam.h"

#include "core/application.h"
#include "data/data_peer.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_reply_markup.h"
#include "mokagram/settings/mokagram_settings.h"
#include "styles/style_basic.h"
#include "styles/style_chat.h"
#include "ui/boxes/confirm_box.h"
#include "ui/painter.h"
#include "ui/layers/generic_box.h"
#include "window/window_controller.h"

namespace Mokagram::Scam {
namespace {

namespace S = Mokagram::Settings;

constexpr auto kTelegramServiceId = 777000;
constexpr auto kMarkChar = QChar(0x2063);

[[nodiscard]] QString U(const char *text) {
	return QString::fromUtf8(text);
}

[[nodiscard]] bool ContainsAny(
		const QString &text,
		std::initializer_list<const char*> needles) {
	for (const auto needle : needles) {
		if (text.contains(U(needle), Qt::CaseInsensitive)) {
			return true;
		}
	}
	return false;
}

} // namespace

void RegisterSettings() {
	S::Register({
		.key = u"scam.enabled"_q,
		.section = U("Защита от скама"),
		.title = U("Предупреждать о фейковых офферах"),
		.description = U("Настоящий оффер выглядит как карточка подарка по центру чата. Если то же самое прислал человек обычным сообщением, клиент пометит его и спросит перед нажатием на кнопки."),
		.kind = S::Kind::Bool,
		.defaultValue = true,
		.category = u"privacy"_q,
		.sectionIcon = u"block"_q,
	});
}

namespace {

[[nodiscard]] bool OfferText(const QString &text) {
	return ContainsAny(text, {
		"предложение действует", "offer is valid",
	}) || (ContainsAny(text, { "предлагает вам", "offers you" })
		&& ContainsAny(text, {
			"за подарок", "за коллекционный", "for your gift",
			"for your collectible", " gram",
		}));
}

} // namespace

void MarkText(
		not_null<const HistoryItem*> item,
		TextWithEntities &text) {
	const auto testing = qEnvironmentVariableIsSet("MOKAGRAM_SCAM_TEST");
	if (!S::Bool(u"scam.enabled"_q)
		|| item->isService()
		|| (item->out() && !testing)
		|| item->isPost()
		|| !OfferText(text.text)) {
		return;
	}
	const auto from = item->from();
	if (from->isVerified() || from->id.value == kTelegramServiceId) {
		return;
	}
	text = TextWithEntities{ QString(kMarkChar) + u"\n"_q }.append(std::move(text));
}

bool IsFakeOffer(not_null<const HistoryItem*> item) {
	if (!S::Bool(u"scam.enabled"_q)
		|| item->isService()
		|| item->out()
		|| item->isPost()) {
		return false;
	}
	const auto from = item->from();
	if (from->isVerified() || from->id.value == kTelegramServiceId) {
		return false;
	}
	const auto text = item->originalText().text;
	// Живой оффер Telegram показывает служебная плашка. Обычное сообщение
	// с такой же фразой и таймером это подделка.
	const auto offerText = ContainsAny(text, {
		"предложение действует", "offer is valid",
	}) || (ContainsAny(text, { "предлагает вам", "offers you" })
		&& ContainsAny(text, {
			"за подарок", "за коллекционный", "for your gift",
			"for your collectible", " gram",
		}));
	if (offerText) {
		return true;
	}
	const auto markup = item->inlineReplyMarkup();
	if (!markup) {
		return false;
	}
	using Type = HistoryMessageMarkupButton::Type;
	auto hasLink = false;
	auto actionLabel = false;
	for (const auto &row : markup->data.rows) {
		for (const auto &button : row) {
			if (button.type == Type::Url
				|| button.type == Type::Auth
				|| button.type == Type::WebView
				|| button.type == Type::SimpleWebView) {
				hasLink = true;
				actionLabel = actionLabel || ContainsAny(button.text, {
					"принять", "отклонить", "подтвердить", "забрать",
					"получить", "accept", "decline", "confirm", "claim",
					"reject",
				});
			}
		}
	}
	const auto impersonates = ContainsAny(from->name(), {
		"telegram", "телеграм", "gift", "подарк",
	});
	return hasLink && actionLabel && impersonates;
}

bool HasMark(const QString &text) {
	return !text.isEmpty() && text[0] == kMarkChar;
}

void PaintBanner(QPainter &p, QRect rect) {
	p.save();
	auto hq = PainterHighQualityEnabler(p);
	const auto red = QColor(0xFF, 0x5C, 0x5C);
	rect.setHeight(rect.height() - 2);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0xFF, 0x3B, 0x30, 46));
	p.drawRoundedRect(rect, 7, 7);
	p.setBrush(red);
	p.drawRoundedRect(QRect(rect.x(), rect.y(), 3, rect.height()), 1.5, 1.5);
	p.setPen(red);
	p.setFont(st::semiboldFont);
	p.drawText(
		rect.adjusted(11, 0, -6, 0),
		Qt::AlignVCenter | Qt::AlignLeft,
		st::semiboldFont->elided(
			QString::fromUtf8("\xE2\x9A\xA0 Похоже на скам"),
			rect.width() - 17));
	p.restore();
}

void ShowWarning(not_null<const HistoryItem*> item, Fn<void()> proceed) {
	const auto window = Core::App().activeWindow();
	if (!window) {
		return;
	}
	window->show(Ui::MakeConfirmBox({
		.text = TextWithEntities{ U("Это похоже на фейковый оффер. "
			"Настоящий оффер выглядит как карточка подарка по центру чата, "
			"а не как обычное сообщение человека с кнопками. "
			"Такие кнопки могут вести на сайты, которые крадут аккаунт и подарки. "
			"Не заходи в аккаунт и не подтверждай ничего на открывшейся странице.") },
		.confirmed = [=](Fn<void()> close) {
			close();
			proceed();
		},
		.confirmText = U("Всё равно открыть"),
		.cancelText = U("Не открывать"),
		.title = rpl::single(U("Возможный скам")),
	}));
}

} // namespace Mokagram::Scam
