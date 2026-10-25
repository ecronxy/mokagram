// Mokagram: дата регистрации аккаунта через API DateReg (https://docs.datereg.pro/api).
#pragma once

#include <rpl/producer.h>

#include <QtCore/QString>

namespace Mokagram::Misc {


[[nodiscard]] bool DateRegEnabled();

// Подпись вида "январь 2024" (после загрузки). Пока ждём, отдаёт "…".
[[nodiscard]] rpl::producer<QString> RegistrationDate(quint64 userId);

} // namespace Mokagram::Misc
