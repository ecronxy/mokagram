// Mokagram: картинка-цитата из сообщений, с анонимизацией имён и аватарок.
#pragma once

#include "data/data_msg_id.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Mokagram::Quote {

void RegisterSettings();

void ShowQuoteBox(
	not_null<Window::SessionController*> controller,
	std::vector<FullMsgId> ids);

} // namespace Mokagram::Quote
