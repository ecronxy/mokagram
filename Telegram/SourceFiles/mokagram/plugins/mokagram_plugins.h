// Mokagram: хост плагинов (встроенный Python).
// Этот файл не включает ничего из Qt/tdesktop: Python.h изолирован в .cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace Mokagram::Plugins {

using LogFn = std::function<void(const std::string &)>;

// Запускает встроенный Python, ставит SDK и загружает все плагины из папки.
// Возвращает число найденных файлов .plugin, -1 при ошибке Python. Повторный вызов ничего не делает.
int Start(LogFn log);

// Папка плагинов: MOKAGRAM_PLUGINS_DIR или ~/.local/share/mokagram/plugins.
std::string PluginsDir();

// Разбор исходника плагина БЕЗ запуска его кода. Возвращает JSON:
// {"meta": {...__поля__...}, "detected": ["network", ...], "error": null|"текст"}
// Запускает Python при необходимости.
std::string ParseMetadataJson(const std::string &source);

// Загружает (или перезагружает) один файл из папки плагинов, например "hello_moka.plugin".
// Возвращает id плагина или пустую строку.
std::string LoadFile(const std::string &fileName, LogFn log);

// JSON-массив установленных плагинов: id, file, state (loaded|disabled|error), error,
// name, description, author, version, icon, permissions.
std::string ListJson();

// Общий выключатель системы плагинов (выгружает все плагины / загружает обратно).
bool EngineEnabled();
bool SetEngineEnabled(bool enabled, LogFn log);

// Закреплённые плагины идут первыми в списке.
bool SetPinned(const std::string &id, bool pinned);

bool SetEnabled(const std::string &id, bool enabled, LogFn log);

// Выгружает плагин и удаляет его файл.
bool Remove(const std::string &id, LogFn log);

// Вызывает хук по цепочке плагинов. Вход и выход JSON:
// {"cancel": bool, "value": <изменённые параметры>}
std::string CallHook(
	const std::string &name,
	const std::string &paramsJson,
	LogFn log);

// Пункты меню плагинов для места ("message", ...). JSON-массив {pid, index, text};
// ctxJson это контекст (для message: id, peer, text, out), передаётся в condition.
std::string MenuItemsJson(const std::string &location, const std::string &ctxJson);

// Нажатие на пункт: вызывает action(ctx) у плагина.
bool MenuItemClick(
	const std::string &pid,
	int index,
	const std::string &ctxJson,
	LogFn log);

// Решение плагинов "скрыть сообщение": JSON сообщения -> true, если какой-то плагин скрывает.
bool CallMessageFilter(const std::string &messageJson, LogFn log);

// Замена текста плагинами: JSON сообщения -> пустая строка (без изменений) или "T"+новый текст.
std::string CallTextHook(const std::string &messageJson, LogFn log);

// Колбэк плагина по токену (диалог, тост с кнопкой, кнопка интерфейса). payloadJson уходит в колбэк как dict.
bool Invoke(
	const std::string &pid,
	const std::string &token,
	const std::string &payloadJson,
	LogFn log);

// Вызывает on_plugin_unload у всех плагинов.
void Shutdown(LogFn log);

// ---- мост к клиенту и хуки обновлений/запросов ----

// Операции SDK, которым нужен Qt и tdesktop (тост, отправка, реестр настроек).
// Вызывается из потока плагина без GIL, обязан сам переходить в главный поток.
using BridgeFn = std::function<std::string(
	const std::string &op,
	const std::string &argsJson)>;
void SetBridge(BridgeFn bridge);

// Ставит мост SDK (тост, отправка, настройки) и подписку на изменение настроек плагинов.
// Реализация в mokagram_plugin_api.cpp, вызывается из Start().
void InstallBridge();

// Идёт ли выгрузка: мост не должен ждать главный поток.
[[nodiscard]] bool Stopping();

enum class HookKind {
	Update = 0,      // on_update_hook, id = constructor обновления
	Updates = 1,     // on_updates_hook, id = constructor контейнера Updates
	PreRequest = 2,  // pre_request_hook, id = constructor метода
	PostRequest = 3, // post_request_hook, id = constructor метода запроса
};

// Есть ли хоть одна подписка этого вида (один атомарный счётчик, без блокировок).
[[nodiscard]] bool HookActive(HookKind kind);

// Быстрая проверка (без Python): есть ли плагин, подписанный на такой id.
[[nodiscard]] bool HookWanted(HookKind kind, uint32_t constructorId);

// Кладёт байты TL в очередь рабочего потока и сразу возвращает управление (наблюдающий хук).
// Можно звать из любого потока. Данные копируются, не больше 1 МБ.
void QueueHook(
	HookKind kind,
	uint32_t constructorId,
	const void *data,
	size_t sizeBytes);

// Событие клиента (new_message, ...) с JSON-данными: в очередь рабочего потока. Из любого потока.
void QueueEvent(const std::string &name, const std::string &payloadJson);

// Пока true, хуки запросов не вызываются: это запросы самого плагина (защита от петли).
void MarkFromPlugin(bool on);
[[nodiscard]] bool IsFromPlugin();

// Запоминает хеш файла и права при установке из окна клиента (fileName в папке плагинов).
std::string RecordInstall(const std::string &fileName, LogFn log);

// Пользователь подтвердил изменённый плагин (состояние needs_review): новый хеш, запуск.
bool Approve(const std::string &id, LogFn log);

// Пользователь поменял настройку plugin.<id>.<key>.
void NotifySettingChanged(
	const std::string &key,
	const std::string &valueJson,
	LogFn log);

} // namespace Mokagram::Plugins
