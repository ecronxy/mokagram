// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 8c4e3e61
#include "mokagram/ui/mokagram_report_box.h"

#include "mokagram/ui/mokagram_box_parts.h"
#include "api/api_report.h"
#include "data/data_report.h"
#include "lang/lang_keys.h"
#include "ui/boxes/report_box_graphics.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtGui/QPainterPath>

namespace Mokagram::Report {
namespace {

using namespace BoxParts;

constexpr auto kBoxWidth = 380;
constexpr auto kPad = 16;
constexpr auto kRowPad = 13;
constexpr auto kMinRowHeight = 48;
constexpr auto kChevron = 14;
constexpr auto kCommentLengthMax = 512;

[[nodiscard]] const style::owned_color &Clear() {
	static const auto result = style::owned_color(QColor(0, 0, 0, 0));
	return result;
}

void SetupRounded(not_null<Ui::GenericBox*> box) {
	static const auto noButtons = [] {
		auto result = st::defaultBox;
		result.buttonPadding = style::margins();
		result.buttonHeight = 0;
		result.bg = Clear().color();
		return result;
	}();
	box->setStyle(noButtons);
	box->paintOn([=](QPainter &p) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::boxBg);
		p.drawRoundedRect(box->rect(), kBoxRadius, kBoxRadius);
	});
	box->setWidth(kBoxWidth);
	box->setNoContentMargin(true);
}

[[nodiscard]] QPen LinePen(const QColor &color, float64 width) {
	auto result = QPen(color, width);
	result.setCapStyle(Qt::RoundCap);
	result.setJoinStyle(Qt::RoundJoin);
	return result;
}

void AddHeader(
		not_null<Ui::GenericBox*> box,
		const QString &title,
		bool nested) {
	static const auto titleSt = [] {
		auto result = st::boxTitle;
		result.minWidth = 1;
		result.maxHeight = 0;
		result.align = style::al_top;
		return result;
	}();
	const auto content = box->verticalLayout();
	const auto header = content->add(
		object_ptr<Ui::RpWidget>(content),
		QMargins(10, 10, 10, 0));
	header->resize(header->width(), kRoundSize);
	const auto close = [=] {
		box->uiShow()->hideLayer();
	};
	const auto left = nested
		? MakeRoundButton(header, RoundIcon::Back, [=] { box->closeBox(); })
		: MakeRoundButton(header, RoundIcon::Close, close);
	const auto right = nested
		? MakeRoundButton(header, RoundIcon::Close, close).get()
		: nullptr;
	const auto label = Ui::CreateChild<Ui::FlatLabel>(header, title, titleSt);
	label->setAttribute(Qt::WA_TransparentForMouseEvents);
	header->widthValue() | rpl::on_next([=](int width) {
		const auto side = kRoundSize + 8;
		label->resizeToWidth(std::max(width - 2 * side, 1));
		const auto height = std::max(kRoundSize, label->height());
		label->moveToLeft(side, (height - label->height()) / 2);
		left->moveToLeft(0, (height - kRoundSize) / 2);
		if (right) {
			right->moveToRight(0, (height - kRoundSize) / 2);
		}
		header->resize(width, height);
	}, header->lifetime());
}

[[nodiscard]] not_null<Ui::VerticalLayout*> AddCard(
		not_null<Ui::VerticalLayout*> container,
		int top) {
	const auto card = container->add(
		object_ptr<Ui::VerticalLayout>(container),
		QMargins(kOuterSide, top, kOuterSide, 0));
	card->paintOn([=](QPainter &p) {
		PaintCard(p, card->rect());
	});
	return card;
}

void AddOption(
		not_null<Ui::VerticalLayout*> card,
		const QString &text,
		Fn<void()> callback) {
	static const auto labelSt = [] {
		auto result = st::boxLabel;
		result.minWidth = 1;
		return result;
	}();
	const auto first = (card->count() == 0);
	const auto row = card->add(object_ptr<Ui::RippleButton>(
		card,
		st::defaultRippleAnimation));
	row->setPointerCursor(true);
	row->setClickedCallback(std::move(callback));
	const auto label = Ui::CreateChild<Ui::FlatLabel>(row, text, labelSt);
	label->setAttribute(Qt::WA_TransparentForMouseEvents);
	row->widthValue() | rpl::on_next([=](int width) {
		const auto available = width - 2 * kPad - kChevron;
		if (available <= 0) {
			return;
		}
		label->resizeToWidth(available);
		const auto height = std::max(
			kMinRowHeight,
			label->height() + 2 * kRowPad);
		label->moveToLeft(kPad, (height - label->height()) / 2);
		row->resize(width, height);
	}, row->lifetime());
	row->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(row);
		const auto origin = row->pos();
		auto path = QPainterPath();
		path.addRoundedRect(
			QRectF(-origin.x(), -origin.y(), card->width(), card->height()),
			kCardRadius,
			kCardRadius);
		p.save();
		p.setClipPath(path);
		row->paintRipple(p, 0, 0);
		p.restore();
		if (!first) {
			auto line = st::windowSubTextFg->c;
			line.setAlpha(40);
			p.fillRect(kPad, 0, row->width() - kPad, st::lineWidth, line);
		}
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(LinePen(st::windowSubTextFg->c, 1.6));
		const auto x = row->width() - kPad - 4.;
		const auto y = row->height() / 2.;
		p.drawLine(QPointF(x - 3, y - 4), QPointF(x, y));
		p.drawLine(QPointF(x, y), QPointF(x - 3, y + 4));
	}, row->lifetime());
}

void AddMainButton(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> text,
		Fn<void()> callback) {
	static const auto buttonSt = [] {
		auto result = st::defaultActiveButton;
		result.height = 48;
		result.textTop = 14;
		result.radius = 24;
		result.fullRadius = true;
		return result;
	}();
	const auto row = container->add(
		object_ptr<Ui::RpWidget>(container),
		QMargins(kOuterSide, 16, kOuterSide, 0));
	row->resize(row->width(), buttonSt.height);
	const auto button = Ui::CreateChild<Ui::RoundButton>(
		row,
		std::move(text),
		buttonSt);
	button->setClickedCallback(std::move(callback));
	row->widthValue() | rpl::on_next([=](int width) {
		button->setFullWidth(width);
		button->moveToLeft(0, 0);
	}, row->lifetime());
}

void AddComment(
		not_null<Ui::GenericBox*> box,
		Api::ReportResult::CommentOption option,
		const Data::ReportInput &input,
		Fn<void(Data::ReportInput)> choose) {
	static const auto noteSt = [] {
		auto result = st::boxDividerLabel;
		result.minWidth = 1;
		return result;
	}();
	const auto content = box->verticalLayout();
	Ui::AddReportDetailsIconButton(box);
	const auto card = AddCard(content, 8);
	const auto field = card->add(
		object_ptr<Ui::InputField>(
			card,
			CardField(st::newGroupDescription),
			Ui::InputField::Mode::MultiLine,
			(option.optional
				? tr::lng_report_details_optional()
				: tr::lng_report_details_non_optional()),
			QString()));
	field->setMaxLength(kCommentLengthMax);
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			tr::lng_report_details_message_about(),
			noteSt),
		QMargins(kOuterSide + kPad, 8, kOuterSide + kPad, 0));
	const auto submit = [=] {
		if (!option.optional && field->empty()) {
			field->showError();
			field->setFocus();
			return;
		}
		auto copy = input;
		copy.optionId = option.id;
		copy.comment = field->getLastText();
		choose(std::move(copy));
	};
	field->submits() | rpl::on_next(submit, field->lifetime());
	box->setFocusCallback([=] {
		field->setFocusFast();
	});
	AddMainButton(content, tr::lng_report_button(), submit);
}

} // namespace

void FlowBox(
		not_null<Ui::GenericBox*> box,
		const Api::ReportResult &result,
		const Data::ReportInput &input,
		Fn<void(Data::ReportInput)> choose) {
	SetupRounded(box);
	AddHeader(
		box,
		result.title.isEmpty() ? input.optionText : result.title,
		!input.optionId.isNull());
	const auto content = box->verticalLayout();
	if (!result.options.empty()) {
		const auto card = AddCard(content, 14);
		for (const auto &option : result.options) {
			AddOption(card, option.text, [=, id = option.id, text = option.text] {
				auto copy = input;
				copy.optionId = id;
				copy.optionText = text;
				choose(std::move(copy));
			});
		}
	}
	if (const auto &comment = result.commentOption) {
		AddComment(box, *comment, input, choose);
	}
	Ui::AddSkip(content, kOuterSide);
}

} // namespace Mokagram::Report
