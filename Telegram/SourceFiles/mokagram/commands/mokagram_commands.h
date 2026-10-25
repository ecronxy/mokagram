// Mokagram: быстрые команды в поле ввода (.mute, .unmute, .ban, .unban).
#pragma once

#include "base/basic_types.h"
#include "data/data_msg_id.h"

#include <QtCore/QStringList>
#include <vector>

struct TextWithTags;
class History;

namespace Window {
class SessionController;
} // namespace Window

namespace Mokagram::Commands {

void RegisterSettings();

// true: сообщение было командой, отправлять его не нужно. Если команда
// хочет оставить в поле другой текст (.calc), он кладётся в replacement.
[[nodiscard]] bool Handle(
	not_null<Window::SessionController*> controller,
	not_null< ::History*> history,
	const TextWithTags &text,
	FullReplyTo replyTo,
	TextWithTags *replacement = nullptr);

// Алиасы: пары (имя, команда) без точки.
[[nodiscard]] std::vector<std::pair<QString, QString>> Aliases();
void SetAliases(const std::vector<std::pair<QString, QString>> &list);
[[nodiscard]] QStringList CommandNames();

} // namespace Mokagram::Commands
