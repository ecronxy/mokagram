// Mokagram: фильтры сообщений, реализация.
#include "mokagram/history/mokagram_history_filters.h"
#include "mokagram/lang/mokagram_lang.h"
#include "mokagram/plugins/mokagram_plugin_api.h"

#include "mokagram/settings/mokagram_settings.h"

#include "base/flat_map.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "history/history_item.h"
#include "logs.h"
#include "settings.h"

#include <rpl/event_stream.h>

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QRegularExpression>

#include <memory>

namespace Mokagram::History {
namespace {

constexpr auto kMaxTextLength = 4096;
constexpr auto kCacheLimit = 20000;

enum Result : uchar {
	kNone = 0,
	kHide = 1,
};

struct Compiled {
	QRegularExpression regex;
	FilterTarget target = FilterTarget::Text;
	bool reversed = false;
	base::flat_set<uint64> peers;
	base::flat_set<uint64> exclusions;
};

// Иммутабельный набор: строится при каждом изменении и подменяется целиком.
struct CompiledSet {
	uint32 version = 0;
	std::vector<Compiled> items;
};

struct CacheEntry {
	uint32 version = 0;
	uchar result = kNone;
};

struct State {
	std::vector<Filter> filters;
	std::shared_ptr<const CompiledSet> compiled;
	base::flat_map<FullMsgId, CacheEntry> cache;
	base::flat_set<uint64> showHidden;
	rpl::event_stream<> changed;
	rpl::lifetime lifetime;
	uint32 version = 0;
	int nextId = 1;
	bool loaded = false;
	bool enabled = false;
	bool hideBlocked = false;
};

[[nodiscard]] State &Data() {
	static auto result = State();
	return result;
}

[[nodiscard]] QString FilePath() {
	return cWorkingDir() + u"tdata/mokagram_filters.json"_q;
}

[[nodiscard]] QRegularExpression Make(
		const QString &pattern,
		bool ignoreCase) {
	auto options = QRegularExpression::PatternOptions(
		QRegularExpression::UseUnicodePropertiesOption);
	if (ignoreCase) {
		options |= QRegularExpression::CaseInsensitiveOption;
	}
	return QRegularExpression(pattern, options);
}

[[nodiscard]] bool Usable(const Filter &filter) {
	return !filter.pattern.isEmpty()
		&& filter.pattern.size() <= kFilterMaxPattern
		&& Make(filter.pattern, filter.ignoreCase).isValid();
}

void Compile(State &data) {
	auto set = std::make_shared<CompiledSet>();
	set->version = ++data.version;

	// Простые фильтры по тексту без областей склеиваются в один объект:
	// один проход по тексту вместо N.
	auto mergedCase = QStringList();
	auto mergedNoCase = QStringList();
	for (const auto &filter : data.filters) {
		if (!Usable(filter)) {
			continue;
		}
		const auto simple = !filter.reversed
			&& filter.target == FilterTarget::Text
			&& filter.peers.empty()
			&& filter.exclusions.empty();
		if (simple) {
			(filter.ignoreCase ? mergedNoCase : mergedCase).push_back(
				u"(?:%1)"_q.arg(filter.pattern));
			continue;
		}
		auto &item = set->items.emplace_back();
		item.regex = Make(filter.pattern, filter.ignoreCase);
		item.regex.optimize();
		item.target = filter.target;
		item.reversed = filter.reversed;
		item.peers = filter.peers;
		item.exclusions = filter.exclusions;
	}
	const auto addMerged = [&](const QStringList &list, bool ignoreCase) {
		if (list.isEmpty()) {
			return;
		}
		auto regex = Make(list.join('|'), ignoreCase);
		if (!regex.isValid()) {
			// Склейка сломалась (например, нумерация групп): по одному.
			for (const auto &single : list) {
				auto &item = set->items.emplace_back();
				item.regex = Make(single, ignoreCase);
			}
			return;
		}
		regex.optimize();
		auto &item = set->items.emplace_back();
		item.regex = std::move(regex);
	};
	addMerged(mergedCase, false);
	addMerged(mergedNoCase, true);
	data.compiled = std::move(set);
	data.cache.clear();
}

[[nodiscard]] QJsonObject ToJson(const Filter &filter) {
	const auto list = [](const base::flat_set<uint64> &set) {
		auto result = QJsonArray();
		for (const auto id : set) {
			result.push_back(QString::number(id));
		}
		return result;
	};
	auto result = QJsonObject();
	result.insert(u"id"_q, filter.id);
	result.insert(u"pattern"_q, filter.pattern);
	result.insert(u"ignoreCase"_q, filter.ignoreCase);
	result.insert(u"reversed"_q, filter.reversed);
	result.insert(u"target"_q, int(filter.target));
	result.insert(u"peers"_q, list(filter.peers));
	result.insert(u"exclusions"_q, list(filter.exclusions));
	return result;
}

[[nodiscard]] Filter FromJson(const QJsonObject &object) {
	const auto list = [&](const QString &key) {
		auto result = base::flat_set<uint64>();
		for (const auto &value : object.value(key).toArray()) {
			result.emplace(value.toString().toULongLong());
		}
		return result;
	};
	auto result = Filter();
	result.id = object.value(u"id"_q).toInt();
	result.pattern = object.value(u"pattern"_q).toString();
	result.ignoreCase = object.value(u"ignoreCase"_q).toBool(true);
	result.reversed = object.value(u"reversed"_q).toBool(false);
	const auto target = object.value(u"target"_q).toInt();
	result.target = (target >= 0 && target <= int(FilterTarget::All))
		? FilterTarget(target)
		: FilterTarget::Text;
	result.peers = list(u"peers"_q);
	result.exclusions = list(u"exclusions"_q);
	return result;
}

void EnsureLoaded() {
	auto &data = Data();
	if (data.loaded) {
		return;
	}
	data.loaded = true;
	auto file = QFile(FilePath());
	if (file.open(QIODevice::ReadOnly)) {
		const auto document = QJsonDocument::fromJson(file.readAll());
		for (const auto &value : document.object().value(u"filters"_q).toArray()) {
			auto filter = FromJson(value.toObject());
			if (filter.id <= 0) {
				filter.id = data.nextId;
			}
			data.nextId = std::max(data.nextId, filter.id + 1);
			data.filters.push_back(std::move(filter));
		}
	}
	Compile(data);
}

void Save() {
	auto array = QJsonArray();
	for (const auto &filter : Data().filters) {
		array.push_back(ToJson(filter));
	}
	auto root = QJsonObject();
	root.insert(u"version"_q, 1);
	root.insert(u"filters"_q, array);
	auto file = QSaveFile(FilePath());
	if (!file.open(QIODevice::WriteOnly)) {
		LOG(("[mokagram] Filters: cannot write mokagram_filters.json."));
		return;
	}
	file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
	if (!file.commit()) {
		LOG(("[mokagram] Filters: commit failed."));
	}
}

[[nodiscard]] bool Matches(
		const Compiled &item,
		QStringView text,
		QStringView sender,
		uint64 peer) {
	if (!item.peers.empty() && !item.peers.contains(peer)) {
		return false;
	} else if (item.exclusions.contains(peer)) {
		return false;
	}
	auto found = false;
	switch (item.target) {
	case FilterTarget::Text:
		found = item.regex.match(text).hasMatch();
		break;
	case FilterTarget::Sender:
		found = item.regex.match(sender).hasMatch();
		break;
	case FilterTarget::All:
		found = item.regex.match(text).hasMatch()
			|| item.regex.match(sender).hasMatch();
		break;
	}
	return found != item.reversed;
}

[[nodiscard]] uchar Compute(
		not_null<const HistoryItem*> item,
		const CompiledSet &set) {
	const auto peer = item->history()->peer->id.value;
	const auto &original = item->originalText().text;
	const auto text = QStringView(original).left(kMaxTextLength);
	const auto name = item->from()->name();
	for (const auto &compiled : set.items) {
		if (Matches(compiled, text, QStringView(name), peer)) {
			return kHide;
		}
	}
	return kNone;
}

} // namespace

QString ValidateFilter(const QString &pattern, bool ignoreCase) {
	if (pattern.isEmpty()) {
		return QString::fromUtf8(Mokagram::Lang::Tr("Пустой шаблон"));
	} else if (pattern.size() > kFilterMaxPattern) {
		return QString::fromUtf8(Mokagram::Lang::Tr("Слишком длинный шаблон"));
	}
	const auto regex = Make(pattern, ignoreCase);
	return regex.isValid() ? QString() : regex.errorString();
}

bool TestFilter(
		const QString &pattern,
		bool ignoreCase,
		const QString &sample) {
	if (!ValidateFilter(pattern, ignoreCase).isEmpty()) {
		return false;
	}
	return Make(pattern, ignoreCase).match(sample).hasMatch();
}

std::vector<Filter> Filters() {
	EnsureLoaded();
	return Data().filters;
}

void ReplaceFilters(std::vector<Filter> filters) {
	EnsureLoaded();
	auto &data = Data();
	for (auto &filter : filters) {
		if (filter.id <= 0) {
			filter.id = data.nextId++;
		}
		data.nextId = std::max(data.nextId, filter.id + 1);
	}
	data.filters = std::move(filters);
	Compile(data);
	Save();
	data.changed.fire({});
}

rpl::producer<> FiltersChanged() {
	return Data().changed.events();
}

bool FilterHidden(not_null<const HistoryItem*> item) {
	auto &data = Data();
	const auto plugins = Plugins::HasMessageFilter();
	if (!data.enabled && !data.hideBlocked && !plugins) {
		return false;
	} else if (item->out() || item->isService() || item->isLocal()) {
		return false;
	}
	const auto peerId = item->history()->peer->id.value;
	if (!data.showHidden.empty() && data.showHidden.contains(peerId)) {
		return false;
	}
	if (plugins && Plugins::HiddenByPlugin(item)) {
		return true;
	}
	if (data.hideBlocked && item->from()->isBlocked()) {
		return true;
	}
	if (!data.enabled) {
		return false;
	}
	EnsureLoaded();
	if (!data.compiled || data.compiled->items.empty()) {
		return false;
	}
	const auto id = item->fullId();
	const auto i = data.cache.find(id);
	if (i != data.cache.end() && i->second.version == data.compiled->version) {
		return i->second.result == kHide;
	}
	const auto result = Compute(item, *data.compiled);
	if (data.cache.size() >= kCacheLimit) {
		data.cache.clear();
	}
	data.cache[id] = CacheEntry{ data.compiled->version, result };
	return result == kHide;
}

void ForgetFilterResult(not_null<const HistoryItem*> item) {
	auto &data = Data();
	if (!data.cache.empty()) {
		data.cache.remove(item->fullId());
	}
}

bool ShowHiddenInChat(uint64 peerId) {
	return Data().showHidden.contains(peerId);
}

void SetShowHiddenInChat(uint64 peerId, bool show) {
	auto &data = Data();
	if (show ? data.showHidden.emplace(peerId).second : data.showHidden.remove(peerId)) {
		data.changed.fire({});
	}
}

void StartFilterSettings() {
	auto &data = Data();
	data.lifetime.destroy();
	Settings::BoolValue(u"history.filters_enabled"_q) | rpl::on_next([](bool value) {
		auto &data = Data();
		data.enabled = value;
		data.changed.fire({});
	}, data.lifetime);
	Settings::BoolValue(u"history.hide_blocked"_q) | rpl::on_next([](bool value) {
		auto &data = Data();
		data.hideBlocked = value;
		data.changed.fire({});
	}, data.lifetime);
}

} // namespace Mokagram::History
