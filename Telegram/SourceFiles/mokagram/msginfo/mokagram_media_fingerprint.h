#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

class QIODevice;

namespace Mokagram::MessageInfo {

[[nodiscard]] QString VoicePlatform(const QByteArray &prefix);
[[nodiscard]] QString RoundVideoPlatform(QIODevice &file);

} // namespace Mokagram::MessageInfo
