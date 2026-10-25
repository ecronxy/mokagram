#include "mokagram/ui/mokagram_box_parts.h"

#include "base/call_delayed.h"
#include "ui/abstract_button.h"
#include "ui/layers/box_content.h"
#include "ui/painter.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <map>

namespace Mokagram::BoxParts {
namespace {

[[nodiscard]] const style::owned_color &Clear() {
	static const auto result = style::owned_color(QColor(0, 0, 0, 0));
	return result;
}

[[nodiscard]] style::RoundButton Pill(const style::RoundButton &base) {
	auto result = base;
	result.width = -30;
	result.height = 40;
	result.textTop = 10;
	result.style = st::defaultBoxButton.style;
	result.fullRadius = true;
	return result;
}

} // namespace

not_null<Ui::AbstractButton*> MakeRoundButton(
		not_null<QWidget*> parent,
		RoundIcon icon,
		Fn<void()> callback) {
	const auto button = Ui::CreateChild<Ui::AbstractButton>(parent.get());
	button->resize(kRoundSize, kRoundSize);
	button->setPointerCursor(true);
	button->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(button);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBg);
		p.drawEllipse(button->rect());
		p.scale(0.75, 0.75);
		if (icon == RoundIcon::Menu) {
			p.setBrush(st::windowFg);
			for (const auto y : { 17., 24., 31. }) {
				p.drawEllipse(QPointF(24., y), 2.2, 2.2);
			}
			return;
		}
		auto pen = QPen(st::windowFg->c, 2.2);
		pen.setCapStyle(Qt::RoundCap);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		if (icon == RoundIcon::Back) {
			p.drawLine(QPointF(27, 16), QPointF(19, 24));
			p.drawLine(QPointF(19, 24), QPointF(27, 32));
		} else {
			p.drawLine(QPointF(17, 17), QPointF(31, 31));
			p.drawLine(QPointF(31, 17), QPointF(17, 31));
		}
	}, button->lifetime());
	button->setClickedCallback(std::move(callback));
	return button;
}

void RoundLayer(not_null<Ui::BoxContent*> box) {
	const auto apply = [=] {
		if (const auto layer = box->parentWidget()) {
			layer->setProperty("moka-radius", kBoxRadius);
			layer->update();
		}
	};
	apply();
	base::call_delayed(0, box, apply);
}

void PaintCard(QPainter &p, QRect rect) {
	auto hq = PainterHighQualityEnabler(p);
	const auto radius = std::min(kCardRadius, rect.height() / 2);
	p.setPen(Qt::NoPen);
	p.setBrush(st::windowBg);
	p.drawRoundedRect(rect, radius, radius);
}

const style::Box &PillButtonsBox() {
	static const auto result = [] {
		auto result = st::defaultBox;
		result.buttonPadding = style::margins(8, 8, 14, 14);
		result.buttonHeight = 40;
		result.button = PillLight();
		return result;
	}();
	return result;
}

const style::RoundButton &PillActive() {
	static const auto result = Pill(st::defaultActiveButton);
	return result;
}

const style::RoundButton &PillLight() {
	static const auto result = [] {
		auto result = Pill(st::defaultBoxButton);
		result.textBg = st::windowBg;
		result.textBgOver = st::windowBgOver;
		return result;
	}();
	return result;
}

const style::RoundButton &PillAttention() {
	static const auto result = [] {
		auto result = Pill(st::attentionBoxButton);
		result.textBg = st::attentionButtonBgOver;
		result.textBgOver = st::attentionButtonBgRipple;
		return result;
	}();
	return result;
}

const style::InputField &CardField(const style::InputField &base) {
	static auto cache = std::map<
		const style::InputField*,
		std::unique_ptr<style::InputField>>();
	auto &result = cache[&base];
	if (!result) {
		result = std::make_unique<style::InputField>(base);
		result->textBg = Clear().color();
		result->textBgActive = Clear().color();
		result->textMargins = style::margins(16, 12, 44, 12);
		result->placeholderMargins = style::margins();
		result->placeholderFgActive = st::windowSubTextFg;
		result->placeholderScale = 0.;
		result->placeholderShift = 0;
		result->placeholderFont = st::normalFont;
		result->border = 0;
		result->borderActive = 0;
		result->heightMin = 44;
	}
	return *result;
}

} // namespace Mokagram::BoxParts
