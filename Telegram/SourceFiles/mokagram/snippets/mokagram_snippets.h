// Mokagram: сниппеты. Пишешь ;имя, и при отправке оно заменяется сохранённым текстом.
#pragma once

#include "ui/text/text_entity.h"

namespace Mokagram::Snippets {

[[nodiscard]] QStringList Names();
[[nodiscard]] QString Get(const QString &name);
void Set(const QString &name, const QString &text);
bool Remove(const QString &name);

// Заменяет ;имя в тексте перед отправкой. Если что-то заменилось, разметка
// сбрасывается: смещения после правки уже неверны.
void Expand(TextWithTags &text);

} // namespace Mokagram::Snippets
