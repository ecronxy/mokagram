// Mokagram: .show, скачивание видео и картинок по ссылке прямо в клиенте (yt-dlp).
#pragma once

namespace Window {
class SessionController;
} // namespace Window

class History;

namespace Mokagram::Downloader {

// Первая http(s) ссылка из текста или пустая строка.
[[nodiscard]] QString FindUrl(const QString &text);

// Скачивает ссылку и открывает окно отправки файлов в этом чате.
void Show(
	not_null<Window::SessionController*> controller,
	not_null< ::History*> history,
	const QString &url);

} // namespace Mokagram::Downloader
