// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 15dde3b3
// Mokagram: экономия ресурсов. Фоновая экономия, режим тостера, возврат памяти системе.
#include "mokagram/perf/mokagram_perf.h"

#include "mokagram/settings/mokagram_settings.h"
#include "base/timer.h"
#include "storage/localstorage.h"
#include "platform/platform_specific.h"
#include "core/core_settings.h"
#include "core/application.h"
#include "settings.h"
#include "ui/text/text_emoji_budget.h"

#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>

#ifdef Q_OS_MAC
#include <crl/crl_async.h>
#include <malloc/malloc.h>
#include <atomic>
#else // Q_OS_MAC
#include <malloc.h>
#endif // Q_OS_MAC

namespace Mokagram::Perf {
namespace {

namespace S = ::Mokagram::Settings;

constexpr auto kIdleDelay = crl::time(8000);
constexpr auto kTrimActive = crl::time(5 * 60 * 1000);
constexpr auto kTrimIdle = crl::time(90 * 1000);

struct State {
	rpl::variable<bool> economy = false;
	bool looking = true;
	bool idleEconomy = false;
};

[[nodiscard]] State &Data() {
	static auto result = State();
	return result;
}

void Apply() {
	auto &state = Data();
	Ui::Text::SetCustomEmojiBudget(S::Int(u"lag.emoji_cap"_q));
	Ui::Text::SetStickerBudget(S::Int(u"lag.sticker_cap"_q));
	state.economy = S::Bool(u"lag.toaster"_q)
		|| (state.idleEconomy && S::Bool(u"lag.idle"_q));
}

// Мелкие блоки возвращаем системе: glibc иначе держит их в арене и память растёт.
void Trim() {
#if defined Q_OS_MAC
	static auto trimming = std::atomic_flag();
	if (!trimming.test_and_set()) {
		crl::async([] {
			malloc_zone_pressure_relief(nullptr, 0);
			trimming.clear();
		});
	}
#elif defined Q_OS_WIN
	_heapmin();
#else
	malloc_trim(0);
#endif
}

} // namespace

rpl::producer<bool> EconomyValue() {
	return Data().economy.value();
}

bool UserIsLooking() {
	return Data().looking;
}

void Start() {
#ifdef Q_OS_LINUX
	// Меньше арен и раньше отдаём память: заметно для долго работающего клиента.
	mallopt(M_ARENA_MAX, 2);
	mallopt(M_TRIM_THRESHOLD, 128 * 1024);
	mallopt(M_MMAP_THRESHOLD, 256 * 1024);
#endif // Q_OS_LINUX

	static auto idleTimer = QTimer();
	idleTimer.setSingleShot(true);
	idleTimer.setTimerType(Qt::VeryCoarseTimer);
	idleTimer.setInterval(int(kIdleDelay));
	QObject::connect(&idleTimer, &QTimer::timeout, [] {
		Data().idleEconomy = true;
		Apply();
		Trim();
	});

	static auto trimTimer = QTimer();
	trimTimer.setTimerType(Qt::VeryCoarseTimer);
	trimTimer.setInterval(int(kTrimActive));
	QObject::connect(&trimTimer, &QTimer::timeout, [] {
		Trim();
		trimTimer.setInterval(int(Data().looking ? kTrimActive : kTrimIdle));
	});
	trimTimer.start();

	QObject::connect(qApp, &QGuiApplication::applicationStateChanged, [](
			Qt::ApplicationState state) {
		auto &data = Data();
		if (state == Qt::ApplicationActive) {
			data.looking = true;
			idleTimer.stop();
			data.idleEconomy = false;
			// Смена фокуса приходит как системное событие, вне цикла событий приложения.
			// Apply() запускает анимации и откладывает вызовы, а это допустимо только
			// внутри обработки события, поэтому уходим в очередь.
			QMetaObject::invokeMethod(qApp, [] {
				Apply();
			}, Qt::QueuedConnection);
		} else {
			data.looking = false;
			idleTimer.start();
		}
	});

	// Переключатели в настройках действуют сразу.
	static auto lifetime = rpl::lifetime();
	S::Changed() | rpl::on_next([](QString key) {
		if (key == u"lag.autostart"_q) {
			// Автозапуск и «крестик сворачивает в фон» это обычные настройки клиента,
			// здесь только удобные переключатели рядом с остальным.
			const auto on = S::Bool(key);
			if (on != cAutoStart() && Platform::AutostartSupported()) {
				cSetAutoStart(on);
				Platform::AutostartToggle(on, [=](bool enabled) {
					if (enabled != on) {
						cSetAutoStart(enabled);
						S::SetBool(u"lag.autostart"_q, enabled);
					}
					Local::writeSettings();
				});
			}
		} else if (key == u"lag.background"_q) {
			const auto behavior = S::Bool(key)
				? Core::Settings::CloseBehavior::RunInBackground
				: Core::Settings::CloseBehavior::Quit;
			auto &settings = Core::App().settings();
			if (settings.closeBehavior() != behavior) {
				settings.setCloseBehavior(behavior);
				Local::writeSettings();
			}
		} else if (key.startsWith(u"lag."_q)) {
			Apply();
		}
	}, lifetime);
	// Переключатели показывают то, что реально включено в клиенте.
	S::SetBool(u"lag.autostart"_q, cAutoStart());
	S::SetBool(
		u"lag.background"_q,
		Core::App().settings().closeBehavior()
			== Core::Settings::CloseBehavior::RunInBackground);
	Apply();
}

} // namespace Mokagram::Perf
