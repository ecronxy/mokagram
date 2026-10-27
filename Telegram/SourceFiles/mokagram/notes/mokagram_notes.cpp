// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 0258dc90
// Mokagram: личные теги и заметки о собеседниках (хранятся только у тебя).
#include "mokagram/notes/mokagram_notes.h"

#include "base/flat_map.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "ui/painter.h"
#include "styles/style_basic.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

namespace Mokagram::Notes {
namespace {

constexpr auto kMaxTag = 24;
constexpr auto kMaxNote = 2000;

struct Entry {
	QString tag;
	QString note;
};

struct State {
	base::flat_map<quint64, Entry> entries;
	rpl::event_stream<PeerId> changed;
	bool loaded = false;
};

[[nodiscard]] State &Data() {
	static auto result = State();
	return result;
}

[[nodiscard]] QString FilePath() {
	return cWorkingDir() + u"tdata/moka_notes.json"_q;
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
		const auto object = i.value().toObject();
		state.entries[i.key().toULongLong()] = {
			.tag = object.value(u"tag"_q).toString().left(kMaxTag),
			.note = object.value(u"note"_q).toString().left(kMaxNote),
		};
	}
}

void Save() {
	auto object = QJsonObject();
	for (const auto &[id, entry] : Data().entries) {
		if (entry.tag.isEmpty() && entry.note.isEmpty()) {
			continue;
		}
		object.insert(QString::number(id), QJsonObject{
			{ u"tag"_q, entry.tag },
			{ u"note"_q, entry.note },
		});
	}
	auto file = QSaveFile(FilePath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

void Update(PeerId id, Fn<void(Entry&)> change) {
	Load();
	auto &state = Data();
	change(state.entries[id.value]);
	Save();
	state.changed.fire_copy(id);
}

} // namespace

QString Tag(PeerId id) {
	Load();
	const auto i = Data().entries.find(id.value);
	return (i != Data().entries.end()) ? i->second.tag : QString();
}

QString Note(PeerId id) {
	Load();
	const auto i = Data().entries.find(id.value);
	return (i != Data().entries.end()) ? i->second.note : QString();
}

void SetTag(PeerId id, const QString &tag) {
	Update(id, [&](Entry &entry) {
		entry.tag = tag.simplified().left(kMaxTag);
	});
}

void SetNote(PeerId id, const QString &note) {
	Update(id, [&](Entry &entry) {
		entry.note = note.trimmed().left(kMaxNote);
	});
}

rpl::producer<PeerId> Changed() {
	return Data().changed.events();
}

rpl::producer<QString> NoteValue(PeerId id) {
	return rpl::single(Note(id)) | rpl::then(
		Changed()
		| rpl::filter([=](PeerId changed) { return changed == id; })
		| rpl::map([=](PeerId) { return Note(id); }));
}

int PaintTag(
		QPainter &p,
		const QString &tag,
		int left,
		int top,
		int maxRight,
		const QColor &color) {
	if (tag.isEmpty()) {
		return 0;
	}
	const auto &font = st::normalFont;
	const auto padding = 6;
	const auto height = font->height - 2;
	// Длинный тег обрезаем многоточием, чтобы он не заезжал на время.
	const auto available = maxRight - left;
	if (available < 3 * padding + font->width(u"\u2026"_q)) {
		return 0;
	}
	const auto shown = font->elided(tag, available - 2 * padding);
	const auto width = font->width(shown) + 2 * padding;
	auto hq = PainterHighQualityEnabler(p);
	auto background = color;
	background.setAlphaF(0.18);
	p.setPen(Qt::NoPen);
	p.setBrush(background);
	const auto rect = QRect(left, top + 1, width, height);
	p.drawRoundedRect(rect, height / 2., height / 2.);
	p.setPen(color);
	p.setFont(font);
	p.drawText(rect, Qt::AlignCenter, shown);
	return width + 4;
}

} // namespace Mokagram::Notes
