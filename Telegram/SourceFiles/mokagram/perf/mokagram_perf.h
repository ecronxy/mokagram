// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 4c8170a2
// Mokagram: экономия ресурсов. Фоновая экономия, режим тостера, возврат памяти системе.
#pragma once

#include <rpl/producer.h>

namespace Mokagram::Perf {

// true: клиент должен вести себя в режиме экономии (окно в фоне или включён режим тостера).
[[nodiscard]] rpl::producer<bool> EconomyValue();

// Нужно ли сейчас реагировать на подвисания: когда окно в фоне, монитор лагов спит.
[[nodiscard]] bool UserIsLooking();

void Start();

} // namespace Mokagram::Perf
