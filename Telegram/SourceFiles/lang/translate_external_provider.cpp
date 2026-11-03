#include "lang/translate_external_provider.h"

#include "mokagram/settings/mokagram_settings.h"
#include "mokagram/lang/mokagram_lang.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrlQuery>
#include <QtCore/QUuid>
#include <QtGui/QTextDocumentFragment>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLineEdit>

#include <algorithm>
#include <deque>

namespace Ui {
namespace {

constexpr auto kChunkChars = 1000;
constexpr auto kResponseLimit = 2 * 1024 * 1024;
constexpr auto kMaxPending = 128;
QString MicrosoftKey;

struct Segment {
	QString original;
	std::vector<int> entities;
	bool unchanged = false;
};

struct FormattedPart {
	QString html;
	std::vector<Segment> segments;
	EntitiesInText entities;
};

[[nodiscard]] bool TranslateEntity(EntityType type) {
	switch (type) {
	case EntityType::Bold:
	case EntityType::Semibold:
	case EntityType::Italic:
	case EntityType::Underline:
	case EntityType::StrikeOut:
	case EntityType::Spoiler:
	case EntityType::Blockquote:
	case EntityType::CustomUrl:
	case EntityType::Subscript:
	case EntityType::Superscript:
	case EntityType::Marked:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] FormattedPart Encode(const TextWithEntities &source) {
	auto result = FormattedPart{ .entities = source.entities };
	auto leading = 0;
	while (leading < source.text.size() && source.text[leading].isSpace()) {
		++leading;
	}
	auto trailing = int(source.text.size());
	while (trailing > leading && source.text[trailing - 1].isSpace()) {
		--trailing;
	}
	auto cuts = std::vector<int>{ 0, leading, trailing, int(source.text.size()) };
	for (const auto &entity : source.entities) {
		if (entity.validForText(source.text.size())) {
			cuts.push_back(entity.offset());
			cuts.push_back(entity.offset() + entity.length());
		}
	}
	std::sort(cuts.begin(), cuts.end());
	cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
	for (auto i = size_t(1); i != cuts.size(); ++i) {
		const auto from = cuts[i - 1];
		const auto till = cuts[i];
		auto segment = Segment{ .original = source.text.mid(from, till - from) };
		segment.unchanged = segment.original.trimmed().isEmpty();
		for (auto j = 0; j != source.entities.size(); ++j) {
			const auto &entity = source.entities[j];
			if (entity.validForText(source.text.size())
				&& entity.offset() <= from
				&& entity.offset() + entity.length() >= till) {
				segment.entities.push_back(j);
				segment.unchanged |= !TranslateEntity(entity.type());
			}
		}
		result.html += u"<span id=\"moka%1\"%2>"_q
			.arg(result.segments.size())
			.arg(segment.unchanged
				? u" translate=\"no\" class=\"notranslate\""_q
				: QString());
		result.html += segment.original.toHtmlEscaped()
			.replace('\n', u"<br/>"_q)
			.replace('\r', u"&#13;"_q)
			.replace('\t', u"&#9;"_q);
		result.html += u"</span>"_q;
		result.segments.push_back(std::move(segment));
	}
	return result;
}

[[nodiscard]] std::optional<TextWithEntities> Decode(
		const FormattedPart &part,
		const QString &html) {
	struct OpenSpan {
		int segment = -1;
		int from = 0;
	};
	auto result = TextWithEntities();
	auto stack = std::vector<OpenSpan>();
	auto seen = std::vector<bool>(part.segments.size());
	auto ranges = std::vector<std::pair<int, int>>(
		part.entities.size(), { -1, -1 });
	const auto tagPattern = QRegularExpression(u"^\\s*(/?)\\s*([a-zA-Z0-9]+)"_q);
	const auto idPattern = QRegularExpression(
		u"(?:^|\\s)id\\s*=\\s*['\"]moka([0-9]+)['\"]"_q);
	for (auto at = qsizetype(0); at < html.size();) {
		if (html[at] != '<') {
			const auto end = html.indexOf('<', at);
			const auto till = end < 0 ? html.size() : end;
			result.text += QTextDocumentFragment::fromHtml(
				u"<pre>"_q + html.mid(at, till - at) + u"</pre>"_q).toPlainText();
			at = till;
			continue;
		}
		if (html.mid(at, 4) == u"<!--"_q) {
			const auto end = html.indexOf(u"-->"_q, at + 4);
			if (end < 0) {
				return std::nullopt;
			}
			at = end + 3;
			continue;
		}
		auto end = at + 1;
		auto quote = QChar();
		for (; end < html.size(); ++end) {
			const auto ch = html[end];
			if (!quote.isNull()) {
				if (ch == quote) {
					quote = QChar();
				}
			} else if (ch == '\'' || ch == '"') {
				quote = ch;
			} else if (ch == '>') {
				break;
			}
		}
		if (end == html.size()) {
			return std::nullopt;
		}
		const auto tag = html.mid(at + 1, end - at - 1);
		const auto match = tagPattern.match(tag);
		const auto closing = !match.captured(1).isEmpty();
		const auto name = match.captured(2).toLower();
		if (name == u"br"_q && !closing) {
			result.text += '\n';
		} else if (name == u"span"_q) {
			if (closing) {
				if (stack.empty()) {
					return std::nullopt;
				}
				const auto span = stack.back();
				stack.pop_back();
				if (span.segment >= 0) {
					const auto &segment = part.segments[span.segment];
					if (segment.unchanged) {
						result.text.truncate(span.from);
						result.text += segment.original;
					}
					for (const auto index : segment.entities) {
						auto &range = ranges[index];
						range.first = range.first < 0
							? span.from
							: qMin(range.first, span.from);
						range.second = qMax(range.second, int(result.text.size()));
					}
				}
			} else {
				const auto id = idPattern.match(tag);
				auto segment = -1;
				if (id.hasMatch()) {
					auto ok = false;
					segment = id.captured(1).toInt(&ok);
					if (!ok || segment >= int(seen.size()) || seen[segment]) {
						return std::nullopt;
					}
					seen[segment] = true;
				}
				stack.push_back({ segment, int(result.text.size()) });
			}
		}
		at = end + 1;
	}
	if (!stack.empty() || std::find(seen.begin(), seen.end(), false) != seen.end()) {
		return std::nullopt;
	}
	for (auto i = 0; i != int(ranges.size()); ++i) {
		const auto [from, till] = ranges[i];
		if (from >= 0 && till > from) {
			const auto &entity = part.entities[i];
			result.entities.push_back(EntityInText(
				entity.type(), from, till - from, entity.data()));
		}
	}
	return result.text.isEmpty() ? std::nullopt : std::make_optional(result);
}

[[nodiscard]] std::vector<FormattedPart> Split(const TextWithEntities &source) {
	auto result = std::vector<FormattedPart>();
	for (auto from = 0, size = int(source.text.size()); from < size;) {
		auto width = qMin(kChunkChars, size - from);
		for (;;) {
			auto till = from + width;
			if (till < size && source.text[till].isLowSurrogate()) {
				--till;
			}
			if (till < size) {
				auto boundary = till;
				while (boundary > from + width * 3 / 4
					&& !source.text[boundary - 1].isSpace()) {
					--boundary;
				}
				if (boundary > from + width * 3 / 4) {
					till = boundary;
				}
			}
			auto slice = TextWithEntities{ .text = source.text.mid(from, till - from) };
			for (const auto &entity : source.entities) {
				if (!entity.validForText(size)) {
					continue;
				}
				const auto begin = qMax(entity.offset(), from);
				const auto end = qMin(entity.offset() + entity.length(), till);
				if (end > begin) {
					slice.entities.push_back(EntityInText(
						entity.type(), begin - from, end - begin, entity.data()));
				}
			}
			auto encoded = Encode(slice);
			if (encoded.html.size() <= 4500 || width <= 2) {
				result.push_back(std::move(encoded));
				from = till;
				break;
			}
			width /= 2;
		}
	}
	return result;
}

class ExternalTranslateProvider final : public TranslateProvider {
public:
	explicit ExternalTranslateProvider(ExternalTranslateService service)
	: _service(service) {
		_network.setTransferTimeout(15000);
	}

	bool supportsMessageId() const override {
		return false;
	}

	void request(
			TranslateProviderRequest request,
			LanguageId to,
			Fn<void(TranslateProviderResult)> done) override {
		if (request.text.text.isEmpty()
			|| request.text.text.size() > 65536
			|| _jobs.size() >= kMaxPending
			|| (_service == ExternalTranslateService::Microsoft
				&& MicrosoftKey.isEmpty())) {
			done({ .error = TranslateProviderError::Unknown });
			return;
		}
		auto job = std::make_shared<Job>();
		job->parts = Split(request.text);
		job->language = to.twoLetterCode();
		job->done = std::move(done);
		_jobs.push_back(std::move(job));
		if (!_busy) {
			sendNext();
		}
	}

private:
	struct Job {
		std::vector<FormattedPart> parts;
		QString language;
		Fn<void(TranslateProviderResult)> done;
		TextWithEntities result;
		size_t part = 0;
	};

	void finish(TranslateProviderResult result) {
		const auto job = _jobs.front();
		_jobs.pop_front();
		_busy = false;
		const auto weak = QPointer(&_network);
		job->done(std::move(result));
		if (weak && !_busy && !_jobs.empty()) {
			sendNext();
		}
	}

	void sendNext() {
		_busy = true;
		sendPart();
	}

	void sendPart() {
		const auto job = _jobs.front();
		const auto &html = job->parts[job->part].html;
		auto url = QUrl();
		auto query = QUrlQuery();
		auto form = QUrlQuery();
		auto body = QByteArray();
		if (_service == ExternalTranslateService::Google) {
			url = QUrl(u"https://translate.googleapis.com/translate_a/t"_q);
			query.addQueryItem(u"client"_q, u"gtx"_q);
			query.addQueryItem(u"sl"_q, u"auto"_q);
			query.addQueryItem(u"tl"_q, job->language);
			query.addQueryItem(u"dt"_q, u"t"_q);
			query.addQueryItem(u"format"_q, u"html"_q);
			query.addQueryItem(u"ie"_q, u"UTF-8"_q);
			query.addQueryItem(u"oe"_q, u"UTF-8"_q);
			form.addQueryItem(u"q"_q, html);
		} else if (_service == ExternalTranslateService::Yandex) {
			url = QUrl(u"https://translate.yandex.net/api/v1/tr.json/translate"_q);
			query.addQueryItem(u"srv"_q, u"android"_q);
			query.addQueryItem(u"id"_q, QUuid::createUuid()
				.toString(QUuid::Id128) + u"-0-0"_q);
			form.addQueryItem(u"lang"_q, job->language);
			form.addQueryItem(u"format"_q, u"html"_q);
			form.addQueryItem(u"text"_q, html);
		} else {
			url = QUrl(u"https://api.cognitive.microsofttranslator.com/translate"_q);
			query.addQueryItem(u"api-version"_q, u"3.0"_q);
			query.addQueryItem(u"to"_q, job->language);
			query.addQueryItem(u"textType"_q, u"html"_q);
			body = QJsonDocument(QJsonArray{
				QJsonObject{ { u"Text"_q, html } },
			}).toJson(QJsonDocument::Compact);
		}
		url.setQuery(query);
		auto request = QNetworkRequest(url);
		request.setHeader(QNetworkRequest::UserAgentHeader, u"mokaGram Desktop"_q);
		if (_service == ExternalTranslateService::Microsoft) {
			request.setRawHeader("Ocp-Apim-Subscription-Key",
				MicrosoftKey.toUtf8());
			const auto region = Mokagram::Settings::String(
				u"translation.microsoftRegion"_q).trimmed();
			if (!region.isEmpty()) {
				request.setRawHeader("Ocp-Apim-Subscription-Region", region.toUtf8());
			}
			request.setHeader(QNetworkRequest::ContentTypeHeader,
				u"application/json; charset=UTF-8"_q);
		} else {
			body = form.query(QUrl::FullyEncoded).toUtf8().replace("+", "%2B");
			request.setHeader(QNetworkRequest::ContentTypeHeader,
				u"application/x-www-form-urlencoded; charset=UTF-8"_q);
		}
		const auto reply = _network.post(request, body);
		QObject::connect(reply, &QNetworkReply::finished, &_network, [=] {
			const auto bytes = reply->readAll();
			const auto error = reply->error();
			reply->deleteLater();
			if (error != QNetworkReply::NoError || bytes.size() > kResponseLimit) {
				finish({ .error = TranslateProviderError::Unknown });
				return;
			}
			const auto parsed = QJsonDocument::fromJson(bytes);
			auto translated = QString();
			if (_service == ExternalTranslateService::Yandex) {
				const auto object = parsed.object();
				if (object.value(u"code"_q).toInt() == 200) {
					translated = object.value(u"text"_q).toArray().at(0).toString();
				}
			} else if (_service == ExternalTranslateService::Microsoft) {
				translated = parsed.array().at(0).toObject()
					.value(u"translations"_q).toArray().at(0).toObject()
					.value(u"text"_q).toString();
			} else {
				const auto first = parsed.array().at(0);
				translated = first.isArray()
					? first.toArray().at(0).toString()
					: first.toString();
			}
			auto decoded = Decode(job->parts[job->part], translated);
			if (!decoded) {
				finish({ .error = TranslateProviderError::Unknown });
				return;
			}
			job->result.append(std::move(*decoded));
			if (++job->part == job->parts.size()) {
				finish({ .text = std::move(job->result) });
			} else {
				sendNext();
			}
		});
	}

	const ExternalTranslateService _service;
	QNetworkAccessManager _network;
	std::deque<std::shared_ptr<Job>> _jobs;
	bool _busy = false;

};

} // namespace

void ConfigureMicrosoftTranslation() {
	const auto dialog = new QInputDialog(QApplication::activeWindow());
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowTitle(QString::fromUtf8(Mokagram::Lang::Tr("Ключ Microsoft Translator")));
	dialog->setLabelText(QString::fromUtf8(Mokagram::Lang::Tr(
		"Ключ Azure Translator хранится в памяти до выхода из mokaGram. Пустое значение удаляет ключ.")));
	dialog->setTextEchoMode(QLineEdit::Password);
	QObject::connect(dialog, &QInputDialog::textValueSelected, dialog,
		[](const QString &value) { MicrosoftKey = value.trimmed().left(256); });
	dialog->open();
}

std::unique_ptr<TranslateProvider> CreateExternalTranslateProvider(
		ExternalTranslateService service) {
	return std::make_unique<ExternalTranslateProvider>(service);
}

} // namespace Ui
