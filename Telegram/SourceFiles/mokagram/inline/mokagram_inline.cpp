#include "mokagram_inline.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QTimeZone>

#include <cmath>
#include <numbers>
#include <vector>

namespace Mokagram::Inline {
namespace {

using namespace Qt::StringLiterals;

constexpr auto kMaxLength = 256;
constexpr auto kMaxOperations = 128;
constexpr auto kMaxDepth = 32;

QString Number(double value) {
	return QString::number(value == 0. ? 0. : value, 'g', 12);
}

class Parser {
public:
	Parser(QString text, bool degrees)
	: _text(std::move(text)), _degrees(degrees) {
		_text.replace(QChar(0x00d7), u'*');
		_text.replace(QChar(0x00f7), u'/');
		_text.replace(QChar(0x2212), u'-');
		_text.replace(QChar(0x03c0), u"pi"_s);
	}

	std::optional<double> parse() {
		const auto value = expression();
		space();
		return _valid && _at == _text.size() && std::isfinite(value)
			? std::optional(value)
			: std::nullopt;
	}

private:
	void space() {
		while (_at < _text.size() && _text[_at].isSpace()) ++_at;
	}

	bool take(QChar ch) {
		space();
		if (_at == _text.size() || _text[_at] != ch) return false;
		++_at;
		if (++_operations > kMaxOperations) _valid = false;
		return true;
	}

	double expression() {
		auto value = term();
		while (_valid) {
			if (take(u'+')) value += term();
			else if (take(u'-')) value -= term();
			else break;
		}
		return value;
	}

	double term() {
		auto value = unary();
		while (_valid) {
			if (take(u'*')) value *= unary();
			else if (take(u'/')) {
				const auto divisor = unary();
				if (divisor == 0.) _valid = false;
				else value /= divisor;
			} else break;
		}
		return value;
	}

	double unary() {
		if (++_depth > kMaxDepth) {
			_valid = false;
			--_depth;
			return 0.;
		}
		const auto value = take(u'+') ? unary()
			: take(u'-') ? -unary()
			: power();
		--_depth;
		return value;
	}

	double power() {
		auto value = atom();
		while (_valid && take(u'%')) value /= 100.;
		if (_valid && take(u'^')) value = std::pow(value, unary());
		return value;
	}

	double atom() {
		space();
		if (!_valid) return 0.;
		if (take(u'(')) {
			const auto value = expression();
			if (!take(u')')) _valid = false;
			return value;
		}
		const auto start = _at;
		if (_at < _text.size() && _text[_at].isLetter()) {
			while (_at < _text.size() && _text[_at].isLetter()) ++_at;
			const auto name = _text.mid(start, _at - start).toLower();
			if (name == u"pi") return std::numbers::pi;
			if (name == u"e") return std::numbers::e;
			if (!take(u'(')) { _valid = false; return 0.; }
			const auto arg = expression();
			if (!take(u')')) _valid = false;
			const auto angle = _degrees ? arg * std::numbers::pi / 180. : arg;
			if (name == u"sqrt") return std::sqrt(arg);
			if (name == u"abs") return std::abs(arg);
			if (name == u"sin") return std::sin(angle);
			if (name == u"cos") return std::cos(angle);
			if (name == u"tan") return std::tan(angle);
			if (name == u"ln") return std::log(arg);
			if (name == u"log") return std::log10(arg);
			if (name == u"exp") return std::exp(arg);
			if (name == u"floor") return std::floor(arg);
			if (name == u"ceil") return std::ceil(arg);
			if (name == u"round") return std::round(arg);
			_valid = false;
			return 0.;
		}
		auto decimal = false;
		while (_at < _text.size()) {
			if (_text[_at].isDigit()) ++_at;
			else if (_text[_at] == u'.' && !decimal) { decimal = true; ++_at; }
			else break;
		}
		if (_at < _text.size() && _text[_at].toLower() == u'e') {
			++_at;
			if (_at < _text.size() && (_text[_at] == u'+' || _text[_at] == u'-')) ++_at;
			while (_at < _text.size() && _text[_at].isDigit()) ++_at;
		}
		auto ok = false;
		const auto value = _text.mid(start, _at - start).toDouble(&ok);
		_valid = _valid && ok;
		return value;
	}

	QString _text;
	qsizetype _at = 0;
	int _depth = 0;
	int _operations = 0;
	bool _valid = true;
	bool _degrees = false;

};

struct Unit {
	QStringList names;
	int dimension = 0;
	double scale = 1.;
	double offset = 0.;
};

const std::vector<Unit> &Units() {
	static const auto result = std::vector<Unit>{
		{{u"m"_s, u"meter"_s, u"meters"_s, u"м"_s}, 1, 1.},
		{{u"km"_s, u"км"_s}, 1, 1000.},
		{{u"cm"_s, u"см"_s}, 1, .01},
		{{u"mm"_s, u"мм"_s}, 1, .001},
		{{u"in"_s, u"inch"_s, u"inches"_s, u"дюйм"_s}, 1, .0254},
		{{u"ft"_s, u"foot"_s, u"feet"_s}, 1, .3048},
		{{u"yd"_s, u"yard"_s}, 1, .9144},
		{{u"mi"_s, u"mile"_s, u"miles"_s}, 1, 1609.344},
		{{u"kg"_s, u"кг"_s}, 2, 1.},
		{{u"g"_s, u"г"_s}, 2, .001},
		{{u"mg"_s, u"мг"_s}, 2, .000001},
		{{u"t"_s, u"tonne"_s, u"т"_s}, 2, 1000.},
		{{u"lb"_s, u"lbs"_s, u"pound"_s}, 2, .45359237},
		{{u"oz"_s, u"ounce"_s}, 2, .028349523125},
		{{u"s"_s, u"sec"_s, u"с"_s, u"сек"_s}, 3, 1.},
		{{u"ms"_s, u"мс"_s}, 3, .001},
		{{u"min"_s, u"мин"_s}, 3, 60.},
		{{u"h"_s, u"hr"_s, u"hour"_s, u"ч"_s}, 3, 3600.},
		{{u"d"_s, u"day"_s, u"д"_s}, 3, 86400.},
		{{u"c"_s, u"°c"_s, u"celsius"_s}, 4, 1., 273.15},
		{{u"f"_s, u"°f"_s, u"fahrenheit"_s}, 4, 5. / 9., 255.3722222222222},
		{{u"k"_s, u"kelvin"_s}, 4, 1.},
		{{u"m/s"_s, u"м/с"_s}, 5, 1.},
		{{u"km/h"_s, u"км/ч"_s, u"kph"_s}, 5, 1. / 3.6},
		{{u"mph"_s}, 5, .44704},
		{{u"knot"_s, u"knots"_s, u"kn"_s}, 5, 1852. / 3600.},
		{{u"m2"_s, u"m²"_s, u"м2"_s, u"м²"_s}, 6, 1.},
		{{u"km2"_s, u"km²"_s, u"км²"_s}, 6, 1000000.},
		{{u"ha"_s, u"га"_s}, 6, 10000.},
		{{u"acre"_s, u"acres"_s}, 6, 4046.8564224},
		{{u"ft2"_s, u"ft²"_s}, 6, .09290304},
		{{u"l"_s, u"liter"_s, u"litre"_s, u"л"_s}, 7, 1.},
		{{u"ml"_s, u"мл"_s}, 7, .001},
		{{u"m3"_s, u"m³"_s, u"м³"_s}, 7, 1000.},
		{{u"gal"_s, u"usgal"_s}, 7, 3.785411784},
		{{u"b"_s, u"byte"_s, u"bytes"_s, u"байт"_s}, 8, 1.},
		{{u"bit"_s, u"bits"_s, u"бит"_s}, 8, .125},
		{{u"kb"_s, u"кб"_s}, 8, 1000.},
		{{u"mb"_s, u"мб"_s}, 8, 1000000.},
		{{u"gb"_s, u"гб"_s}, 8, 1000000000.},
		{{u"tb"_s, u"тб"_s}, 8, 1000000000000.},
		{{u"kib"_s}, 8, 1024.},
		{{u"mib"_s}, 8, 1048576.},
		{{u"gib"_s}, 8, 1073741824.},
		{{u"rad"_s, u"radian"_s}, 9, 1.},
		{{u"deg"_s, u"degree"_s, u"°"_s}, 9, std::numbers::pi / 180.},
	};
	return result;
}

const Unit *FindUnit(const QString &name) {
	for (const auto &unit : Units()) {
		if (unit.names.contains(name.toLower())) return &unit;
	}
	return nullptr;
}

std::optional<QTimeZone> Zone(const QString &name) {
	const auto key = name.trimmed().toLower();
	static const auto aliases = QHash<QString, QByteArray>{
		{u"utc"_s, "UTC"}, {u"gmt"_s, "UTC"},
		{u"msk"_s, "Europe/Moscow"}, {u"мск"_s, "Europe/Moscow"},
		{u"moscow"_s, "Europe/Moscow"}, {u"москва"_s, "Europe/Moscow"},
		{u"london"_s, "Europe/London"}, {u"лондон"_s, "Europe/London"},
		{u"berlin"_s, "Europe/Berlin"}, {u"берлин"_s, "Europe/Berlin"},
		{u"new york"_s, "America/New_York"}, {u"nyc"_s, "America/New_York"},
		{u"tokyo"_s, "Asia/Tokyo"}, {u"токио"_s, "Asia/Tokyo"},
		{u"novosibirsk"_s, "Asia/Novosibirsk"},
		{u"новосибирск"_s, "Asia/Novosibirsk"},
	};
	const auto alias = aliases.constFind(key);
	if (alias != aliases.cend()) return QTimeZone(*alias);
	static const auto offsets = QRegularExpression(
		u"^(?:utc|gmt)([+-])(\\d{1,2})(?::?(\\d{2}))?$"_s);
	const auto offset = offsets.match(key);
	if (offset.hasMatch()) {
		const auto hours = offset.captured(2).toInt();
		const auto minutes = offset.captured(3).toInt();
		if (hours > 14 || minutes > 59 || (hours == 14 && minutes)) return std::nullopt;
		return QTimeZone::fromSecondsAheadOfUtc(
			(hours * 3600 + minutes * 60) * (offset.captured(1) == u"-" ? -1 : 1));
	}
	if (!key.contains(u'/')) return std::nullopt;
	static const auto ids = [] {
		auto result = QHash<QString, QByteArray>();
		for (const auto &id : QTimeZone::availableTimeZoneIds()) {
			result.insert(QString::fromUtf8(id).toLower(), id);
		}
		return result;
	}();
	const auto id = ids.constFind(key);
	return id == ids.cend() ? std::nullopt : std::optional(QTimeZone(*id));
}

std::optional<Result> TimeResult(const QString &text, const QDateTime &now) {
	static const auto pattern = QRegularExpression(
		u"^(?:(\\d{4}-\\d{2}-\\d{2})\\s+)?(\\d{1,2}:\\d{2}|now|сейчас)\\s+(.+?)\\s+(?:to|in|в|->|→)\\s+(.+)$"_s,
		QRegularExpression::CaseInsensitiveOption);
	const auto match = pattern.match(text);
	if (!match.hasMatch()) return std::nullopt;
	const auto source = Zone(match.captured(3));
	const auto target = Zone(match.captured(4));
	if (!source || !target || !source->isValid() || !target->isValid()) return std::nullopt;
	auto moment = now.toTimeZone(*source);
	if (match.captured(2).contains(u':')) {
		const auto date = match.captured(1).isEmpty()
			? moment.date()
			: QDate::fromString(match.captured(1), Qt::ISODate);
		const auto parts = match.captured(2).split(u':');
		const auto time = QTime(parts[0].toInt(), parts[1].toInt());
		if (!date.isValid() || !time.isValid()) return std::nullopt;
		moment = QDateTime(date, time, *source, QDateTime::TransitionResolution::Reject);
	}
	if (!moment.isValid()) return std::nullopt;
	const auto converted = moment.toTimeZone(*target);
	return Result{Kind::TimeZone, converted.toString(u"yyyy-MM-dd HH:mm t"_s)};
}

} // namespace

std::optional<Result> Evaluate(
		const QString &text,
		const Options &options,
		const QHash<QString, double> &usdRates,
		const QDateTime &now) {
	if (text.size() > kMaxLength || text.contains(u'\n')) return std::nullopt;
	auto input = text.trimmed();
	if (input.endsWith(u'=')) input.chop(1);
	input = input.trimmed();
	if (input.isEmpty()) return std::nullopt;
	if (options.timeZones) {
		if (auto result = TimeResult(input, now)) return result;
	}
	static const auto conversion = QRegularExpression(
		u"^([+-]?(?:\\d+(?:[.,]\\d+)?|[.,]\\d+))\\s*([^\\s]+)\\s+(?:to|in|в|->|→)\\s+([^\\s]+)$"_s,
		QRegularExpression::CaseInsensitiveOption);
	const auto match = conversion.match(input);
	if (match.hasMatch()) {
		auto number = match.captured(1);
		number.replace(u',', u'.');
		const auto amount = number.toDouble();
		if (!std::isfinite(amount)) return std::nullopt;
		const auto from = match.captured(2);
		const auto to = match.captured(3);
		if (options.units) {
			const auto source = FindUnit(from);
			const auto target = FindUnit(to);
			if (source && target && source->dimension == target->dimension) {
				const auto base = amount * source->scale + source->offset;
				if (source->dimension == 4 && base < -1e-10) return std::nullopt;
				const auto adjusted = source->dimension == 4 && std::abs(base) < 1e-10 ? 0. : base;
				const auto value = (adjusted - target->offset) / target->scale;
				if (std::isfinite(value)) return Result{Kind::Unit, Number(value) + u' ' + to};
			}
		}
		static const auto code = QRegularExpression(u"^[A-Za-z]{3}$"_s);
		if (options.currency && code.match(from).hasMatch() && code.match(to).hasMatch()) {
			const auto source = usdRates.constFind(from.toLower());
			const auto target = usdRates.constFind(to.toLower());
			if (source == usdRates.cend() || target == usdRates.cend()) {
				return usdRates.isEmpty()
					? std::optional(Result{Kind::Currency, {}, true})
					: std::nullopt;
			}
			if (*source <= 0. || *target <= 0.) return std::nullopt;
			const auto value = amount / *source * *target;
			if (std::isfinite(value)) return Result{Kind::Currency, u"≈ "_s + Number(value) + u' ' + to.toUpper()};
		}
		return std::nullopt;
	}
	if (!options.math) return std::nullopt;
	static const auto meaningful = QRegularExpression(
		u"[+*/^%()×÷π]|[0-9.)]\\s*-|(?:pi|sqrt|sin|cos|tan|log|ln|abs|exp|floor|ceil|round)"_s);
	if (!meaningful.match(input).hasMatch() && !text.trimmed().endsWith(u'=')) return std::nullopt;
	const auto value = Parser(input, options.degrees).parse();
	return value ? std::optional(Result{Kind::Math, Number(*value)}) : std::nullopt;
}

namespace {

constexpr auto kStarUsd = 0.015;

[[nodiscard]] QString SwapLayout(const QString &word, bool toRussian) {
	static const auto latin = u"qwertyuiop[]asdfghjkl;'zxcvbnm,./`"_s;
	static const auto cyrillic = u"йцукенгшщзхъфывапролджэячсмитьбю.ё"_s;
	auto result = word;
	for (auto &ch : result) {
		const auto i = (toRussian ? latin : cyrillic).indexOf(ch);
		if (i >= 0) ch = (toRussian ? cyrillic : latin)[i];
	}
	return result;
}

[[nodiscard]] QString CurrencyCode(const QString &raw, const QHash<QString, double> &rates) {
	static const auto aliases = QHash<QString, QString>{
		{ u"ton"_s, u"ton"_s }, { u"тон"_s, u"ton"_s }, { u"тонкоин"_s, u"ton"_s },
		{ u"toncoin"_s, u"ton"_s }, { u"gram"_s, u"ton"_s }, { u"грам"_s, u"ton"_s }, { u"грамм"_s, u"ton"_s },
		{ u"граммы"_s, u"ton"_s }, { u"грамы"_s, u"ton"_s }, { u"грамов"_s, u"ton"_s }, { u"тона"_s, u"ton"_s }, { u"тонов"_s, u"ton"_s },
		{ u"usdt"_s, u"usdt"_s }, { u"юсдт"_s, u"usdt"_s }, { u"тезер"_s, u"usdt"_s },
		{ u"tether"_s, u"usdt"_s }, { u"тетер"_s, u"usdt"_s },
		{ u"usd"_s, u"usd"_s }, { u"$"_s, u"usd"_s }, { u"доллар"_s, u"usd"_s },
		{ u"доллара"_s, u"usd"_s }, { u"долларов"_s, u"usd"_s }, { u"бакс"_s, u"usd"_s },
		{ u"баксов"_s, u"usd"_s }, { u"баксы"_s, u"usd"_s }, { u"дол"_s, u"usd"_s },
		{ u"rub"_s, u"rub"_s }, { u"₽"_s, u"rub"_s }, { u"руб"_s, u"rub"_s },
		{ u"рубль"_s, u"rub"_s }, { u"рубля"_s, u"rub"_s }, { u"рублей"_s, u"rub"_s },
		{ u"р"_s, u"rub"_s }, { u"деревянных"_s, u"rub"_s },
		{ u"eur"_s, u"eur"_s }, { u"€"_s, u"eur"_s }, { u"евро"_s, u"eur"_s },
		{ u"btc"_s, u"btc"_s }, { u"биткоин"_s, u"btc"_s }, { u"биток"_s, u"btc"_s },
		{ u"eth"_s, u"eth"_s }, { u"эфир"_s, u"eth"_s }, { u"эфириум"_s, u"eth"_s },
		{ u"stars"_s, u"xtr"_s }, { u"star"_s, u"xtr"_s }, { u"xtr"_s, u"xtr"_s },
		{ u"звезд"_s, u"xtr"_s }, { u"звёзд"_s, u"xtr"_s }, { u"звезды"_s, u"xtr"_s },
		{ u"звёзды"_s, u"xtr"_s }, { u"звезда"_s, u"xtr"_s }, { u"звёзда"_s, u"xtr"_s },
		{ u"⭐"_s, u"xtr"_s }, { u"⭐️"_s, u"xtr"_s }, { u"★"_s, u"xtr"_s },
	};
	static const auto latin = QRegularExpression(u"^[a-z]{2,10}$"_s);
	for (const auto &candidate : {
		raw.toLower(),
		SwapLayout(raw.toLower(), true),
		SwapLayout(raw.toLower(), false),
	}) {
		const auto i = aliases.constFind(candidate);
		if (i != aliases.cend()) return *i;
		if (latin.match(candidate).hasMatch() && rates.contains(candidate)) return candidate;
	}
	return QString();
}

[[nodiscard]] QString CurrencyLabel(const QString &code) {
	return (code == u"rub"_s) ? u"₽"_s
		: (code == u"xtr"_s) ? u"⭐"_s
		: code.toUpper();
}

[[nodiscard]] QString CurrencyName(const QString &code) {
	return (code == u"xtr"_s) ? u"STARS"_s : code.toUpper();
}

[[nodiscard]] QString CurrencyEmoji(const QString &code) {
	static const auto emoji = QHash<QString, QString>{
		{ u"ton"_s, u"💎"_s }, { u"usdt"_s, u"💵"_s }, { u"usd"_s, u"💵"_s },
		{ u"rub"_s, u"🇷🇺"_s }, { u"xtr"_s, u"⭐"_s }, { u"eur"_s, u"💶"_s },
		{ u"btc"_s, u"🟠"_s }, { u"eth"_s, u"🔷"_s },
	};
	return emoji.value(code, u"🪙"_s);
}

[[nodiscard]] QString MoneyNumber(double value);

[[nodiscard]] QString MoneyFor(const QString &code, double value) {
	static const auto crypto = QStringList{
		u"ton"_s, u"btc"_s, u"eth"_s, u"not"_s, u"sol"_s, u"bnb"_s, u"doge"_s,
		u"ltc"_s, u"trx"_s, u"xrp"_s, u"ada"_s, u"dot"_s,
	};
	if (code == u"xtr"_s && std::abs(value) >= 1.) {
		return MoneyNumber(std::round(value));
	} else if (!crypto.contains(code) && code != u"xtr"_s && std::abs(value) >= 0.01) {
		auto text = QString::number(std::abs(value), 'f', 2);
		const auto dot = text.indexOf(u'.');
		auto integer = text.left(dot);
		for (auto i = integer.size() - 3; i > 0; i -= 3) integer.insert(i, QChar(0x202F));
		return (value < 0 ? u"-"_s : QString()) + integer + text.mid(dot);
	}
	return MoneyNumber(value);
}

[[nodiscard]] QString MoneyNumber(double value) {
	const auto abs = std::abs(value);
	const auto digits = (abs >= 100.) ? 2 : (abs >= 1.) ? 3 : (abs >= 0.01) ? 5 : 8;
	auto text = QString::number(value, 'f', digits);
	if (text.contains(u'.')) {
		while (text.endsWith(u'0')) text.chop(1);
		if (text.endsWith(u'.')) text.chop(1);
	}
	const auto dot = text.indexOf(u'.');
	auto integer = (dot < 0) ? text : text.left(dot);
	const auto rest = (dot < 0) ? QString() : text.mid(dot);
	const auto negative = integer.startsWith(u'-');
	if (negative) integer.remove(0, 1);
	for (auto i = integer.size() - 3; i > 0; i -= 3) integer.insert(i, QChar(0x202F));
	return (negative ? u"-"_s : QString()) + integer + rest;
}

} // namespace

std::optional<Conversion> ConvertCurrencyCommand(
		const QString &args,
		const QHash<QString, double> &usdRates) {
	static const auto decimalComma = QRegularExpression(u"(?<=\\d),(?=\\d)"_s);
	static const auto separators = QRegularExpression(u"[,;]+|->|→|=|\\s+"_s);
	static const auto linkWords = QStringList{ u"в"_s, u"to"_s, u"in"_s, u"на"_s, u"и"_s, u"and"_s };
	static const auto glued = QRegularExpression(
		u"^(\\d+(?:\\.\\d+)?(?:kk|кк|k|к|m|м)?)([^\\d.].*)$"_s);
	static const auto amountRe = QRegularExpression(
		u"^(\\d+(?:\\.\\d+)?)(kk|кк|k|к|m|м)?$"_s,
		QRegularExpression::CaseInsensitiveOption);
	if (args.size() > 80 || args.contains(u'\n')) return std::nullopt;
	auto text = args.trimmed();
	text.replace(decimalComma, u"."_s);
	auto tokens = QStringList();
	for (const auto &part : text.split(separators, Qt::SkipEmptyParts)) {
		if (linkWords.contains(part.toLower())) continue;
		const auto match = glued.match(part);
		if (!amountRe.match(part).hasMatch() && match.hasMatch()) {
			tokens << match.captured(1) << match.captured(2);
		} else {
			tokens << part;
		}
	}
	if (tokens.isEmpty()) return std::nullopt;
	auto amount = 1.;
	if (const auto match = amountRe.match(tokens.first()); match.hasMatch()) {
		amount = match.captured(1).toDouble();
		const auto suffix = match.captured(2).toLower();
		if (suffix == u"k"_s || suffix == u"к"_s) amount *= 1e3;
		else if (suffix == u"kk"_s || suffix == u"кк"_s || suffix == u"m"_s || suffix == u"м"_s) amount *= 1e6;
		tokens.removeFirst();
	}
	if (tokens.isEmpty() || !std::isfinite(amount)) return std::nullopt;
	auto rates = usdRates;
	if (!rates.isEmpty() && !rates.contains(u"xtr"_s)) rates.insert(u"xtr"_s, 1. / kStarUsd);
	auto codes = QStringList();
	for (const auto &token : tokens) {
		const auto code = CurrencyCode(token, rates);
		if (code.isEmpty()) {
			static const auto looksLikeCode = QRegularExpression(u"^[a-zA-Z]{2,10}$"_s);
			if (rates.isEmpty() && looksLikeCode.match(token).hasMatch()) {
				return Conversion{ {}, {}, true };
			}
			continue;
		}
		codes << code;
	}
	if (codes.isEmpty()) return std::nullopt;
	if (rates.isEmpty()) return Conversion{ {}, {}, true };
	const auto source = codes.takeFirst();
	codes.removeAll(source);
	codes.removeDuplicates();
	if (codes.isEmpty()) {
		for (const auto &code : { u"usdt"_s, u"xtr"_s, u"rub"_s }) {
			if (code != source) codes << code;
		}
	}
	const auto from = rates.value(source);
	if (from <= 0.) return std::nullopt;
	auto parts = QStringList();
	auto lines = QStringList();
	lines << u"🔄 Конвертация "_s + MoneyNumber(amount) + u' ' + CurrencyName(source);
	lines << CurrencyName(source) + u' ' + CurrencyEmoji(source) + u": "_s + MoneyNumber(amount);
	for (const auto &code : codes) {
		const auto to = rates.value(code);
		if (to <= 0.) return std::nullopt;
		const auto value = amount / from * to;
		if (!std::isfinite(value)) return std::nullopt;
		parts << MoneyFor(code, value) + u' ' + CurrencyLabel(code);
		lines << CurrencyName(code) + u' ' + CurrencyEmoji(code) + u": "_s + MoneyFor(code, value);
	}
	return Conversion{ lines.join(u'\n'), parts.join(u" · "_s) };
}

} // namespace Mokagram::Inline
