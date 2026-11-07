#pragma once

#include "base/basic_types.h"

#include <QtCore/QHash>
#include <QtCore/QString>

namespace Ui {
class InputField;
} // namespace Ui

namespace Mokagram::Inline {

void SetupInlineResults(not_null<Ui::InputField*> field);

// Курсы к доллару для .cr: пусто, пока не загрузились. RefreshCurrencyRates запускает загрузку.
[[nodiscard]] const QHash<QString, double> &CurrencyRatesNow();
void RefreshCurrencyRates();

} // namespace Mokagram::Inline
