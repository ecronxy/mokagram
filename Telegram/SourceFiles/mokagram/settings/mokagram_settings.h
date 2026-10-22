// Mokagram: единый реестр настроек клиента (публичный интерфейс).
// Реализация и экран «Кастомизация» в mokagram_settings.cpp и mokagram_settings_box.cpp.
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QJsonValue>

#include <rpl/producer.h>

#include <vector>

namespace Mokagram::Settings {

enum class Kind {
	Bool,
	Int,
	Choice,
	String,
	Color,
};

struct Entry {
	QString key;          // "streamer.phone", "ghost.read", "plugin.<id>.<key>"
	QString section;      // "Режим стримера", "Призрак", ...
	QString title;        // что видит человек
	QString description;  // одна строка пояснения, можно пусто
	Kind kind = Kind::Bool;
	QJsonValue defaultValue = false;
	int minValue = 0;     // для Int
	int maxValue = 100;   // для Int
	QStringList choices;  // для Choice: подписи; значение это индекс
	QString dependsOn;    // ключ Bool, при false запись неактивна
	bool requiresRestart = false;
	// Только для вида окна (необязательно).
	QString category;     // "privacy", "chats", "accounts", "look", "plugins"
	QString sectionIcon;  // значок раздела: "lock", "ghost", "history"...
	QString sectionNote;  // сноска под группой строк, простыми словами
	QString icon;         // значок строки, по умолчанию как у раздела
	QString details;      // подробное объяснение (до нескольких строк)
	QString example;      // "было → станет", например "+7 912 → +• •••"
};

// Регистрируется один раз при старте (повтор того же ключа заменяет запись).
void Register(Entry entry);

[[nodiscard]] const std::vector<Entry> &Entries();

// Текущие значения (если не меняли, то значение по умолчанию).
[[nodiscard]] bool Bool(const QString &key);
[[nodiscard]] int Int(const QString &key);
[[nodiscard]] QString String(const QString &key);

void SetBool(const QString &key, bool value);
void SetInt(const QString &key, int value);
void SetString(const QString &key, const QString &value);
void Reset(const QString &key);
void ResetAll();
void ResetSection(const QString &section);

// Отличается ли значение от умолчания.
[[nodiscard]] bool IsChanged(const QString &key);

// Срабатывает при любом изменении ключа (и сразу отдаёт текущее значение).
[[nodiscard]] rpl::producer<bool> BoolValue(const QString &key);
[[nodiscard]] rpl::producer<int> IntValue(const QString &key);
[[nodiscard]] rpl::producer<QString> StringValue(const QString &key);

// Любое изменение любого ключа (для перерисовки без rpl).
[[nodiscard]] rpl::producer<QString> Changed();

// Хранилище: tdata/moka_settings.json, пишутся только отличия от умолчаний.
void Load();
void Save();

// Экспорт и импорт набора настроек (.mokaset, JSON).
[[nodiscard]] QByteArray Export();
bool Import(const QByteArray &data, QString *error = nullptr);

struct ImportSummary {
	int changed = 0;  // значений, которые изменят настройки
	int unknown = 0;  // ключей без записи (плагин не загружен), сохраняются
	int invalid = 0;  // значений неверного типа или вне диапазона, пропускаются
};
// Разбирает файл и считает, что изменится, ничего не применяя.
bool PreviewImport(
	const QByteArray &data,
	ImportSummary *summary,
	QString *error = nullptr);

} // namespace Mokagram::Settings
