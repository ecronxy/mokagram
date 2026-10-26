// Mokagram: сниппеты. Пишешь ;имя, и при отправке оно заменяется сохранённым текстом.
#pragma once

#include "ui/text/text_entity.h"

namespace Mokagram::Snippets {

[[nodiscard]] QStringList Names();
[[nodiscard]] TextWithTags Get(const QString &name);
void Set(const QString &name, const TextWithTags &text);

// Часть текста с разметкой (эмодзи, жирный и т.д.), сдвинутая к нулю.
[[nodiscard]] TextWithTags Slice(
	const TextWithTags &text,
	int from,
	int length);
bool Remove(const QString &name);

// Заменяет ;имя в тексте перед отправкой с сохранением разметки и премиум-эмодзи.
void Expand(TextWithTags &text);

// Шрифт сообщений из настроек: жирный, курсив и т. д. или буквы другого начертания.
// Не трогает команды плагинов (.команда, /команда) и текст, где уже есть своё форматирование.
void ApplyFont(TextWithTags &text);

} // namespace Mokagram::Snippets
