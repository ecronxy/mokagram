// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 8fcc96b1
// Mokagram: фильтры сообщений (регулярные выражения, скрытие в ленте чата).
#pragma once

#include "base/basic_types.h"
#include "base/flat_set.h"

#include <rpl/producer.h>

#include <QtCore/QString>

#include <vector>

class HistoryItem;

namespace Mokagram::History {

enum class FilterTarget {
	Text,
	Sender,
	All,
};

struct Filter {
	int id = 0;
	QString pattern;
	bool ignoreCase = true;
	bool reversed = false; // Скрывать то, что НЕ совпало.
	FilterTarget target = FilterTarget::Text;
	base::flat_set<uint64> peers;      // Пусто = везде.
	base::flat_set<uint64> exclusions; // Чаты, где фильтр не действует.
};

constexpr auto kFilterMaxPattern = 500;

// Пустая строка = регулярка корректна, иначе текст ошибки для подсветки.
[[nodiscard]] QString ValidateFilter(const QString &pattern, bool ignoreCase);

// Совпадает ли регулярка с тестовой строкой (для проверки вживую).
[[nodiscard]] bool TestFilter(
	const QString &pattern,
	bool ignoreCase,
	const QString &sample);

[[nodiscard]] std::vector<Filter> Filters();
void ReplaceFilters(std::vector<Filter> filters);
[[nodiscard]] rpl::producer<> FiltersChanged();

// Горячий путь: читает кэш по id сообщения, без регулярок на отрисовке.
[[nodiscard]] bool FilterHidden(not_null<const HistoryItem*> item);

// Сообщение изменилось: результат для него надо пересчитать.
void ForgetFilterResult(not_null<const HistoryItem*> item);

// Переключатель «показать скрытое» для чата (только в памяти сессии).
[[nodiscard]] bool ShowHiddenInChat(uint64 peerId);
void SetShowHiddenInChat(uint64 peerId, bool show);

// Подписка на настройки history.filters_enabled / history.hide_blocked.
void StartFilterSettings();

} // namespace Mokagram::History
