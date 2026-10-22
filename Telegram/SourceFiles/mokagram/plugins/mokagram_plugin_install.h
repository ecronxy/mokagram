// Mokagram: окно установки плагина при клике на файл .plugin в чате.
#pragma once

#include "data/data_file_origin.h"
#include "ui/text/text_entity.h"

class DocumentData;
class HistoryItem;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Ui {
class GenericBox;
class RpWidget;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Mokagram::Plugins {

[[nodiscard]] bool IsPluginDocument(not_null<DocumentData*> document);

// Скачивает файл (если нужно), разбирает метаданные без запуска кода
// и показывает окно «Установка плагина» с кнопками «Установить» / «Отмена».
void OpenPluginDocument(
	Window::SessionController *controller,
	not_null<DocumentData*> document,
	Data::FileOrigin origin);

// Хук send_message_hook: плагины видят и могут изменить текст перед отправкой.
// Возвращает false, если какой-то плагин отменил отправку. При изменении текста
// форматирование (tags) сбрасывается: смещения после правки плагином уже неверны.
[[nodiscard]] bool RunSendMessageHook(TextWithTags &text, uint64 peerId);

// Делает из прямоугольной кнопки пилюлю: накладывает на углы виджеты цвета фона.
void MakePill(not_null<Ui::RpWidget*> widget, Fn<QColor()> pageBg);

// Пункты плагинов в контекстное меню сообщения.
void AddMessageMenuItems(not_null<Ui::PopupMenu*> menu, HistoryItem *item);

// Окно «Плагины»: список установленных, переключатель и удаление.
void PluginsBox(
	not_null<Ui::GenericBox*> box,
	Window::SessionController *controller);

} // namespace Mokagram::Plugins
