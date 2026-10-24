#include "mokagram/streamer/mokagram_streamer.h"

#include "mokagram/settings/mokagram_settings.h"
#include "ui/toast/toast.h"

#include <rpl/distinct_until_changed.h>
#include <rpl/filter.h>
#include <rpl/map.h>

#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>

namespace Mokagram::Streamer {
namespace {

constexpr auto kDot = QChar(0x2022);

enum class CopyMode {
	Real = 0,
	Block = 1,
	Mask = 2,
};

[[nodiscard]] QString KeyFor(Field field) {
	switch (field) {
	case Field::Phone: return u"streamer.phone"_q;
	case Field::Username: return u"streamer.username"_q;
	case Field::Link: return u"streamer.link"_q;
	case Field::Id: return u"streamer.id"_q;
	case Field::Name: return u"streamer.name"_q;
	case Field::Notify: return u"streamer.notify"_q;
	}
	Unexpected("Field in Mokagram::Streamer::KeyFor.");
}

[[nodiscard]] QString Dots(int count) {
	return QString(count, kDot);
}

[[nodiscard]] QString MaskPhone(const QString &real) {
	auto result = real;
	for (auto &ch : result) {
		if (ch.isDigit()) {
			ch = kDot;
		}
	}
	return result;
}

[[nodiscard]] QString MaskUsername(const QString &real) {
	const auto hasAt = real.startsWith('@');
	const auto length = real.size() - (hasAt ? 1 : 0);
	return (hasAt ? u"@"_q : QString()) + Dots(std::clamp(int(length), 5, 16));
}

[[nodiscard]] QString MaskName(const QString &real) {
	return real.isEmpty() ? real : (real.left(1) + Dots(3));
}

} // namespace

bool Active(Field field) {
	return Settings::Bool(u"streamer.enabled"_q)
		&& Settings::Bool(KeyFor(field));
}

rpl::producer<bool> ActiveValue(Field field) {
	return rpl::combine(
		Settings::BoolValue(u"streamer.enabled"_q),
		Settings::BoolValue(KeyFor(field))
	) | rpl::map([](bool enabled, bool flag) {
		return enabled && flag;
	}) | rpl::distinct_until_changed();
}

rpl::producer<> Changed() {
	return Settings::Changed() | rpl::filter([](const QString &key) {
		return key.startsWith(u"streamer."_q);
	}) | rpl::to_empty;
}

QString MaskUrl(const QString &url) {
	// Хвост после первого "/" за доменом заменяем точками.
	const auto scheme = url.indexOf(u"://"_q);
	const auto hostStart = (scheme >= 0) ? (scheme + 3) : 0;
	const auto slash = url.indexOf('/', hostStart);
	if (slash < 0 || slash + 1 >= url.size()) {
		return url;
	}
	return url.left(slash + 1) + Dots(6);
}

QString Mask(Field field, const QString &real) {
	if (real.isEmpty()) {
		return real;
	}
	switch (field) {
	case Field::Phone: return MaskPhone(real);
	case Field::Username: return MaskUsername(real);
	case Field::Link: return MaskUrl(real);
	case Field::Id: return Dots(9);
	case Field::Name: return MaskName(real);
	case Field::Notify: return real;
	}
	Unexpected("Field in Mokagram::Streamer::Mask.");
}

TextWithEntities Mask(Field field, TextWithEntities real) {
	auto masked = Mask(field, real.text);
	if (masked.size() != real.text.size()) {
		// Длина изменилась: сущности больше не ложатся на текст.
		real.entities.clear();
	}
	real.text = std::move(masked);
	return real;
}

QString ForCopy(Field field, const QString &real) {
	if (!Active(field)) {
		return real;
	}
	switch (CopyMode(Settings::Int(u"streamer.copy"_q))) {
	case CopyMode::Block: return QString();
	case CopyMode::Mask: return Mask(field, real);
	case CopyMode::Real: break;
	}
	return real;
}

bool CopyBlocked(Field field) {
	if (!Active(field)
		|| CopyMode(Settings::Int(u"streamer.copy"_q)) != CopyMode::Block) {
		return false;
	}
	Ui::Toast::Show(QString::fromUtf8(
		"Режим стримера: копирование отключено"));
	return true;
}

bool SetClipboard(Field field, const QString &real) {
	if (CopyBlocked(field)) {
		return false;
	}
	QGuiApplication::clipboard()->setText(ForCopy(field, real));
	return true;
}

void RegisterSettings() {
	const auto section = QString::fromUtf8("Режим стримера");
	const auto note = QString::fromUtf8(
		"Для тех, кто показывает экран зрителям: выбранные личные данные "
		"заменяются точками везде в клиенте. Переписка и чужие имена "
		"остаются как есть.");
	const auto add = [&](
			const char *key,
			const char *title,
			const char *description,
			const char *example,
			const char *icon,
			bool value,
			const char *dependsOn = "streamer.enabled") {
		Settings::Register({
			.key = QString::fromUtf8(key),
			.section = section,
			.title = QString::fromUtf8(title),
			.description = QString::fromUtf8(description),
			.kind = Settings::Kind::Bool,
			.defaultValue = value,
			.dependsOn = QString::fromUtf8(dependsOn),
			.category = u"privacy"_q,
			.sectionIcon = u"hide"_q,
			.sectionNote = note,
			.icon = QString::fromUtf8(icon),
			.example = QString::fromUtf8(example),
		});
	};
	add("streamer.enabled",
		"Режим стримера",
		"Прячет ваш номер, юзернейм и ID на экране, "
		"чтобы их не увидели зрители.",
		"",
		"hide",
		false,
		"");
	add("streamer.phone",
		"Номер телефона",
		"Ваш номер в настройках, профиле и меню.",
		"+888676767 → +•••••••••",
		"phone",
		true);
	add("streamer.username",
		"Юзернейм",
		"Ваш @юзернейм там, где он показан.",
		"@ecronx → @••••••",
		"username",
		true);
	add("streamer.link",
		"Ссылки на ваш профиль",
		"Адреса вида t.me/ваш_ник.",
		"t.me/ecronx → t.me/••••••",
		"link",
		true);
	add("streamer.id",
		"Числовой ID",
		"Служебный номер вашего аккаунта.",
		"",
		"id",
		true);
	add("streamer.name",
		"Имя и фамилия",
		"Только ваши: в профиле, меню и настройках. "
		"Чужие имена в чатах не трогаем.",
		"",
		"person",
		false);
	add("streamer.notify",
		"Текст уведомлений",
		"Во всплывающих уведомлениях не видно, кто написал "
		"и что именно.",
		"",
		"notify",
		false);
	Settings::Register({
		.key = u"streamer.copy"_q,
		.section = section,
		.title = QString::fromUtf8("Что копировать со скрытым"),
		.description = QString::fromUtf8(
			"Что попадёт в буфер обмена, если скопировать "
			"спрятанное значение."),
		.kind = Settings::Kind::Choice,
		.defaultValue = int(CopyMode::Real),
		.choices = {
			QString::fromUtf8("Настоящее значение"),
			QString::fromUtf8("Ничего"),
			QString::fromUtf8("Точки"),
		},
		.dependsOn = u"streamer.enabled"_q,
		.category = u"privacy"_q,
		.sectionIcon = u"hide"_q,
		.icon = u"copy"_q,
	});
}

} // namespace Mokagram::Streamer
