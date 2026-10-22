// Mokagram: окно «Кастомизация», строится из реестра настроек.
#pragma once

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Mokagram::Settings {

void CustomizeBox(
	not_null<Ui::GenericBox*> box,
	Window::SessionController *controller);

void ShowCustomizeBox(Window::SessionController *controller);
void StartDebugCommands();

} // namespace Mokagram::Settings
