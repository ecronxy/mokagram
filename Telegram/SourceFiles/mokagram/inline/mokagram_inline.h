#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QString>

#include <optional>

namespace Mokagram::Inline {

enum class Kind { Math, Unit, Currency, TimeZone };

struct Options {
	bool math = true;
	bool units = true;
	bool currency = false;
	bool timeZones = true;
	bool degrees = false;
};

struct Result {
	Kind kind = Kind::Math;
	QString value;
	bool needsRates = false;
};

[[nodiscard]] std::optional<Result> Evaluate(
	const QString &text,
	const Options &options = {},
	const QHash<QString, double> &usdRates = {},
	const QDateTime &now = QDateTime::currentDateTimeUtc());

// Команда «.cr 2 ton usdt»: сводка или перевод в указанные валюты. Пустой
// text при needsRates: курсы ещё не загружены.
struct Conversion {
	QString text;
	bool needsRates = false;
};
[[nodiscard]] std::optional<Conversion> ConvertCurrencyCommand(
	const QString &args,
	const QHash<QString, double> &usdRates);

} // namespace Mokagram::Inline
