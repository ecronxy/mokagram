// Mokagram: сниппеты. Пишешь ;имя, и при отправке оно заменяется сохранённым текстом.
#include "mokagram/snippets/mokagram_snippets.h"

#include "core/application.h"
#include "core/core_settings.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>

#include <map>

namespace Mokagram::Snippets {
namespace {

constexpr auto kMaxText = 4000;
constexpr auto kMaxName = 24;

struct State {
	std::map<QString, QString> items;
	bool loaded = false;
};

[[nodiscard]] State &Data() {
	static auto result = State();
	return result;
}

[[nodiscard]] QString FilePath() {
	return cWorkingDir() + u"tdata/moka_snippets.json"_q;
}

void Load() {
	auto &state = Data();
	if (state.loaded) {
		return;
	}
	state.loaded = true;
	auto file = QFile(FilePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto root = QJsonDocument::fromJson(file.readAll()).object();
	for (auto i = root.begin(); i != root.end(); ++i) {
		state.items[i.key()] = i.value().toString().left(kMaxText);
	}
}

void Save() {
	auto object = QJsonObject();
	for (const auto &[name, text] : Data().items) {
		object.insert(name, text);
	}
	auto file = QSaveFile(FilePath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

} // namespace

QStringList Names() {
	Load();
	auto result = QStringList();
	for (const auto &[name, text] : Data().items) {
		result.push_back(name);
	}
	return result;
}

QString Get(const QString &name) {
	Load();
	const auto i = Data().items.find(name.toLower());
	return (i != Data().items.end()) ? i->second : QString();
}

void Set(const QString &name, const QString &text) {
	Load();
	Data().items[name.toLower().left(kMaxName)] = text.left(kMaxText);
	Save();
}

bool Remove(const QString &name) {
	Load();
	const auto removed = (Data().items.erase(name.toLower()) > 0);
	if (removed) {
		Save();
	}
	return removed;
}

void Expand(TextWithTags &text) {
	if (!text.text.contains(';')) {
		return;
	}
	Load();
	if (Data().items.empty()) {
		return;
	}
	static const auto re = QRegularExpression(
		u"(^|(?<=\\s));([\\p{L}\\d_]{1,24})(?![\\p{L}\\d_])"_q);
	auto result = QString();
	auto from = 0;
	auto changed = false;
	auto i = re.globalMatch(text.text);
	while (i.hasNext()) {
		const auto match = i.next();
		const auto found = Data().items.find(match.captured(2).toLower());
		if (found == Data().items.end()) {
			continue;
		}
		result += text.text.mid(from, match.capturedStart() - from);
		result += found->second;
		from = match.capturedEnd();
		changed = true;
	}
	if (changed) {
		result += text.text.mid(from);
		text.text = result;
		text.tags.clear();
	}
}

} // namespace Mokagram::Snippets
