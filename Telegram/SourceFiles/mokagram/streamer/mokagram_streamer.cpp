// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 2775002d
#include "mokagram/streamer/mokagram_streamer.h"
#include "mokagram/lang/mokagram_lang.h"

#include "mokagram/settings/mokagram_settings.h"
#include "ui/toast/toast.h"

#include <rpl/distinct_until_changed.h>
#include <rpl/filter.h>
#include <rpl/map.h>

#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>
#include <QtGui/QWindow>

#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

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

#ifdef Q_OS_WIN
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif // WDA_EXCLUDEFROMCAPTURE

// Прячет окна от захвата экрана (Discord, OBS, запись), пока включён
// режим стримера. Нужен Windows 10 2004+, на старых вызов молча не работает.
void ApplyCapture(QWindow *window, bool hidden) {
	if (!window || !window->isVisible()) {
		return;
	}
	SetWindowDisplayAffinity(
		reinterpret_cast<HWND>(window->winId()),
		hidden ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
}

[[nodiscard]] bool CaptureHidden() {
	return Settings::Bool(u"streamer.enabled"_q)
		&& Settings::Bool(u"streamer.capture"_q);
}

class CaptureGuard final : public QObject {
public:
	CaptureGuard() {
		qApp->installEventFilter(this);
		Settings::Changed() | rpl::filter([](const QString &key) {
			return key == u"streamer.enabled"_q
				|| key == u"streamer.capture"_q;
		}) | rpl::on_next([=] {
			refreshAll();
		}, _lifetime);
		refreshAll();
	}

	bool eventFilter(QObject *object, QEvent *e) override {
		if (e->type() == QEvent::Show) {
			if (const auto window = qobject_cast<QWindow*>(object)) {
				ApplyCapture(window, CaptureHidden());
			}
		}
		return false;
	}

private:
	void refreshAll() {
		const auto hidden = CaptureHidden();
		for (const auto window : QGuiApplication::allWindows()) {
			ApplyCapture(window, hidden);
		}
	}

	rpl::lifetime _lifetime;

};
#endif // Q_OS_WIN

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
		Mokagram::Lang::Tr("Режим стримера: копирование отключено")));
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
#ifdef Q_OS_WIN
	static const auto guard = std::make_unique<CaptureGuard>();
#endif // Q_OS_WIN
	const auto section = QString::fromUtf8(Mokagram::Lang::Tr("Режим стримера"));
	const auto note = QString::fromUtf8(
		Mokagram::Lang::Tr("Для тех, кто показывает экран зрителям: выбранные личные данные заменяются точками везде в клиенте. Переписка и чужие имена остаются как есть."));
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
		Mokagram::Lang::Tr("Режим стримера"),
		Mokagram::Lang::Tr("Прячет ваш номер, юзернейм и ID на экране, чтобы их не увидели зрители."),
		"",
		"hide",
		false,
		"");
	add("streamer.phone",
		Mokagram::Lang::Tr("Номер телефона"),
		Mokagram::Lang::Tr("Ваш номер в настройках, профиле и меню."),
		"+888676767 → +•••••••••",
		"phone",
		true);
	add("streamer.username",
		Mokagram::Lang::Tr("Юзернейм"),
		Mokagram::Lang::Tr("Ваш @юзернейм там, где он показан."),
		"@ecronx → @••••••",
		"username",
		true);
	add("streamer.link",
		Mokagram::Lang::Tr("Ссылки на ваш профиль"),
		Mokagram::Lang::Tr("Адреса вида t.me/ваш_ник."),
		"t.me/ecronx → t.me/••••••",
		"link",
		true);
	add("streamer.id",
		Mokagram::Lang::Tr("Числовой ID"),
		Mokagram::Lang::Tr("Служебный номер вашего аккаунта."),
		"",
		"id",
		true);
	add("streamer.name",
		Mokagram::Lang::Tr("Имя и фамилия"),
		Mokagram::Lang::Tr("Только ваши: в профиле, меню и настройках. Чужие имена в чатах не трогаем."),
		"",
		"person",
		false);
	add("streamer.notify",
		Mokagram::Lang::Tr("Текст уведомлений"),
		Mokagram::Lang::Tr("Во всплывающих уведомлениях не видно, кто написал и что именно."),
		"",
		"notify",
		false);
#ifdef Q_OS_WIN
	add("streamer.capture",
		Mokagram::Lang::Tr("Скрыть окно от трансляции"),
		Mokagram::Lang::Tr("Клиент не виден в Discord, OBS и записи экрана: зрители видят пустое место."),
		"",
		"hide",
		true);
#endif // Q_OS_WIN
	Settings::Register({
		.key = u"streamer.copy"_q,
		.section = section,
		.title = QString::fromUtf8(Mokagram::Lang::Tr("Что копировать со скрытым")),
		.description = QString::fromUtf8(
			Mokagram::Lang::Tr("Что попадёт в буфер обмена, если скопировать спрятанное значение.")),
		.kind = Settings::Kind::Choice,
		.defaultValue = int(CopyMode::Real),
		.choices = {
			QString::fromUtf8(Mokagram::Lang::Tr("Настоящее значение")),
			QString::fromUtf8(Mokagram::Lang::Tr("Ничего")),
			QString::fromUtf8(Mokagram::Lang::Tr("Точки")),
		},
		.dependsOn = u"streamer.enabled"_q,
		.category = u"privacy"_q,
		.sectionIcon = u"hide"_q,
		.icon = u"copy"_q,
	});
}

} // namespace Mokagram::Streamer
