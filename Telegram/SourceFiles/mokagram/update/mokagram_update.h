#pragma once

namespace Mokagram::Update {

// WHY: AppVersion stays the upstream Telegram Desktop version, so releases
// of mokaGram are ordered by their own number, read by publish_update.sh.
constexpr auto kVersion = 1;
constexpr auto kVersionTitle = "1.0";

void RegisterSettings();
void Start();
void CheckNow();

} // namespace Mokagram::Update
