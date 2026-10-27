// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: 981700e5
#pragma once

#include <QtCore/QByteArray>

namespace Mokagram::Update {

// WHY: AppVersion stays the upstream Telegram Desktop version, so releases
// of mokaGram are ordered by their own number, read by publish_update.sh.
constexpr auto kVersion = 2;
constexpr auto kVersionTitle = "1.1";

// Ed25519-подпись ключом mokaGram. Используется и магазином плагинов.
[[nodiscard]] bool VerifyDetached(
	const QByteArray &message,
	const QByteArray &signature);

void RegisterSettings();
void Start();
void CheckNow();

} // namespace Mokagram::Update
