// Mokagram: картинка-цитата из сообщений, с анонимизацией имён и аватарок.
#pragma once

#include "data/data_msg_id.h"

#include <QtGui/QImage>

namespace Window {
class SessionController;
} // namespace Window

namespace Mokagram::Quote {

void RegisterSettings();

// rows: снимки сообщений с экрана (с прозрачным фоном); если пусто,
// сообщения рисуются текстом по ids.
void ShowQuoteBox(
	not_null<Window::SessionController*> controller,
	std::vector<QImage> rows,
	std::vector<FullMsgId> ids);

} // namespace Mokagram::Quote
