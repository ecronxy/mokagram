// Mokagram: защита от фейковых офферов (сообщения с кнопками-ссылками,
// которые выдают себя за настоящее предложение Telegram).
#pragma once

#include "ui/text/text_entity.h"

#include <QtGui/QPainter>
#include <QtCore/QVariant>

class HistoryItem;

namespace Mokagram::Scam {

void RegisterSettings();

// Похоже ли сообщение на фейковый оффер со ссылочными кнопками.
[[nodiscard]] bool IsFakeOffer(not_null<const HistoryItem*> item);

// Дописывает к тексту подозрительного сообщения предупреждение.
void MarkText(
	not_null<const HistoryItem*> item,
	TextWithEntities &text);

// Перехватывает открытие подозрительной ссылки (спрятанной за текстом,
// мини-апп, гарант-бот). true: показано предупреждение, ссылку не открывать.
[[nodiscard]] bool GuardLink(
	const QString &url,
	const QVariant &context,
	Fn<void()> again);

// Помечен ли текст (в начале стоит невидимый маркер и две пустые строки).
[[nodiscard]] bool HasMark(const QString &text);

// Рисует красную плашку клиента на месте пустых строк маркера.
void PaintBanner(QPainter &p, QRect rect);

// Показывает предупреждение; proceed вызывается, если человек всё равно
// хочет открыть ссылку.
void ShowWarning(not_null<const HistoryItem*> item, Fn<void()> proceed);

} // namespace Mokagram::Scam
