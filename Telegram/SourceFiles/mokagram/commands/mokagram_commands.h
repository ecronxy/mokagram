// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 0d571a70
// Mokagram: быстрые команды в поле ввода (.mute, .unmute, .ban, .unban).
#pragma once

#include "base/basic_types.h"
#include "data/data_msg_id.h"

#include <QtCore/QStringList>
#include <QtCore/QRect>
#include <QtGui/QColor>
#include <optional>
#include <vector>

class QPainter;
struct TextWithTags;
class History;

namespace Window {
class SessionController;
} // namespace Window

namespace Mokagram::Commands {

void RegisterSettings();

// true: сообщение было командой, отправлять его не нужно. Если команда
// хочет оставить в поле другой текст (.calc), он кладётся в replacement.
[[nodiscard]] bool Handle(
	not_null<Window::SessionController*> controller,
	not_null< ::History*> history,
	const TextWithTags &text,
	FullReplyTo replyTo,
	TextWithTags *replacement = nullptr);

// Для «.calc 12+3» в поле: выражение и результат, пока набираешь. Пусто, если не .calc или не считается.
[[nodiscard]] std::optional<std::pair<QString, QString>> CalcPreview(
	const QString &text);

// Для «.cr 2 ton usdt» в поле: готовая строка сводки. needsRates: курсы ещё грузятся.
struct CrResult {
	QString text;
	QString compact;
	bool needsRates = false;
};
[[nodiscard]] std::optional<CrResult> CrPreview(const QString &text);

// Алиасы: пары (имя, команда) без точки.
[[nodiscard]] std::vector<std::pair<QString, QString>> Aliases();
void SetAliases(const std::vector<std::pair<QString, QString>> &list);
[[nodiscard]] QStringList CommandNames();

// Экран «выберите чат» с кадром и репликой из локального набора данных.
// Рисует только если набор есть, иначе возвращает false.
void QuoteStageReset();
[[nodiscard]] bool PaintQuoteStage(QPainter &p, QRect area, QColor textColor);
// Сцена ещё появляется: окну нужно перерисоваться на следующем кадре.
[[nodiscard]] bool QuoteStageAnimating();
[[nodiscard]] bool QuoteStageClick(QPoint point);

} // namespace Mokagram::Commands
