// Mokagram: экономия ресурсов. Фоновая экономия, режим тостера, возврат памяти системе.
#include "mokagram/perf/mokagram_perf.h"

#include "mokagram/settings/mokagram_settings.h"
#include "base/timer.h"
#include "ui/text/text_emoji_budget.h"

#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>

#include <malloc.h>

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
	state.economy = S::Bool(u"lag.toaster"_q)
		|| (state.idleEconomy && S::Bool(u"lag.idle"_q));
}

// Мелкие блоки возвращаем системе: glibc иначе держит их в арене и память растёт.
void Trim() {
	malloc_trim(0);
}

} // namespace

rpl::producer<bool> EconomyValue() {
	return Data().economy.value();
}

bool UserIsLooking() {
	return Data().looking;
}

void Start() {
	// Меньше арен и раньше отдаём память: заметно для долго работающего клиента.
	mallopt(M_ARENA_MAX, 2);
	mallopt(M_TRIM_THRESHOLD, 128 * 1024);
	mallopt(M_MMAP_THRESHOLD, 256 * 1024);

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
			Apply();
		} else {
			data.looking = false;
			idleTimer.start();
		}
	});

	// Переключатели в настройках действуют сразу.
	static auto lifetime = rpl::lifetime();
	S::Changed() | rpl::on_next([](QString key) {
		if (key.startsWith(u"lag."_q)) {
			Apply();
		}
	}, lifetime);
	Apply();
}

} // namespace Mokagram::Perf
