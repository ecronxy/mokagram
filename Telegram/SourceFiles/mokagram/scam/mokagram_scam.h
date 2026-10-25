// Mokagram: защита от фейковых офферов (сообщения с кнопками-ссылками,
// которые выдают себя за настоящее предложение Telegram).
#pragma once

#include "ui/text/text_entity.h"

class HistoryItem;

namespace Mokagram::Scam {

void RegisterSettings();

// Похоже ли сообщение на фейковый оффер со ссылочными кнопками.
[[nodiscard]] bool IsFakeOffer(not_null<const HistoryItem*> item);

// Дописывает к тексту подозрительного сообщения предупреждение.
void MarkText(
	not_null<const HistoryItem*> item,
	TextWithEntities &text);

// Показывает предупреждение; proceed вызывается, если человек всё равно
// хочет открыть ссылку.
void ShowWarning(not_null<const HistoryItem*> item, Fn<void()> proceed);

} // namespace Mokagram::Scam
