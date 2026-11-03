#pragma once

#include <QtCore/QString>

namespace Main {
class Session;
} // namespace Main

namespace Media::View {

[[nodiscard]] bool OpenPdf(const QString &path, Main::Session *session);

} // namespace Media::View
