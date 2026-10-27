// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 5e01045f
// Mokagram: английский вариант интерфейса. Русский текст остаётся ключом.
#include "mokagram/lang/mokagram_lang.h"

#include "core/application.h"
#include "lang/lang_keys.h"

#include <QtCore/QProcessEnvironment>

#include <string_view>
#include <unordered_map>

namespace Mokagram::Lang {
namespace {

struct Pair {
	const char *russian;
	const char *english;
};

constexpr Pair kTable[] = {
#include "mokagram/lang/mokagram_lang_table.inc"
};

[[nodiscard]] const std::unordered_map<std::string_view, const char*> &Map() {
	static const auto result = [] {
		auto map = std::unordered_map<std::string_view, const char*>();
		for (const auto &pair : kTable) {
			map.emplace(pair.russian, pair.english);
		}
		return map;
	}();
	return result;
}

// 0 пока язык не известен, 1 русский, 2 остальные.
[[nodiscard]] int Mode() {
	static auto cached = 0;
	if (cached) {
		return cached;
	}
	// MOKAGRAM_LANG=en|ru: принудительный язык, нужен для проверки.
	const auto forced = qEnvironmentVariable("MOKAGRAM_LANG");
	if (!forced.isEmpty()) {
		cached = (forced == u"ru") ? 1 : 2;
		return cached;
	}
	// До запуска приложения язык ещё не известен (статические инициализаторы).
	if (!Core::IsAppLaunched()) {
		return 1;
	}
	const auto id = ::Lang::Id();
	if (id.isEmpty()) {
		// Встроенный английский (языковой пакет не выбран): id пустой, но интерфейс английский.
		// Не запоминаем, чтобы выбор языка позже подхватился.
		return 2;
	}
	cached = (id.startsWith(u"ru") || id.startsWith(u"uk") || id.startsWith(u"be"))
		? 1
		: 2;
	return cached;
}

} // namespace

const char *Tr(const char *russian) {
	if (Mode() != 2) {
		return russian;
	}
	const auto &map = Map();
	const auto i = map.find(std::string_view(russian));
	return (i != map.end()) ? i->second : russian;
}

} // namespace Mokagram::Lang
