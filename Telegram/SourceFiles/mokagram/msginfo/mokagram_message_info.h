// Mokagram: окно «Информация о сообщении» (ID, даты, пересылка, вложение, кодировщик голосового).
#pragma once

class HistoryItem;

namespace Window {
class SessionController;
} // namespace Window

namespace Mokagram::MessageInfo {

void Show(
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);

} // namespace Mokagram::MessageInfo
