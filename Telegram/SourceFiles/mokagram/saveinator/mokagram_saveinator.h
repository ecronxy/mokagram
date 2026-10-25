// Mokagram: скачивание медиа по ссылке из сообщения через API Saveinator.
#pragma once

#include "base/basic_types.h"

class HistoryItem;

namespace Api {
struct SendAction;
} // namespace Api

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Mokagram::Saveinator {

void RegisterSettings();
void Start();

[[nodiscard]] QString FindLink(not_null<HistoryItem*> item);

void AddMessageAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);

[[nodiscard]] bool OfferSendDownload(
	not_null<Window::SessionController*> controller,
	const Api::SendAction &action,
	const QString &text,
	Fn<void()> sendAsText,
	Fn<void()> clearField);

} // namespace Mokagram::Saveinator
