#include "media/view/media_view_pdf.h"

#ifdef MOKAGRAM_HAS_QT_PDF
#include "main/main_session.h"
#include "mokagram/lang/mokagram_lang.h"
#include "mokagram/settings/mokagram_settings.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtGui/QAction>
#include <QtGui/QShortcut>
#include <QtPdf/QPdfDocument>
#include <QtPdf/QPdfLink>
#include <QtPdf/QPdfPageNavigator>
#include <QtPdf/QPdfSearchModel>
#include <QtPdfWidgets/QPdfView>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QToolBar>
#endif // MOKAGRAM_HAS_QT_PDF

namespace Media::View {

bool OpenPdf(const QString &path, Main::Session *session) {
#ifdef MOKAGRAM_HAS_QT_PDF
	if (!Mokagram::Settings::Bool(u"documents.pdf"_q)
		|| QFileInfo(path).suffix().compare(u"pdf"_q, Qt::CaseInsensitive)) {
		return false;
	}
	const auto text = [](const char *value) {
		return QString::fromUtf8(Mokagram::Lang::Tr(value));
	};
	const auto window = new QMainWindow();
	window->setAttribute(Qt::WA_DeleteOnClose);
	window->setWindowTitle(QFileInfo(path).fileName() + u" — mokaGram"_q);
	window->resize(st::boxWideWidth * 2, st::boxWideWidth * 2);
	const auto view = new QPdfView(window);
	const auto document = new QPdfDocument(view);
	view->setDocument(document);
	view->setPageMode(QPdfView::PageMode::MultiPage);
	view->setZoomMode(QPdfView::ZoomMode::FitToWidth);
	window->setCentralWidget(view);
	const auto toolbar = window->addToolBar(text("PDF"));
	toolbar->setMovable(false);
	const auto page = new QSpinBox(toolbar);
	page->setMinimum(1);
	page->setAccessibleName(text("Страница"));
	toolbar->addWidget(page);
	const auto count = new QLabel(toolbar);
	toolbar->addWidget(count);
	const auto navigator = view->pageNavigator();
	QObject::connect(document, &QPdfDocument::pageCountChanged, window,
		[=](int pages) {
			page->setMaximum(qMax(pages, 1));
			count->setText(u" / %1"_q.arg(pages));
		});
	QObject::connect(page, &QSpinBox::valueChanged, window, [=](int value) {
		if (value - 1 != navigator->currentPage()) {
			navigator->jump(value - 1, {});
		}
	});
	QObject::connect(navigator, &QPdfPageNavigator::currentPageChanged,
		page, [=](int value) { page->setValue(value + 1); });
	const auto zoom = [=](double factor) {
		view->setZoomMode(QPdfView::ZoomMode::Custom);
		view->setZoomFactor(qBound(0.1, view->zoomFactor() * factor, 8.));
	};
	const auto zoomOut = toolbar->addAction(text("Уменьшить"));
	zoomOut->setShortcuts(QKeySequence::ZoomOut);
	QObject::connect(zoomOut, &QAction::triggered, window,
		[=] { zoom(1. / 1.2); });
	const auto zoomIn = toolbar->addAction(text("Увеличить"));
	zoomIn->setShortcuts(QKeySequence::ZoomIn);
	QObject::connect(zoomIn, &QAction::triggered, window,
		[=] { zoom(1.2); });
	const auto fit = toolbar->addAction(text("По ширине"));
	QObject::connect(fit, &QAction::triggered, window, [=] {
		view->setZoomMode(QPdfView::ZoomMode::FitToWidth);
	});
	toolbar->addSeparator();
	const auto search = new QLineEdit(toolbar);
	search->setPlaceholderText(text("Поиск в PDF"));
	toolbar->addWidget(search);
	const auto matches = new QPdfSearchModel(view);
	matches->setDocument(document);
	view->setSearchModel(matches);
	QObject::connect(search, &QLineEdit::textChanged,
		matches, &QPdfSearchModel::setSearchString);
	const auto nextMatch = [=] {
		const auto total = matches->rowCount({});
		if (total > 0) {
			const auto index = (view->currentSearchResultIndex() + 1) % total;
			view->setCurrentSearchResultIndex(index);
			navigator->jump(matches->resultAtIndex(index));
		}
	};
	QObject::connect(search, &QLineEdit::returnPressed, window, nextMatch);
	const auto next = toolbar->addAction(text("Далее"));
	next->setShortcut(QKeySequence::FindNext);
	QObject::connect(next, &QAction::triggered, window, nextMatch);
	const auto find = new QShortcut(QKeySequence::Find, window);
	QObject::connect(find, &QShortcut::activated, window, [=] {
		search->setFocus();
		search->selectAll();
	});
	const auto close = new QShortcut(QKeySequence::Close, window);
	QObject::connect(close, &QShortcut::activated, window, &QWidget::close);
	QObject::connect(document, &QPdfDocument::passwordRequired, window, [=] {
		const auto dialog = new QInputDialog(window);
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->setWindowTitle(text("PDF"));
		dialog->setLabelText(text("Пароль документа"));
		dialog->setTextEchoMode(QLineEdit::Password);
		QObject::connect(dialog, &QInputDialog::textValueSelected,
			document, &QPdfDocument::setPassword);
		QObject::connect(dialog, &QInputDialog::rejected,
			window, &QWidget::close);
		dialog->open();
	});
	QObject::connect(document, &QPdfDocument::statusChanged, window,
		[=](QPdfDocument::Status status) {
			if (status == QPdfDocument::Status::Error
				&& document->error() != QPdfDocument::Error::IncorrectPassword) {
				window->statusBar()->showMessage(text("Не удалось открыть PDF"));
			}
		});
	if (session) {
		session->lifetime().add([weak = QPointer(window)] {
			if (weak) {
				weak->close();
			}
		});
	}
	const auto error = document->load(path);
	if (error != QPdfDocument::Error::None
		&& error != QPdfDocument::Error::IncorrectPassword) {
		delete window;
		return false;
	}
	window->show();
	window->raise();
	window->activateWindow();
	return true;
#else // MOKAGRAM_HAS_QT_PDF
	return false;
#endif // MOKAGRAM_HAS_QT_PDF
}

} // namespace Media::View
