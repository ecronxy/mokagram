// Mokagram: единый реестр настроек клиента, хранение в tdata/moka_settings.json.
#include "mokagram/settings/mokagram_settings.h"
#include "mokagram/lang/mokagram_lang.h"

#include "base/timer.h"
#include "base/flat_map.h"
#include "logs.h"
#include "settings.h"

#include <rpl/event_stream.h>
#include <rpl/filter.h>
#include <rpl/map.h>
#include <rpl/then.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

namespace Mokagram::Settings {
namespace {

constexpr auto kFormatVersion = 1;
constexpr auto kSaveDelay = crl::time(300);

struct State {
	std::vector<Entry> entries;
	base::flat_map<QString, int> entryIndices;
	// Явно заданные значения: отличия от умолчаний и неизвестные ключи.
	base::flat_map<QString, QJsonValue> values;
	rpl::event_stream<QString> changed;
	base::Timer saveTimer;
	bool dirty = false;
	bool loaded = false;
	bool quitHooked = false;
};

[[nodiscard]] State &Data() {
	static auto result = State();
	return result;
}

[[nodiscard]] QString FilePath() {
	return cWorkingDir() + u"tdata/moka_settings.json"_q;
}

[[nodiscard]] const Entry *Find(const QString &key) {
	const auto &data = Data();
	const auto i = data.entryIndices.find(key);
	return (i != data.entryIndices.end()) ? &data.entries[i->second] : nullptr;
}

// Приводит значение к виду, допустимому для записи. Пусто, если не подходит.
[[nodiscard]] std::optional<QJsonValue> Normalize(
		const Entry &entry,
		const QJsonValue &value) {
	switch (entry.kind) {
	case Kind::Bool:
		if (value.isBool()) {
			return value;
		}
		break;
	case Kind::Int:
		if (value.isDouble()) {
			const auto number = value.toInt();
			if (number >= entry.minValue && number <= entry.maxValue) {
				return QJsonValue(number);
			}
		}
		break;
	case Kind::Choice:
		if (value.isDouble()) {
			const auto number = value.toInt();
			if (number >= 0 && number < int(entry.choices.size())) {
				return QJsonValue(number);
			}
		}
		break;
	case Kind::String:
	case Kind::Color:
		if (value.isString()) {
			return value;
		}
		break;
	}
	return std::nullopt;
}

[[nodiscard]] bool SameValue(const QJsonValue &a, const QJsonValue &b) {
	return (a.type() == b.type())
		&& (a.isBool()
			? a.toBool() == b.toBool()
			: a.isString()
			? a.toString() == b.toString()
			: a.toDouble() == b.toDouble());
}

[[nodiscard]] QJsonValue Current(const QString &key) {
	auto &data = Data();
	if (const auto i = data.values.find(key); i != data.values.end()) {
		return i->second;
	}
	if (const auto entry = Find(key)) {
		return entry->defaultValue;
	}
	return QJsonValue();
}

void ScheduleSave() {
	auto &data = Data();
	data.dirty = true;
	if (!data.loaded) {
		return;
	}
	if (!data.saveTimer.isActive()) {
		data.saveTimer.callOnce(kSaveDelay);
	}
}

void Store(const QString &key, const QJsonValue &value) {
	auto &data = Data();
	const auto entry = Find(key);
	if (!entry) {
		return;
	}
	const auto fixed = Normalize(*entry, value);
	if (!fixed) {
		LOG(("[mokagram] Settings: bad value for '%1'.").arg(key));
		return;
	}
	const auto was = Current(key);
	if (SameValue(*fixed, entry->defaultValue)) {
		data.values.remove(key);
	} else {
		data.values[key] = *fixed;
	}
	if (SameValue(was, Current(key))) {
		return;
	}
	ScheduleSave();
	data.changed.fire_copy(key);
}

void ResetKey(const QString &key) {
	auto &data = Data();
	const auto entry = Find(key);
	if (!entry || !data.values.contains(key)) {
		return;
	}
	data.values.remove(key);
	ScheduleSave();
	data.changed.fire_copy(key);
}

[[nodiscard]] QByteArray BuildFile(const QJsonObject &values, bool exported) {
	auto root = QJsonObject();
	if (exported) {
		root.insert(u"format"_q, u"mokaset"_q);
		root.insert(u"app"_q, u"7.3.3-moka"_q);
	}
	root.insert(u"version"_q, kFormatVersion);
	root.insert(u"values"_q, values);
	return QJsonDocument(root).toJson(
		exported ? QJsonDocument::Indented : QJsonDocument::Compact);
}

[[nodiscard]] QJsonObject ValuesObject() {
	auto result = QJsonObject();
	for (const auto &[key, value] : Data().values) {
		result.insert(key, value);
	}
	return result;
}

} // namespace

void Register(Entry entry) {
	auto &data = Data();
	const auto key = entry.key;
	const auto index = data.entryIndices.find(key);
	if (index != data.entryIndices.end()) {
		data.entries[index->second] = std::move(entry);
	} else {
		data.entryIndices.emplace(key, int(data.entries.size()));
		data.entries.push_back(std::move(entry));
	}
	// Значение из файла могло прийти раньше записи: проверяем его теперь.
	const auto &now = *Find(key);
	if (const auto i = data.values.find(key); i != data.values.end()) {
		const auto fixed = Normalize(now, i->second);
		if (!fixed || SameValue(*fixed, now.defaultValue)) {
			if (!fixed) {
				LOG(("[mokagram] Settings: bad stored value for '%1'."
					).arg(key));
			}
			data.values.remove(key);
			ScheduleSave();
		} else if (!SameValue(*fixed, i->second)) {
			i->second = *fixed;
		}
	}
}

const std::vector<Entry> &Entries() {
	return Data().entries;
}

bool Bool(const QString &key) {
	const auto value = Current(key);
	return value.isBool() ? value.toBool() : false;
}

int Int(const QString &key) {
	const auto value = Current(key);
	return value.isDouble() ? value.toInt() : 0;
}

QString String(const QString &key) {
	const auto value = Current(key);
	return value.isString() ? value.toString() : QString();
}

void SetBool(const QString &key, bool value) {
	Store(key, value);
}

void SetInt(const QString &key, int value) {
	Store(key, value);
}

void SetString(const QString &key, const QString &value) {
	Store(key, value);
}

void Reset(const QString &key) {
	ResetKey(key);
}

void ResetAll() {
	auto keys = QStringList();
	for (const auto &entry : Data().entries) {
		keys.push_back(entry.key);
	}
	for (const auto &key : keys) {
		ResetKey(key);
	}
}

void ResetSection(const QString &section) {
	auto keys = QStringList();
	for (const auto &entry : Data().entries) {
		if (entry.section == section) {
			keys.push_back(entry.key);
		}
	}
	for (const auto &key : keys) {
		ResetKey(key);
	}
}

bool IsChanged(const QString &key) {
	if (const auto entry = Find(key); entry && entry->action) {
		return false;
	}
	return Data().values.contains(key);
}

rpl::producer<bool> BoolValue(const QString &key) {
	return rpl::single(
		Bool(key)
	) | rpl::then(
		Data().changed.events()
		| rpl::filter([=](const QString &changed) { return changed == key; })
		| rpl::map([=](const QString &) { return Bool(key); }));
}

rpl::producer<int> IntValue(const QString &key) {
	return rpl::single(
		Int(key)
	) | rpl::then(
		Data().changed.events()
		| rpl::filter([=](const QString &changed) { return changed == key; })
		| rpl::map([=](const QString &) { return Int(key); }));
}

rpl::producer<QString> StringValue(const QString &key) {
	return rpl::single(
		String(key)
	) | rpl::then(
		Data().changed.events()
		| rpl::filter([=](const QString &changed) { return changed == key; })
		| rpl::map([=](const QString &) { return String(key); }));
}

rpl::producer<QString> Changed() {
	return Data().changed.events();
}

void Load() {
	auto &data = Data();
	if (!data.quitHooked) {
		data.quitHooked = true;
		data.saveTimer.setCallback([] { Save(); });
		QObject::connect(
			QCoreApplication::instance(),
			&QCoreApplication::aboutToQuit,
			[] { Save(); });
	}
	data.values.clear();
	auto file = QFile(FilePath());
	if (file.open(QIODevice::ReadOnly)) {
		auto error = QJsonParseError();
		const auto document = QJsonDocument::fromJson(
			file.readAll(),
			&error);
		if (error.error != QJsonParseError::NoError
			|| !document.isObject()) {
			LOG(("[mokagram] Settings: moka_settings.json is broken."));
		} else {
			const auto values = document.object().value(
				u"values"_q).toObject();
			for (auto i = values.begin(); i != values.end(); ++i) {
				data.values[i.key()] = i.value();
			}
		}
	}
	// Уже зарегистрированные записи проверяем сразу, остальные при Register.
	for (const auto &entry : data.entries) {
		const auto i = data.values.find(entry.key);
		if (i == data.values.end()) {
			continue;
		}
		const auto fixed = Normalize(entry, i->second);
		if (!fixed || SameValue(*fixed, entry.defaultValue)) {
			data.values.remove(entry.key);
		} else {
			i->second = *fixed;
		}
	}
	data.loaded = true;
	data.dirty = false;
}

void Save() {
	auto &data = Data();
	data.saveTimer.cancel();
	if (!data.dirty) {
		return;
	}
	data.dirty = false;
	auto file = QSaveFile(FilePath());
	if (!file.open(QIODevice::WriteOnly)) {
		LOG(("[mokagram] Settings: cannot write moka_settings.json."));
		data.dirty = true;
		return;
	}
	file.write(BuildFile(ValuesObject(), false));
	if (!file.commit()) {
		LOG(("[mokagram] Settings: commit of moka_settings.json failed."));
		data.dirty = true;
	}
}

QByteArray Export() {
	return BuildFile(ValuesObject(), true);
}

bool PreviewImport(
		const QByteArray &data,
		ImportSummary *summary,
		QString *error) {
	const auto fail = [&](const char *text) {
		if (error) {
			*error = QString::fromUtf8(text);
		}
		return false;
	};
	auto parse = QJsonParseError();
	const auto document = QJsonDocument::fromJson(data, &parse);
	if (parse.error != QJsonParseError::NoError || !document.isObject()) {
		return fail(Mokagram::Lang::Tr("Файл не похож на набор настроек."));
	}
	const auto root = document.object();
	if (root.value(u"format"_q).toString() != u"mokaset"_q) {
		return fail(Mokagram::Lang::Tr("Это не файл .mokaset."));
	}
	if (root.value(u"version"_q).toInt() > kFormatVersion) {
		return fail(Mokagram::Lang::Tr("Файл сделан более новой версией mokaGram."));
	}
	if (!root.value(u"values"_q).isObject()) {
		return fail(Mokagram::Lang::Tr("В файле нет значений."));
	}
	auto result = ImportSummary();
	const auto values = root.value(u"values"_q).toObject();
	for (auto i = values.begin(); i != values.end(); ++i) {
		const auto entry = Find(i.key());
		if (!entry) {
			++result.unknown;
			continue;
		}
		const auto fixed = Normalize(*entry, i.value());
		if (!fixed) {
			++result.invalid;
		} else if (!SameValue(*fixed, Current(i.key()))) {
			++result.changed;
		}
	}
	if (summary) {
		*summary = result;
	}
	return true;
}

bool Import(const QByteArray &data, QString *error) {
	if (!PreviewImport(data, nullptr, error)) {
		return false;
	}
	// Перед импортом сохраняем копию текущего файла.
	Save();
	const auto path = FilePath();
	QFile::remove(path + u".bak"_q);
	QFile::copy(path, path + u".bak"_q);

	auto &state = Data();
	const auto values = QJsonDocument::fromJson(
		data).object().value(u"values"_q).toObject();
	for (auto i = values.begin(); i != values.end(); ++i) {
		if (Find(i.key())) {
			Store(i.key(), i.value());
		} else {
			// Ключ плагина, которого нет: ждёт в файле до его загрузки.
			state.values[i.key()] = i.value();
			ScheduleSave();
		}
	}
	Save();
	return true;
}

} // namespace Mokagram::Settings
