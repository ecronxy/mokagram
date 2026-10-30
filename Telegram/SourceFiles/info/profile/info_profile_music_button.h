/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/widgets/buttons.h"
#include <rpl/event_stream.h>
#include "ui/text/format_song_name.h"
#include "ui/text/text.h"

namespace Info::Profile {

// Mokagram: музыка прилегает к шапке, пока показана хотя бы одна плашка.
[[nodiscard]] bool MusicShown();
// Плашка стоит прямо под нижним краем шапки (не прокручена в сторону).
[[nodiscard]] bool MusicAttachedTo(not_null<QWidget*> bar);
[[nodiscard]] std::optional<QColor> MusicBandColor();
[[nodiscard]] rpl::producer<> MusicShownChanges();

struct MusicButtonData {
	Ui::Text::FormatSongName name;
};

class MusicButton final : public Ui::RippleButton {
public:
	MusicButton(QWidget *parent, MusicButtonData data, Fn<void()> handler);
	~MusicButton();

	void updateData(MusicButtonData data);
	void setOverrideBg(std::optional<QColor> color);

private:
	void paintEvent(QPaintEvent *e) override;
	void setCounted(bool counted);
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	int resizeGetHeight(int newWidth) override;

	Ui::Text::String _performer;
	Ui::Text::String _title;
	std::optional<QColor> _overrideBg;

	const QString _noteSymbol;
	const int _noteWidth;
	bool _counted = false;

};

} // namespace Info::Profile
