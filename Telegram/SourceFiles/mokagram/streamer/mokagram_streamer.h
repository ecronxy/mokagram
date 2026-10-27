// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 889bc569
// Mokagram: режим стримера. Маскирует номер, юзернейм, ссылки, ID, имя
// и уведомления. Флаги лежат в реестре настроек (ключи "streamer.*").
// Не зависит от Main::Session, вызывается из любого кода.
#pragma once

#include "base/basic_types.h"
#include "ui/text/text_entity.h"

#include <rpl/combine.h>
#include <rpl/map.h>
#include <rpl/producer.h>

namespace Mokagram::Streamer {

enum class Field {
	Phone,
	Username,
	Link,
	Id,
	Name,
	Notify,
};

// Регистрирует записи раздела «Режим стримера» в реестре настроек.
void RegisterSettings();

// Скрывается ли поле сейчас: главный переключатель и галочка поля.
[[nodiscard]] bool Active(Field field);
// То же, но реактивно (distinct_until_changed).
[[nodiscard]] rpl::producer<bool> ActiveValue(Field field);
// Любое изменение флагов режима стримера (для не реактивных мест).
[[nodiscard]] rpl::producer<> Changed();

// Маска с сохранением длины и знаков. Без проверки Active.
[[nodiscard]] QString Mask(Field field, const QString &real);
[[nodiscard]] TextWithEntities Mask(Field field, TextWithEntities real);
// https://t.me/name -> https://t.me/••••••
[[nodiscard]] QString MaskUrl(const QString &url);

// Что положить в буфер: настоящее, маска или пусто (если копирование запрещено).
[[nodiscard]] QString ForCopy(Field field, const QString &real);
// Запрещено ли копировать это поле сейчас. Если да, сразу показывает тост.
[[nodiscard]] bool CopyBlocked(Field field);
// Копирует в буфер с учётом режима. При запрете показывает тост и возвращает false.
bool SetClipboard(Field field, const QString &real);

// Подмешивает ActiveValue: при активном поле отдаёт маску.
template <typename T, typename Generator>
[[nodiscard]] rpl::producer<T> Wrap(
		Field field,
		rpl::producer<T, rpl::no_error, Generator> real) {
	return rpl::combine(
		std::move(real),
		ActiveValue(field)
	) | rpl::map([=](T &&value, bool active) {
		return active ? Mask(field, std::move(value)) : std::move(value);
	});
}

} // namespace Mokagram::Streamer
