// Mokagram: сниппеты. Пишешь ;имя, и при отправке оно заменяется сохранённым текстом.
#include "mokagram/snippets/mokagram_snippets.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "mokagram/settings/mokagram_settings.h"
#include "ui/widgets/fields/input_field.h"

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
				int(match.capturedStart()),
				int(match.capturedEnd()),
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

namespace {

// Буквы и цифры латиницы в другом начертании. Кириллицы там нет, она остаётся как есть.
[[nodiscard]] QString MapLatin(const QString &text, int kind) {
	const auto pick = [&](QChar c) -> QString {
		const auto upper = (c >= 'A' && c <= 'Z');
		const auto lower = (c >= 'a' && c <= 'z');
		const auto digit = (c >= '0' && c <= '9');
		if (!upper && !lower && !digit) {
			return QString(c);
		}
		const auto n = upper ? (c.unicode() - 'A') : lower ? (c.unicode() - 'a') : (c.unicode() - '0');
		const auto ucs = [](uint code) {
			const auto value = char32_t(code);
			return QString::fromUcs4(&value, 1);
		};
		switch (kind) {
		case 0: // 𝐁𝐨𝐥𝐝
			return upper ? ucs(0x1D400 + n) : lower ? ucs(0x1D41A + n) : ucs(0x1D7CE + n);
		case 1: // 𝓢𝓬𝓻𝓲𝓹𝓽
			return digit ? QString(c) : upper ? ucs(0x1D4D0 + n) : ucs(0x1D4EA + n);
		case 2: // 𝔊𝔬𝔱𝔥𝔦𝔠
			return digit ? QString(c) : upper ? ucs(0x1D56C + n) : ucs(0x1D586 + n);
		case 3: // Ⓒⓘⓡⓒⓛⓔⓢ
			return digit
				? (n == 0 ? ucs(0x24EA) : ucs(0x2460 + n - 1))
				: upper ? ucs(0x24B6 + n) : ucs(0x24D0 + n);
		case 4: // Ｗｉｄｅ
			return upper ? ucs(0xFF21 + n) : lower ? ucs(0xFF41 + n) : ucs(0xFF10 + n);
		default: { // ꜱᴍᴀʟʟ ᴄᴀᴘꜱ
			static const char16_t small[] = u"ᴀʙᴄᴅᴇꜰɢʜɪᴊᴋʟᴍɴᴏᴘǫʀꜱᴛᴜᴠᴡxʏᴢ";
			return (upper || lower) ? QString(QChar(small[n])) : QString(c);
		}
		}
	};
	auto result = QString();
	for (const auto c : text) {
		result += pick(c);
	}
	return result;
}

// Ссылки, @имена, #теги и команды трогать нельзя: от замены букв они перестанут работать.
[[nodiscard]] bool IsProtectedWord(const QString &word) {
	static const auto domain = QRegularExpression(u"\\w\\.\\w{2,}"_q);
	return word.contains('/') || word.contains('@') || word.contains('#')
		|| word.contains(':') || word.contains(domain);
}

} // namespace

void ApplyFont(TextWithTags &text) {
	const auto kind = Mokagram::Settings::Int(u"chats.font"_q);
	if (kind <= 0 || text.text.trimmed().isEmpty()) {
		return;
	}
	const auto first = text.text.trimmed().at(0);
	if (first == '.' || first == '/' || first == '!' || first == ';') {
		return;
	}
	const auto whole = [&](const QString &id) {
		text.tags.push_back({ 0, int(text.text.size()), id });
	};
	if (kind < 9) {
		// Если у человека уже есть своё форматирование, не накладываем поверх.
		if (!text.tags.isEmpty()) {
			return;
		}
		switch (kind) {
		case 1: whole(Ui::InputField::kTagBold); break;
		case 2: whole(Ui::InputField::kTagItalic); break;
		case 3: whole(Ui::InputField::kTagBold + '|' + Ui::InputField::kTagItalic); break;
		case 4: whole(Ui::InputField::kTagUnderline); break;
		case 5: whole(Ui::InputField::kTagStrikeOut); break;
		case 6: whole(Ui::InputField::kTagCode); break;
		case 7: whole(Ui::InputField::kTagSpoiler); break;
		case 8: whole(Ui::InputField::kTagBlockquote); break;
		}
		return;
	}
	if (!text.tags.isEmpty()) {
		return;
	}
	static const auto word = QRegularExpression(u"\\S+"_q);
	auto result = QString();
	auto last = 0;
	auto found = word.globalMatch(text.text);
	while (found.hasNext()) {
		const auto match = found.next();
		result += text.text.mid(last, match.capturedStart() - last);
		const auto part = match.captured();
		result += IsProtectedWord(part) ? part : MapLatin(part, kind - 9);
		last = match.capturedEnd();
	}
	result += text.text.mid(last);
	text.text = result;
}

} // namespace Mokagram::Snippets
