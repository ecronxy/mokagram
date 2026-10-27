// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: f1d391dd
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
