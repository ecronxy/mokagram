#include "mokagram/scam/mokagram_scam.h"

#include "core/application.h"
#include "data/data_peer.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_reply_markup.h"
#include "mokagram/settings/mokagram_settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "window/window_controller.h"

namespace Mokagram::Scam {
namespace {

namespace S = Mokagram::Settings;

constexpr auto kTelegramServiceId = 777000;

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
		.description = U("Настоящий оффер появляется в чате служебной плашкой Telegram. Если обычное сообщение с кнопками «Принять» и «Отклонить» ведёт по ссылке, клиент пометит его и спросит перед открытием."),
		.kind = S::Kind::Bool,
		.defaultValue = true,
		.category = u"privacy"_q,
		.sectionIcon = u"block"_q,
	});
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

void ShowWarning(not_null<const HistoryItem*> item, Fn<void()> proceed) {
	const auto window = Core::App().activeWindow();
	if (!window) {
		return;
	}
	window->show(Ui::MakeConfirmBox({
		.text = TextWithEntities{ U("Это похоже на фейковый оффер. "
			"Настоящий оффер появляется в самом чате служебной плашкой Telegram, "
			"а не обычным сообщением от пользователя или бота с кнопками-ссылками. "
			"Такие кнопки ведут на сайты, которые крадут аккаунт и подарки. "
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
