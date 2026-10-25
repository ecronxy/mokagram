// Mokagram: сниппеты. Пишешь ;имя, и при отправке оно заменяется сохранённым текстом.
#include "mokagram/snippets/mokagram_snippets.h"

#include "core/application.h"
#include "core/core_settings.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
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
	std::map<QString, TextWithTags> items;
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
		// Старый формат: просто строка без разметки.
		if (i.value().isString()) {
			state.items[i.key()] = { i.value().toString().left(kMaxText) };
			continue;
		}
		const auto object = i.value().toObject();
		auto value = TextWithTags{
			object.value(u"text"_q).toString().left(kMaxText),
		};
		for (const auto &tag : object.value(u"tags"_q).toArray()) {
			const auto entry = tag.toObject();
			const auto offset = entry.value(u"o"_q).toInt();
			const auto length = entry.value(u"l"_q).toInt();
			if (offset >= 0 && length > 0
				&& offset + length <= value.text.size()) {
				value.tags.push_back(TextWithTags::Tag{
					offset,
					length,
					entry.value(u"id"_q).toString(),
				});
			}
		}
		state.items[i.key()] = std::move(value);
	}
}

void Save() {
	auto object = QJsonObject();
	for (const auto &[name, value] : Data().items) {
		auto tags = QJsonArray();
		for (const auto &tag : value.tags) {
			tags.push_back(QJsonObject{
				{ u"o"_q, tag.offset },
				{ u"l"_q, tag.length },
				{ u"id"_q, tag.id },
			});
		}
		object.insert(name, QJsonObject{
			{ u"text"_q, value.text },
			{ u"tags"_q, tags },
		});
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

TextWithTags Get(const QString &name) {
	Load();
	const auto i = Data().items.find(name.toLower());
	return (i != Data().items.end()) ? i->second : TextWithTags();
}

void Set(const QString &name, const TextWithTags &text) {
	Load();
	auto value = Slice(text, 0, kMaxText);
	Data().items[name.toLower().left(kMaxName)] = std::move(value);
	Save();
}

TextWithTags Slice(const TextWithTags &text, int from, int length) {
	auto result = TextWithTags{ text.text.mid(from, length) };
	const auto till = from + int(result.text.size());
	for (const auto &tag : text.tags) {
		const auto start = std::max(tag.offset, from);
		const auto end = std::min(tag.offset + tag.length, till);
		if (end > start) {
			result.tags.push_back(TextWithTags::Tag{ start - from, end - start, tag.id });
		}
	}
	return result;
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
	struct Replacement {
		int start = 0;
		int end = 0;
		const TextWithTags *with = nullptr;
	};
	auto list = std::vector<Replacement>();
	auto i = re.globalMatch(text.text);
	while (i.hasNext()) {
		const auto match = i.next();
		const auto found = Data().items.find(match.captured(2).toLower());
		if (found != Data().items.end()) {
			list.push_back({
				match.capturedStart(),
				match.capturedEnd(),
				&found->second,
			});
		}
	}
	if (list.empty()) {
		return;
	}
	auto result = TextWithTags();
	auto from = 0;
	auto delta = 0;
	// Исходная разметка переезжает вместе с текстом; та, что задевает
	// заменённое слово, отбрасывается.
	const auto moveTags = [&](int till) {
		for (const auto &tag : text.tags) {
			if (tag.offset >= from && tag.offset + tag.length <= till) {
				result.tags.push_back(TextWithTags::Tag{
					tag.offset + delta,
					tag.length,
					tag.id,
				});
			}
		}
	};
	for (const auto &item : list) {
		moveTags(item.start);
		result.text += text.text.mid(from, item.start - from);
		const auto base = int(result.text.size());
		result.text += item.with->text;
		for (const auto &tag : item.with->tags) {
			result.tags.push_back(TextWithTags::Tag{
				base + tag.offset,
				tag.length,
				tag.id,
			});
		}
		delta += int(item.with->text.size()) - (item.end - item.start);
		from = item.end;
	}
	moveTags(text.text.size());
	result.text += text.text.mid(from);
	text = std::move(result);
}

} // namespace Mokagram::Snippets
