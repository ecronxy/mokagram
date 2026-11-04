// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 2760f29f
#include "mokagram/ui/mokagram_box_parts.h"

#include "base/call_delayed.h"
#include "ui/abstract_button.h"
#include "ui/rp_widget.h"
#include "ui/effects/animations.h"
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

void PaintSwitch(QPainter &p, float64 t) {
	const auto on = st::windowBgActive->c;
	const auto off = st::windowSubTextFg->c;
	const auto track = QRectF(1, 1, 38, 22);
	p.setBrush(anim::color(QColor(0, 0, 0, 0), on, t));
	p.setPen(QPen(anim::color(off, on, t), 1.6));
	p.drawRoundedRect(track, 11, 11);
	const auto size = 10. + 8. * t;
	const auto cx = 12. + 16. * t;
	p.setPen(Qt::NoPen);
	p.setBrush(anim::color(off, st::windowFgActive->c, t));
	p.drawEllipse(QPointF(cx, 12.), size / 2, size / 2);
}

} // namespace

// Переключатель в стиле Material 3: переключатель 40x24, при включении бегунок растёт.
not_null<Ui::AbstractButton*> MakeSwitch(
		not_null<QWidget*> parent,
		bool checked,
		Fn<void(bool)> changed,
		rpl::producer<bool> active) {
	struct State {
		Ui::Animations::Simple animation;
		bool checked = false;
		bool active = true;
		[[nodiscard]] bool shown() const {
			return checked && active;
		}
	};
	const auto button = Ui::CreateChild<Ui::AbstractButton>(parent.get());
	const auto state = button->lifetime().make_state<State>();
	state->checked = checked;
	button->resize(40, 24);
	button->setPointerCursor(true);
	button->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(button);
		auto hq = PainterHighQualityEnabler(p);
		const auto t = state->animation.value(state->shown() ? 1. : 0.);
		PaintSwitch(p, t);
	}, button->lifetime());
	button->setClickedCallback([=] {
		const auto was = state->shown();
		state->checked = !state->checked;
		const auto now = state->shown();
		if (was != now) {
			state->animation.start(
				[=] { button->update(); },
				was ? 1. : 0.,
				now ? 1. : 0.,
				200);
		}
		changed(state->checked);
	});
	// Выключенная система плагинов гасит тумблер, не меняя выбор пользователя.
	std::move(active) | rpl::on_next([=](bool value) {
		const auto was = state->shown();
		state->active = value;
		const auto now = state->shown();
		if (was != now) {
			state->animation.start(
				[=] { button->update(); },
				was ? 1. : 0.,
				now ? 1. : 0.,
				200);
		}
	}, button->lifetime());
	return button;
}

// Переключатель в стиле Material 3, значение приходит из реестра.
// Сам не кликается: клик обрабатывает строка.
not_null<Ui::AbstractButton*> MakeSwitch(
		not_null<QWidget*> parent,
		rpl::producer<bool> value) {
	struct State {
		Ui::Animations::Simple animation;
		bool checked = false;
		bool started = false;
	};
	const auto button = Ui::CreateChild<Ui::AbstractButton>(parent.get());
	const auto state = button->lifetime().make_state<State>();
	button->resize(40, 24);
	button->setAttribute(Qt::WA_TransparentForMouseEvents);
	button->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(button);
		auto hq = PainterHighQualityEnabler(p);
		const auto t = state->animation.value(state->checked ? 1. : 0.);
		PaintSwitch(p, t);
	}, button->lifetime());
	std::move(value) | rpl::on_next([=](bool checked) {
		if (state->checked == checked && state->started) {
			return;
		}
		const auto first = !state->started;
		state->started = true;
		state->checked = checked;
		if (first) {
			button->update();
			return;
		}
		state->animation.start(
			[=] { button->update(); },
			checked ? 0. : 1.,
			checked ? 1. : 0.,
			200);
	}, button->lifetime());
	return button;
}

void MakePill(not_null<Ui::RpWidget*> widget, Fn<QColor()> pageBg) {
	struct Corner {
		not_null<Ui::RpWidget*> widget;
		bool right;
		bool bottom;
	};
	struct State {
		std::vector<Corner> corners;
		int radius = 1;
	};
	const auto state = widget->lifetime().make_state<State>();
	for (const auto right : { false, true }) {
		for (const auto bottom : { false, true }) {
			const auto corner = Ui::CreateChild<Ui::RpWidget>(widget.get());
			corner->setAttribute(Qt::WA_TransparentForMouseEvents);
			corner->paintRequest(
			) | rpl::on_next([=] {
				auto p = QPainter(corner);
				auto hq = PainterHighQualityEnabler(p);
				const auto r = state->radius;
				auto path = QPainterPath();
				path.addRect(corner->rect());
				auto circle = QPainterPath();
				circle.addEllipse(QRectF(
					right ? -r : 0,
					bottom ? -r : 0,
					2 * r,
					2 * r));
				p.fillPath(path.subtracted(circle), pageBg());
			}, corner->lifetime());
			state->corners.push_back({ corner, right, bottom });
		}
	}
	widget->sizeValue(
	) | rpl::on_next([=](QSize size) {
		const auto r = std::max(1, std::min(size.width(), size.height()) / 2);
		state->radius = r;
		for (const auto &corner : state->corners) {
			corner.widget->resize(r, r);
			corner.widget->move(
				corner.right ? size.width() - r : 0,
				corner.bottom ? size.height() - r : 0);
			corner.widget->update();
			corner.widget->raise();
		}
	}, widget->lifetime());
}


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
