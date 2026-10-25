// Mokagram: окно установки плагина при клике на файл .plugin в чате.
#pragma once

#include "data/data_file_origin.h"
#include "ui/text/text_entity.h"
#include "ui/style/style_core.h"

class DocumentData;
class HistoryItem;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Ui {
class GenericBox;
class RpWidget;
class AbstractButton;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace style {
struct Box;
} // namespace style

class PeerData;

namespace Mokagram::Plugins {

// Разбирает файл плагина и показывает окно установки (права, версия).
void InstallFromBytes(
	Window::SessionController *controller,
	const QByteArray &bytes,
	Fn<void()> installed);

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
[[nodiscard]] bool RunSendMessageHook(
	TextWithTags &text,
	uint64 peerId,
	FullMsgId replyTo = FullMsgId());

// Делает из прямоугольной кнопки пилюлю: накладывает на углы виджеты цвета фона.
void MakePill(not_null<Ui::RpWidget*> widget, Fn<QColor()> pageBg);

// Пункты плагинов в контекстное меню сообщения.
void AddMessageMenuItems(
	not_null<Ui::PopupMenu*> menu,
	HistoryItem *item,
	const QString &selected = QString(),
	const std::vector<not_null<HistoryItem*>> &selectedItems = {});

// Пункты плагинов для места "chat" (меню чата и строки списка) и "profile" (меню профиля).
// ctx: peer, title, kind (user|bot|group|channel), username. add вызывается на каждый пункт.
using MenuAdd = Fn<void(const QString &, Fn<void()>, const style::icon *)>;
void AddPeerMenuItems(
	const QString &location,
	PeerData *peer,
	const MenuAdd &add);

// Пункты плагинов для бокового меню (drawer).
struct DrawerItem {
	QString text;
	Fn<void()> handler;
	const style::icon *icon = nullptr;
};
[[nodiscard]] std::vector<DrawerItem> DrawerMenuItems();

// Каркас окон в стиле «Плагины»: круглое окно без кнопок и круглые
// кнопки шапки (закрыть или поиск). Все кастомные окна строим на них.
void RoundedWindow(not_null<Ui::GenericBox*> box);

// Скругляет обычное окно (радиус 28), оставляя его кнопки внизу.
void RoundedKeepButtons(
	not_null<Ui::GenericBox*> box,
	const style::Box &base);
[[nodiscard]] not_null<Ui::AbstractButton*> MakeHeaderButton(
	not_null<QWidget*> parent,
	bool close,
	Fn<void()> callback);

// Окно «Плагины»: список установленных, переключатель и удаление.
void PluginsBox(
	not_null<Ui::GenericBox*> box,
	Window::SessionController *controller);

} // namespace Mokagram::Plugins
