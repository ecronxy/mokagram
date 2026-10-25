// Mokagram: общие детали окон в стиле mokaGram.
#pragma once

namespace Ui {
class AbstractButton;
class BoxContent;
} // namespace Ui

namespace style {
struct Box;
struct InputField;
struct RoundButton;
} // namespace style

namespace Mokagram::BoxParts {

inline constexpr auto kBoxRadius = 28;
inline constexpr auto kCardRadius = 22;
inline constexpr auto kOuterSide = 14;
inline constexpr auto kRoundSize = 36;

enum class RoundIcon {
	Close,
	Back,
	Menu,
};

[[nodiscard]] not_null<Ui::AbstractButton*> MakeRoundButton(
	not_null<QWidget*> parent,
	RoundIcon icon,
	Fn<void()> callback);

void RoundLayer(not_null<Ui::BoxContent*> box);
void PaintCard(QPainter &p, QRect rect);

[[nodiscard]] const style::Box &PillButtonsBox();
[[nodiscard]] const style::RoundButton &PillActive();
[[nodiscard]] const style::RoundButton &PillLight();
[[nodiscard]] const style::InputField &CardField(
	const style::InputField &base);

} // namespace Mokagram::BoxParts
