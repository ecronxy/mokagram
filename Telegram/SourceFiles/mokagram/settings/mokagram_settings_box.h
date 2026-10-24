// Mokagram: окно «Кастомизация», строится из реестра настроек.
#pragma once

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Ui::Text {
class CustomEmoji;
} // namespace Ui::Text

namespace style::internal {
class Icon;
} // namespace style::internal
namespace style {
using icon = internal::Icon;
} // namespace style

namespace Mokagram::Settings {

void CustomizeBox(
	not_null<Ui::GenericBox*> box,
	Window::SessionController *controller);

void ShowCustomizeBox(Window::SessionController *controller);
void StartDebugCommands();

// Значок строки настроек из набора iOSTelegram; nullptr, если для значка
// нет пары или набор ещё не загрузился.
// Отдельный значок: строка «Настройки mokaGram» с эмодзи из набора mymoka.
[[nodiscard]] const style::icon &OwnRowIcon();
[[nodiscard]] std::unique_ptr<Ui::Text::CustomEmoji> RowIconEmoji(
	const void *icon,
	Fn<void()> repaint,
	int size);
[[nodiscard]] std::unique_ptr<Ui::Text::CustomEmoji> AltIconEmoji(
	const QString &alt,
	Fn<void()> repaint,
	int size);
[[nodiscard]] rpl::producer<> RowIconsUpdated();

} // namespace Mokagram::Settings
