#include "mokagram/misc/mokagram_ping.h"

#include "core/application.h"
#include "mokagram/plugins/mokagram_plugin_install.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"
#include "window/window_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtNetwork/QTcpSocket>

#include <memory>
#include <vector>

namespace Mokagram::Misc {
namespace {

struct Datacenter {
	const char *name;
	const char *address;
};

const std::vector<Datacenter> &Datacenters() {
	static const auto result = std::vector<Datacenter>{
		{ "DC1 · Майами", "149.154.175.53" },
		{ "DC2 · Амстердам", "149.154.167.51" },
		{ "DC3 · Майами", "149.154.175.100" },
		{ "DC4 · Амстердам", "149.154.167.91" },
		{ "DC5 · Сингапур", "91.108.56.130" },
	};
	return result;
}

// Время установки TCP-соединения: ничего не меняет в настройках сети.
void Measure(
		const QString &address,
		Fn<void(int ms, QString error)> done) {
	const auto socket = new QTcpSocket();
	const auto timer = new QElapsedTimer();
	timer->start();
	const auto finished = std::make_shared<bool>(false);
	const auto finish = [=](int ms, QString error) {
		if (*finished) {
			return;
		}
		*finished = true;
		delete timer;
		socket->abort();
		socket->deleteLater();
		done(ms, std::move(error));
	};
	QObject::connect(socket, &QTcpSocket::connected, [=] {
		finish(int(timer->elapsed()), QString());
	});
	QObject::connect(socket, &QAbstractSocket::errorOccurred, [=] {
		finish(-1, socket->errorString());
	});
	QTimer::singleShot(4000, socket, [=] {
		finish(-1, QString::fromUtf8("нет ответа"));
	});
	socket->connectToHost(address, 443);
}

} // namespace

void ShowPingBox() {
	const auto window = Core::App().activeWindow();
	if (!window) {
		return;
	}
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(QString::fromUtf8("Пинг дата-центров")));
		box->setWidth(380);
		const auto content = box->verticalLayout();
		Ui::AddSkip(content);
		auto labels = std::make_shared<std::vector<QPointer<Ui::FlatLabel>>>();
		for (const auto &dc : Datacenters()) {
			labels->push_back(content->add(
				object_ptr<Ui::FlatLabel>(
					content,
					QString::fromUtf8(dc.name) + QString::fromUtf8(": …"),
					st::boxLabel),
				QMargins(24, 6, 24, 6)));
		}
		Ui::AddSkip(content);
		const auto run = [=] {
			for (auto i = 0; i != int(Datacenters().size()); ++i) {
				const auto dc = Datacenters()[i];
				const auto label = (*labels)[i];
				label->setText(QString::fromUtf8(dc.name)
					+ QString::fromUtf8(": …"));
				Measure(QString::fromUtf8(dc.address), crl::guard(label, [=](int ms, QString error) {
					label->setText(QString::fromUtf8(dc.name)
						+ QString::fromUtf8(": ")
						+ (ms >= 0
							? QString::number(ms) + QString::fromUtf8(" мс")
							: QString::fromUtf8("не отвечает")));
				}));
			}
		};
		box->addButton(rpl::single(QString::fromUtf8("Проверить снова")), run);
		box->addButton(
			rpl::single(QString::fromUtf8("Закрыть")),
			[=] { box->closeBox(); });
		run();
	}));
}

} // namespace Mokagram::Misc
