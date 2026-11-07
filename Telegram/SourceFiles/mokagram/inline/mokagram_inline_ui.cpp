#include "mokagram/inline/mokagram_inline_ui.h"

#include "mokagram/inline/mokagram_inline.h"
#include "mokagram/commands/mokagram_commands.h"
#include "mokagram/lang/mokagram_lang.h"
#include "mokagram/settings/mokagram_settings.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/abstract_button.h"
#include "ui/painter.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTimer>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QTextEdit>

#include "styles/style_basic.h"

#include <cmath>

namespace Mokagram::Inline {
namespace {

constexpr auto kDebounceMs = 180;
constexpr auto kRefreshSeconds = 6 * 60 * 60;
constexpr auto kCryptoRefreshSeconds = 120;
constexpr auto kMaxAgeSeconds = 24 * 60 * 60;
constexpr auto kRetrySeconds = 10 * 60;
constexpr auto kMaxReplyBytes = 128 * 1024;

class Rates final : public QObject {
public:
	Rates() : QObject(QCoreApplication::instance()), _manager(this) {
	}

	const QHash<QString, double> &values() const {
		static const auto empty = QHash<QString, double>();
		return QDateTime::currentSecsSinceEpoch() - _fetchedAt <= kMaxAgeSeconds
			? _values
			: empty;
	}

	QString date() const {
		return _date;
	}

	rpl::producer<> changes() const {
		return _changes.events();
	}

	void refreshFresh(qint64 maxAge) {
		const auto now = QDateTime::currentSecsSinceEpoch();
		if (_requesting
			|| now - _attemptAt < 20
			|| (!_values.isEmpty() && now - _fetchedAt < maxAge)) {
			return;
		}
		_requesting = true;
		_attemptAt = now;
		request(false);
	}

	void refresh() {
		const auto now = QDateTime::currentSecsSinceEpoch();
		if (_requesting
			|| now - _attemptAt < kRetrySeconds
			|| (!_values.isEmpty() && now - _fetchedAt < kRefreshSeconds)) {
			return;
		}
		_requesting = true;
		_attemptAt = now;
		request(false);
	}

private:
	void request(bool fallback) {
		auto networkRequest = QNetworkRequest(QUrl(fallback
			? u"https://cdn.jsdelivr.net/npm/@fawazahmed0/currency-api@latest/v1/currencies/usd.min.json"_q
			: u"https://api.coinbase.com/v2/exchange-rates?currency=USD"_q));
		networkRequest.setTransferTimeout(10000);
		networkRequest.setRawHeader("Accept", "application/json");
		networkRequest.setRawHeader("User-Agent", "mokaGram");
		const auto reply = _manager.get(networkRequest);
		QObject::connect(reply, &QNetworkReply::readyRead, this, [=] {
			if (reply->bytesAvailable() > kMaxReplyBytes) reply->abort();
		});
		QObject::connect(reply, &QNetworkReply::finished, this, [=] {
			const auto bytes = reply->read(kMaxReplyBytes + 1);
			const auto root = QJsonDocument::fromJson(bytes).object();
			const auto date = fallback
				? QDate::fromString(root.value(u"date"_q).toString(), Qt::ISODate)
				: QDateTime::currentDateTimeUtc().date();
			const auto today = QDateTime::currentDateTimeUtc().date();
			const auto good = reply->error() == QNetworkReply::NoError
				&& reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
				&& bytes.size() <= kMaxReplyBytes
				&& date.isValid()
				&& date >= today.addDays(-3)
				&& date <= today.addDays(1);
			auto rates = QHash<QString, double>();
			if (good) {
				const auto table = fallback
					? root.value(u"usd"_q).toObject()
					: root.value(u"data"_q).toObject().value(u"rates"_q).toObject();
				for (auto i = table.constBegin(); i != table.constEnd(); ++i) {
					const auto value = fallback ? i.value().toDouble() : i.value().toString().toDouble();
					if (value > 0. && std::isfinite(value)) rates.insert(i.key().toLower(), value);
				}
			}
			reply->deleteLater();
			if (rates.size() < 20 || std::abs(rates.value(u"usd"_q) - 1.) > .000001) {
				if (!fallback) {
					request(true);
					return;
				}
			} else {
				_values = std::move(rates);
				_fetchedAt = QDateTime::currentSecsSinceEpoch();
				_date = date.toString(Qt::ISODate);
			}
			_requesting = false;
			_changes.fire({});
		});
	}

	QNetworkAccessManager _manager;
	QHash<QString, double> _values;
	QString _date;
	qint64 _fetchedAt = 0;
	qint64 _attemptAt = 0;
	bool _requesting = false;
	rpl::event_stream<> _changes;

};

Rates &CurrencyRates() {
	static const auto instance = new Rates();
	return *instance;
}

class Hint final : public Ui::AbstractButton {
public:
	Hint(not_null<Ui::InputField*> field)
	: Ui::AbstractButton(field->rawTextEdit()->viewport())
	, _field(field) {
		setFocusPolicy(Qt::NoFocus);
		setAttribute(Qt::WA_TranslucentBackground);
		setPointerCursor(true);
		hide();
		_timer.setSingleShot(true);
		QObject::connect(&_timer, &QTimer::timeout, this, [=] { evaluate(); });
		_clock.setInterval(60000);
		QObject::connect(&_clock, &QTimer::timeout, this, [=] {
			if (isVisible()) evaluate();
		});
		_clock.start();
		setClickedCallback([=] { accept(); });
		field->changes() | rpl::on_next([=] {
			hide();
			_timer.start(kDebounceMs);
		}, lifetime());
		field->focusedChanges() | rpl::on_next([=] { updatePosition(); }, lifetime());
		field->sizeValue() | rpl::on_next([=] { updatePosition(); }, lifetime());
		field->scrollTop().changes() | rpl::on_next([=] { updatePosition(); }, lifetime());
		Settings::Changed() | rpl::on_next([=](const QString &key) {
			if (key.startsWith(u"inline."_q)) evaluate();
		}, lifetime());
		CurrencyRates().changes() | rpl::on_next([=] { evaluate(); }, lifetime());
		QObject::connect(field->rawTextEdit(), &QTextEdit::cursorPositionChanged,
			this, [=] { updatePosition(); });
		field->rawTextEdit()->installEventFilter(this);
		field->rawTextEdit()->viewport()->installEventFilter(this);
	}

protected:
	bool eventFilter(QObject *object, QEvent *event) override {
		if (event->type() == QEvent::InputMethod) {
			_composing = !static_cast<QInputMethodEvent*>(event)->preeditString().isEmpty();
			if (_composing) hide();
			else _timer.start(kDebounceMs);
		} else if (event->type() == QEvent::Resize) {
			updatePosition();
		} else if (event->type() == QEvent::KeyPress) {
			const auto key = static_cast<QKeyEvent*>(event);
			if (key->key() == Qt::Key_Tab
				&& key->modifiers() == Qt::NoModifier
				&& isVisible()) {
				accept();
				return true;
			}
		}
		return Ui::AbstractButton::eventFilter(object, event);
	}

	void paintEvent(QPaintEvent*) override {
		Painter p(this);
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(rect(), Qt::AlignVCenter | Qt::AlignLeft, _display);
	}

private:
	void evaluate() {
		_value.clear();
		if (!Settings::Bool(u"inline.enabled"_q) || _composing) {
			hide();
			return;
		}
		const auto text = _field->getTextWithTags().text;
		if (!_field->getTextWithTags().tags.empty()) {
			hide();
			return;
		}
		const auto options = Options{
			.math = Settings::Bool(u"inline.math"_q),
			.units = Settings::Bool(u"inline.units"_q),
			.currency = Settings::Bool(u"inline.currency"_q),
			.timeZones = Settings::Bool(u"inline.timezones"_q),
			.degrees = Settings::Bool(u"inline.degrees"_q),
		};
		if (const auto calc = Commands::CalcPreview(text)) {
			_query = text;
			_value = calc->second;
			_accept = calc->first + u" = "_q + calc->second;
			setAccessibleName(_value);
			setToolTip(QString::fromUtf8(Lang::Tr("Нажмите или Tab, чтобы вставить результат")));
			updatePosition();
			return;
		}
		if (const auto cr = Commands::CrPreview(text)) {
			CurrencyRates().refreshFresh(kCryptoRefreshSeconds);
			if (cr->needsRates) {
				hide();
				return;
			}
			_query = text;
			_value = cr->compact;
			_accept = cr->text;
			setAccessibleName(_value);
			setToolTip(QString::fromUtf8(Lang::Tr("Нажмите или Tab, чтобы вставить результат")));
			updatePosition();
			return;
		}
		_accept.clear();
		const auto result = Evaluate(text, options, CurrencyRates().values());
		if (!result || result->needsRates) {
			hide();
			if (result && result->needsRates) CurrencyRates().refresh();
			return;
		}
		_query = text;
		_value = result->value;
		setAccessibleName(_value);
		auto tooltip = QString::fromUtf8(Lang::Tr("Нажмите или Tab, чтобы вставить результат"));
		if (result->kind == Kind::Currency) {
			tooltip += u"\n"_q + QString::fromUtf8(Lang::Tr("Курс получен"))
				+ u' ' + CurrencyRates().date();
			CurrencyRates().refresh();
		}
		setToolTip(tooltip);
		updatePosition();
	}

	void updatePosition() {
		const auto edit = _field->rawTextEdit();
		const auto cursor = edit->textCursor();
		if (_value.isEmpty() || _composing || !_field->hasFocus()
			|| cursor.hasSelection() || !cursor.atEnd()
			|| edit->document()->blockCount() != 1) {
			hide();
			return;
		}
		const auto caret = edit->cursorRect();
		const auto x = caret.right() + st::lineWidth;
		const auto available = edit->viewport()->width() - x;
		const auto full = u" = "_q + _value;
		if (available < st::normalFont->width(u" = …"_q)) {
			hide();
			return;
		}
		_display = st::normalFont->elided(full, available);
		setGeometry(x, caret.top(), st::normalFont->width(_display), caret.height());
		show();
		raise();
		update();
	}

	void accept() {
		if (_value.isEmpty() || _query != _field->getTextWithTags().text) return;
		const auto value = _accept.isEmpty() ? _value : _accept;
		_field->setTextWithTags({value, {}}, Ui::InputField::HistoryAction::NewEntry);
		_field->setCursorPosition(value.size());
		_field->setFocus();
		hide();
	}

	const not_null<Ui::InputField*> _field;
	QTimer _timer;
	QTimer _clock;
	QString _query;
	QString _value;
	QString _accept;
	QString _display;
	bool _composing = false;

};

} // namespace

const QHash<QString, double> &CurrencyRatesNow() {
	return CurrencyRates().values();
}

void RefreshCurrencyRates() {
	CurrencyRates().refreshFresh(kCryptoRefreshSeconds);
}

void SetupInlineResults(not_null<Ui::InputField*> field) {
	if (field->property("mokagramInlineResults").toBool()) return;
	field->setProperty("mokagramInlineResults", true);
	RefreshCurrencyRates();
	new Hint(field);
}

} // namespace Mokagram::Inline
