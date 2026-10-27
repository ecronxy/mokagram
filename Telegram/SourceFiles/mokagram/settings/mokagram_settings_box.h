// Mokagram: окно «Кастомизация», строится из реестра настроек.
#pragma once

namespace Ui {
class RpWidget;
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
namespace Main {
class Session;
} // namespace Main

namespace style {
using icon = internal::Icon;
} // namespace style

namespace Mokagram::Settings {

void CustomizeBox(
	not_null<Ui::GenericBox*> box,
	Window::SessionController *controller,
	QString page = u"look"_q);

void ShowCustomizeBox(Window::SessionController *controller);
// Страницы: privacy, security, history, chats, music, system, tools (look = Кастомизация).
void ShowSettingsPage(
	Window::SessionController *controller,
	const QString &page);
// Окно «Поддержать разработку» (как у вкладки «Важное»).
void ShowSupportBox(Window::SessionController *controller);
void StartDebugCommands();

// Открывает «Кастомизацию» сразу с поиском по тексту (например, по названию плагина).
void ShowCustomizeSearch(
	Window::SessionController *controller,
	const QString &query);

// Значок плагина из набора iOSTelegram по обычному эмодзи. false: набора пока нет, рисуйте запасной.
bool PaintPackIcon(
	QPainter &p,
	const QRect &rect,
	const QString &alt,
	std::shared_ptr<void> &state,
	not_null<Ui::RpWidget*> widget);

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
// Есть ли для значка пара из набора (до загрузки набора старый значок не рисуем).
[[nodiscard]] bool HasRowIcon(const void *icon);
// Грузит набор значков заранее, чтобы к открытию настроек он уже был.
void PreloadRowIcons(not_null<Main::Session*> session);

// Страница «Быстрые команды» в настройках mokaGram.
void ShowCommandsPage(Window::SessionController *controller);

} // namespace Mokagram::Settings
