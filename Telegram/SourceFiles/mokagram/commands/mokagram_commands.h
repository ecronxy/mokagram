// Mokagram: быстрые команды в поле ввода (.mute, .unmute, .ban, .unban).
#pragma once

#include "base/basic_types.h"
#include "data/data_msg_id.h"

struct TextWithTags;
class History;

namespace Window {
class SessionController;
} // namespace Window

namespace Mokagram::Commands {

void RegisterSettings();

// true: сообщение было командой, отправлять его не нужно.
[[nodiscard]] bool Handle(
	not_null<Window::SessionController*> controller,
	not_null< ::History*> history,
	const TextWithTags &text,
	FullReplyTo replyTo);

// Окно со списком команд и алиасами (кнопка в настройках mokaGram).
void ShowCommandsBox(not_null<Window::SessionController*> controller);

} // namespace Mokagram::Commands
