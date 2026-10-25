// Mokagram: английский вариант интерфейса. Русский текст остаётся ключом.
#pragma once

namespace Mokagram::Lang {

// Русский и украинский/белорусский интерфейс Telegram: текст как есть.
// Остальные языки: английский перевод, а если его нет, русский.
[[nodiscard]] const char *Tr(const char *russian);

} // namespace Mokagram::Lang
