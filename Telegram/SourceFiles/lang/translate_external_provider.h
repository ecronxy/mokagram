#pragma once

#include "translate_provider.h"

namespace Ui {

enum class ExternalTranslateService {
	Google,
	Microsoft,
	Yandex,
};

void ConfigureMicrosoftTranslation();

[[nodiscard]] std::unique_ptr<TranslateProvider> CreateExternalTranslateProvider(
	ExternalTranslateService service);

} // namespace Ui
