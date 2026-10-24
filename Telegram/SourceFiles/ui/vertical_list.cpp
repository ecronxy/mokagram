/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/vertical_list.h"

#include "lang/lang_text_entity.h"
#include "ui/widgets/box_content_divider.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"

namespace Ui {

void AddSkip(not_null<Ui::VerticalLayout*> container) {
	AddSkip(container, st::defaultVerticalListSkip);
}

void AddSkip(not_null<Ui::VerticalLayout*> container, int skip) {
	container->add(object_ptr<Ui::FixedHeightWidget>(container, skip));
}

void AddDivider(
		not_null<Ui::VerticalLayout*> container,
		const style::DividerBar &st) {
	const auto divider = container->add(object_ptr<Ui::BoxContentDivider>(
		container,
		st::boxDividerHeight,
		st));
	divider->setProperty("moka-break", true);
}

not_null<Ui::FlatLabel*> AddDividerText(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> text,
		const style::margins &margins,
		const style::DividerLabel &st,
		RectParts parts) {
	return AddDividerText(
		container,
		std::move(text) | rpl::map(tr::marked),
		margins,
		st,
		parts);
}

not_null<Ui::FlatLabel*> AddDividerText(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<TextWithEntities> text,
		const style::margins &margins,
		const style::DividerLabel &st,
		RectParts parts) {
	auto label = object_ptr<Ui::FlatLabel>(
		container,
		std::move(text),
		st.label);
	const auto result = label.data();
	const auto divider = container->add(object_ptr<Ui::DividerLabel>(
		container,
		std::move(label),
		margins,
		st.bar,
		parts));
	divider->setProperty("moka-break", true);
	return result;
}

not_null<Ui::FlatLabel*> AddSubsectionTitle(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> text,
		style::margins addPadding,
		const style::FlatLabel *st) {
	// Mokagram: внутри карточки настроек заголовок серый, заглавными,
	// и стоит над карточкой (карточка режется по нему).
	auto inCard = false;
	for (QWidget *w = container; w; w = w->parentWidget()) {
		if (w->property("moka-group-outer").isValid()) {
			inCard = true;
			break;
		}
	}
	{
		text = std::move(text) | rpl::map([](QString value) {
			return value.toUpper();
		});
	}
	const auto label = container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			std::move(text),
			st ? *st : st::defaultSubsectionTitle),
		inCard
			? style::margins(12, 14, 12, 6)
			: (st::defaultSubsectionTitlePadding + addPadding));
	label->setTextColorOverride(st::windowSubTextFg->c);
	if (inCard) {
		label->setProperty("moka-title", true);
	}
	return label;
}

} // namespace Ui
