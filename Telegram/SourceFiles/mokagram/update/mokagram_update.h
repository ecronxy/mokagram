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
