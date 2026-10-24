/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_builder.h"

#include "apiwrap.h"
#include "lang/lang_keys.h"
#include "api/api_user_privacy.h"
#include "boxes/edit_privacy_box.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "settings/sections/settings_privacy_security.h"
#include "ui/vertical_list.h"
#include "mokagram/plugins/mokagram_plugin_install.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/painter.h"
#include "window/window_session_controller.h"
#include "styles/style_settings.h"
#include "styles/style_window.h"

namespace Settings::Builder {

namespace {

// Mokagram: блок строк настроек как отдельная скруглённая карточка.
constexpr auto kGroupRadius = 22;
const auto kGroupMargins = style::margins(10, 7, 10, 7);
constexpr auto kGroupInset = 6;

[[nodiscard]] Ui::VerticalLayout *GroupOuter(
		not_null<Ui::VerticalLayout*> container) {
	const auto value = container->property("moka-group-outer");
	return value.isValid()
		? reinterpret_cast<Ui::VerticalLayout*>(value.value<quintptr>())
		: nullptr;
}

[[nodiscard]] not_null<Ui::VerticalLayout*> BeginGroup(
		not_null<Ui::VerticalLayout*> outer) {
	const auto pad = outer->add(
		object_ptr<Ui::PaddingWrap<Ui::VerticalLayout>>(
			outer,
			object_ptr<Ui::VerticalLayout>(outer),
			kGroupMargins + style::margins(
				kGroupInset,
				kGroupInset,
				kGroupInset,
				kGroupInset)));
	const auto inner = pad->entity();
	inner->setProperty(
		"moka-group-outer",
		QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(outer.get())));

	pad->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		auto p = QPainter(pad);
		p.fillRect(clip, st::dialogsBg);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBg);
		const auto card = pad->rect() - kGroupMargins;
		const auto r = std::min(kGroupRadius, card.height() / 2);
		p.drawRoundedRect(card, r, r);
	}, pad->lifetime());

	struct Corner {
		not_null<Ui::RpWidget*> widget;
		bool right;
		bool bottom;
	};
	struct State {
		std::vector<Corner> corners;
		int radius = kGroupRadius;
	};
	const auto state = pad->lifetime().make_state<State>();
	for (const auto right : { false, true }) {
		for (const auto bottom : { false, true }) {
			const auto corner = Ui::CreateChild<Ui::RpWidget>(pad);
			corner->setAttribute(Qt::WA_TransparentForMouseEvents);
			corner->paintRequest(
			) | rpl::on_next([=] {
				auto p = QPainter(corner);
				auto hq = PainterHighQualityEnabler(p);
				const auto r = state->radius;
				auto path = QPainterPath();
				path.addRect(corner->rect());
				auto circle = QPainterPath();
				circle.addEllipse(QRectF(
					right ? -r : 0,
					bottom ? -r : 0,
					2 * r,
					2 * r));
				p.fillPath(path.subtracted(circle), st::dialogsBg);
			}, corner->lifetime());
			state->corners.push_back({ corner, right, bottom });
		}
	}

	// Тонкие линии между строками, от начала текста до края.
	const auto lines = Ui::CreateChild<Ui::RpWidget>(pad);
	lines->setAttribute(Qt::WA_TransparentForMouseEvents);
	struct HoverWatcher final : QObject {
		explicit HoverWatcher(QWidget *target) : QObject(target), target(target) {
		}
		bool eventFilter(QObject *, QEvent *e) override {
			if (e->type() == QEvent::Enter || e->type() == QEvent::Leave) {
				target->update();
			}
			return false;
		}
		QWidget *target;
	};
	const auto watcher = new HoverWatcher(lines);
	lines->paintRequest(
	) | rpl::on_next([=] {
		auto rows = std::vector<QWidget*>();
		for (const auto child : inner->children()) {
			if (const auto w = qobject_cast<QWidget*>(child)) {
				if (!w->isHidden() && w->height() >= 30) {
					rows.push_back(w);
				}
			}
		}
		std::sort(rows.begin(), rows.end(), [](QWidget *a, QWidget *b) {
			return a->y() < b->y();
		});
		auto p = QPainter(lines);
		p.setPen(QColor(255, 255, 255, 34));
		const auto left = kGroupInset + st::settingsButton.padding.left();
		const auto hovered = [&](QWidget *row) {
			return row->rect().contains(row->mapFromGlobal(QCursor::pos()))
				&& row->isVisible()
				&& row->underMouse();
		};
		for (const auto row : rows) {
			// Строка под курсором подсвечена пилюлей: линии рядом прячем.
			if (!row->property("moka-watched").toBool()) {
				row->setProperty("moka-watched", true);
				row->installEventFilter(watcher);
			}
		}
		for (auto i = 0; i + 1 < int(rows.size()); ++i) {
			if (hovered(rows[i]) || hovered(rows[i + 1])) {
				continue;
			}
			const auto a = rows[i];
			const auto y = kGroupInset + a->y() + a->height() - 1;
			p.drawLine(
				a->x() + left,
				y,
				kGroupInset + a->x() + a->width() - 12,
				y);
		}
	}, lines->lifetime());

	pad->sizeValue(
	) | rpl::on_next([=](QSize size) {
		const auto card = QRect(QPoint(), size) - kGroupMargins;
		const auto r = std::max(1, std::min(
			kGroupRadius,
			std::min(card.width(), card.height()) / 2));
		state->radius = r;
		for (const auto &corner : state->corners) {
			corner.widget->resize(r, r);
			corner.widget->move(
				card.x() + (corner.right ? card.width() - r : 0),
				card.y() + (corner.bottom ? card.height() - r : 0));
			corner.widget->raise();
		}
		lines->setGeometry(card);
		lines->raise();
		lines->update();
	}, pad->lifetime());
	inner->heightValue(
	) | rpl::on_next([=](int) {
		lines->update();
	}, lines->lifetime());
	return inner;
}

} // namespace
namespace {

[[nodiscard]] QString ResolveTitle(rpl::producer<QString> title) {
	auto result = QString();
	auto lifetime = rpl::lifetime();
	std::move(title).start(
		[&](QString value) { result = std::move(value); },
		[](auto&&) {},
		[] {},
		lifetime);
	return result;
}

} // namespace

BuildHelper::BuildHelper(
	SectionMeta &&meta,
	FnMut<void(SectionBuilder&)> method)
: build([=](
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		Fn<void(Type)> showOther,
		rpl::producer<> showFinished) {
	auto &lifetime = container->lifetime();
	const auto highlights = lifetime.make_state<HighlightRegistry>();
	const auto isPaused = Window::PausedIn(
		controller,
		Window::GifPauseReason::Layer);
	auto builder = SectionBuilder(WidgetContext{
		.container = static_cast<Ui::VerticalLayout*>(
			container->add(object_ptr<Ui::OverrideMargins>(
				container,
				object_ptr<Ui::VerticalLayout>(container)))->entity()),
		.controller = controller,
		.showOther = std::move(showOther),
		.isPaused = isPaused,
		.highlights = highlights,
	});
	_method(builder);

	std::move(showFinished) | rpl::on_next([=] {
		for (const auto &[id, entry] : *highlights) {
			if (entry.widget) {
				controller->checkHighlightControl(
					id,
					entry.widget,
					base::duplicate(entry.args));
			}
		}
	}, lifetime);
})
, _meta(std::move(meta))
, _method(std::move(method)) {
	Expects(_method != nullptr);

	SearchRegistry::Instance().add(
		&_meta,
		[=](not_null<::Main::Session*> session) { return index(session); });
}

SearchRegistry &SearchRegistry::Instance() {
	static SearchRegistry instance;
	return instance;
}

void SearchRegistry::add(
		not_null<const SectionMeta*> meta,
		SearchEntriesIndexer indexer) {
	_sections[meta->id] = meta;
	_indexers.push_back({ meta->id, std::move(indexer) });
}

std::vector<SearchEntry> SearchRegistry::collectAll(
		not_null<Main::Session*> session) const {
	auto result = std::vector<SearchEntry>();
	for (const auto &[sectionId, meta] : _sections) {
		if (meta->parentId) {
			result.push_back({
				.title = (*meta->title)(tr::now),
				.section = sectionId,
				.icon = { meta->icon },
			});
		}
	}
	for (const auto &entry : _indexers) {
		auto entries = entry.indexer(session);
		result.insert(result.end(), entries.begin(), entries.end());
	}
	return result;
}

QString SearchRegistry::sectionTitle(Type sectionId) const {
	const auto it = _sections.find(sectionId);
	return (it != _sections.end()) ? (*it->second->title)(tr::now) : QString();
}

QString SearchRegistry::sectionPath(Type sectionId, bool parentsOnly) const {
	auto parts = QStringList();
	auto current = sectionId;
	while (current) {
		if (const auto title = sectionTitle(current); !title.isEmpty()) {
			parts.prepend(title);
		}
		const auto it = _sections.find(current);
		current = (it != _sections.end()) ? it->second->parentId : nullptr;
	}
	if (parts.size() > 1 && parentsOnly) {
		parts.pop_back();
	}
	return parts.join(u" > "_q);
}

std::vector<SearchEntry> BuildHelper::index(
		not_null<Main::Session*> session) const {
	auto entries = std::vector<SearchEntry>();
	auto builder = SectionBuilder(SearchContext{
		.sectionId = _meta.id,
		.session = session,
		.entries = &entries,
	});
	_method(builder);
	for (auto &entry : entries) {
		if (!entry.section) {
			entry.section = _meta.id;
		}
	}
	return entries;
}

SectionBuilder::SectionBuilder(BuildContext context)
: _context(std::move(context)) {
}

void SectionBuilder::add(FnMut<void(const BuildContext &ctx)> method) {
	Expects(method != nullptr);

	method(_context);
}

Ui::VerticalLayout *SectionBuilder::scope(
		FnMut<void()> method,
		rpl::producer<bool> shown,
		FnMut<void(ToggledScopePtr)> hook) {
	auto result = (Ui::VerticalLayout*)nullptr;
	v::match(_context, [&](WidgetContext &wctx) {
		const auto outer = wctx.container;
		const auto toggled = (shown || hook);
		const auto wrap = toggled
			? outer->add(
				object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
					outer,
					object_ptr<Ui::VerticalLayout>(outer)))
			: nullptr;
		const auto inner = wrap
			? wrap->entity()
			: outer->add(object_ptr<Ui::VerticalLayout>(outer));
		wctx.container = inner;
		method();
		if (shown) {
			wrap->toggleOn(std::move(shown));
			wrap->finishAnimating();
		}
		if (hook) {
			hook(wrap);
		}
		wctx.container = outer;
		result = inner;
	}, [&](const SearchContext &sctx) {
		method();
	});
	return result;
}

Ui::RpWidget *SectionBuilder::add(
		FnMut<WidgetToAdd(const WidgetContext &ctx)> widget,
		FnMut<SearchEntry()> search) {
	auto result = (Ui::RpWidget*)nullptr;
	add([&](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			if (auto w = widget ? widget(wctx) : WidgetToAdd()) {
				result = w.widget.data();
				wctx.container->add(std::move(w.widget), w.margin, w.align);

				if (auto entry = search ? search() : SearchEntry()) {
					if (wctx.highlights) {
						wctx.highlights->push_back({
							std::move(entry.id),
							{ result, std::move(w.highlight) },
						});
					}
				}
			}
		}, [&](const SearchContext &sctx) {
			if (auto entry = search ? search() : SearchEntry()) {
				entry.section = sctx.sectionId;
				sctx.entries->push_back(std::move(entry));
			}
		});
	});
	return result;
}

Ui::RpWidget *SectionBuilder::addControl(ControlArgs &&args) {
	if (auto shown = base::take(args.shown)) {
		auto result = (Ui::RpWidget*)nullptr;
		scope([&] {
			result = addControl(std::move(args));
		}, std::move(shown));
		return result;
	}
	return add([&](const WidgetContext &ctx) {
		return WidgetToAdd{
			.widget = args.factory ? args.factory(ctx.container) : nullptr,
			.margin = args.margin,
			.align = args.align,
			.highlight = std::move(args.highlight),
		};
	}, [&]() mutable {
		return SearchEntry{
			.id = std::move(args.id),
			.title = ResolveTitle(std::move(args.title)),
			.keywords = std::move(args.keywords),
			.icon = std::move(args.searchIcon),
			.checkIcon = args.searchCheckIcon,
		};
	});
}

Ui::SettingsButton *SectionBuilder::addButton(ButtonArgs &&args) {
	const auto &st = args.st ? *args.st : st::settingsButton;
	auto iconForSearch = IconDescriptor{ args.icon.icon };
	const auto navigates = bool(args.onClick) && !args.label && !args.toggled && args.id != u"main/faq"_q;
	const auto factory = [&](not_null<Ui::VerticalLayout*> container) {
		auto button = CreateButtonWithIcon(
			args.container ? args.container : container.get(),
			rpl::duplicate(args.title),
			st,
			std::move(args.icon));
		if (button && args.onClick) {
			button->addClickHandler(std::move(args.onClick));
		}
		if (args.label) {
			CreateRightLabel(
				button.data(),
				std::move(args.label),
				st,
				rpl::duplicate(args.title));
		}
		if (args.toggled) {
			button->toggleOn(std::move(args.toggled));
		}
		Mokagram::Plugins::MakePill(
			button.data(),
			[] { return st::windowBg->c; });
		if (navigates) {
			// Mokagram: стрелка справа, как в настройках macOS.
			const auto chevron = Ui::CreateChild<Ui::RpWidget>(button.data());
			chevron->setAttribute(Qt::WA_TransparentForMouseEvents);
			chevron->resize(10, 14);
			chevron->paintRequest(
			) | rpl::on_next([=] {
				auto p = QPainter(chevron);
				auto hq = PainterHighQualityEnabler(p);
				auto pen = QPen(st::windowSubTextFg->c, 1.6);
				pen.setCapStyle(Qt::RoundCap);
				pen.setJoinStyle(Qt::RoundJoin);
				p.setPen(pen);
				p.drawPolyline(QPolygonF({
					QPointF(2, 2),
					QPointF(8, 7),
					QPointF(2, 12),
				}));
			}, chevron->lifetime());
			button->sizeValue(
			) | rpl::on_next([=](QSize size) {
				chevron->move(
					size.width() - 18 - chevron->width(),
					(size.height() - chevron->height()) / 2);
			}, chevron->lifetime());

		}
		return button;
	};
	return static_cast<Ui::SettingsButton*>(addControl({
		.factory = factory,
		.id = std::move(args.id),
		.title = rpl::duplicate(args.title),
		.highlight = std::move(args.highlight),
		.shown = std::move(args.shown),

		.keywords = std::move(args.keywords),
		.searchIcon = std::move(iconForSearch),
	}));
}

Ui::SettingsButton *SectionBuilder::addSectionButton(SectionArgs &&args) {
	const auto wctx = std::get_if<WidgetContext>(&_context);
	const auto showOther = wctx ? wctx->showOther : nullptr;
	const auto target = args.targetSection;
	return addButton({
		.title = std::move(args.title),
		.icon = std::move(args.icon),
		.onClick = [=] { showOther(target); },
		.keywords = std::move(args.keywords),
	});
}

void SectionBuilder::addDivider() {
	v::match(_context, [&](WidgetContext &ctx) {
		// Mokagram: вместо полосы-разделителя начинается новая карточка.
		auto outer = ctx.container;
		if (const auto parent = GroupOuter(outer)) {
			outer = parent;
		}
		ctx.container = BeginGroup(outer);
	}, [](const SearchContext &) {
	});
}

void SectionBuilder::addSkip() {
	v::match(_context, [&](const WidgetContext &ctx) {
		Ui::AddSkip(ctx.container);
	}, [](const SearchContext &) {
	});
}

void SectionBuilder::addSkip(int height) {
	v::match(_context, [&](const WidgetContext &ctx) {
		Ui::AddSkip(ctx.container, height);
	}, [](const SearchContext &) {
	});
}

void SectionBuilder::addDividerText(rpl::producer<QString> text) {
	v::match(_context, [&](WidgetContext &ctx) {
		auto outer = ctx.container;
		if (const auto parent = GroupOuter(outer)) {
			outer = parent;
			ctx.container = outer;
		}
		Ui::AddDividerText(outer, std::move(text));
	}, [](const SearchContext &) {
	});
}

Ui::SettingsButton *SectionBuilder::addPremiumButton(PremiumButtonArgs &&args) {
	const auto result = addButton({
		.id = std::move(args.id),
		.title = std::move(args.title),
		.label = std::move(args.label),
		.onClick = std::move(args.onClick),
		.keywords = std::move(args.keywords),
	});
	if (result) {
		AddPremiumStar(
			result,
			args.credits,
			v::get<WidgetContext>(_context).isPaused);
	}
	return result;
}

Ui::SettingsButton *SectionBuilder::addPrivacyButton(PrivacyButtonArgs &&args) {
	const auto controller = this->controller();
	const auto session = this->session();

	const auto button = addButton({
		.id = args.id,
		.title = rpl::duplicate(args.title),
		.st = &st::settingsButtonNoIcon,
		.label = PrivacyButtonLabel(session, args.key),
		.keywords = args.keywords,
	});
	if (button) {
		const auto id = args.id;
		const auto premium = args.premium;
		const auto shower = Ui::CreateChild<rpl::lifetime>(button);
		const auto factory = args.controllerFactory;
		button->addClickHandler([=, key = args.key] {
			*shower = session->api().userPrivacy().value(
				key
			) | rpl::take(
				1
			) | rpl::on_next(crl::guard(controller, [=](
					const Api::UserPrivacy::Rule &value) {
				controller->show(Box<EditPrivacyBox>(
					controller,
					factory(),
					value));
			}));
		});
		if (premium) {
			AddPrivacyPremiumStar(
				button,
				session,
				std::move(args.title),
				st::settingsButtonNoIcon.padding);
		}
	}
	return button;
}

Ui::Checkbox *SectionBuilder::addCheckbox(CheckboxArgs &&args) {
	const auto factory = [&](not_null<Ui::VerticalLayout*> container) {
		return object_ptr<Ui::Checkbox>(
			container,
			ResolveTitle(rpl::duplicate(args.title)),
			args.checked,
			st::settingsCheckbox);
	};
	return static_cast<Ui::Checkbox*>(addControl({
		.factory = factory,
		.id = std::move(args.id),
		.title = rpl::duplicate(args.title),
		.margin = st::settingsCheckboxPadding,
		.highlight = std::move(args.highlight),
		.shown = std::move(args.shown),
		.keywords = std::move(args.keywords),
		.searchCheckIcon = (args.checked
			? SearchEntryCheckIcon::Checked
			: SearchEntryCheckIcon::Unchecked),
	}));
}

void SectionBuilder::addSubsectionTitle(SubsectionTitleArgs &&args) {
	v::match(_context, [&](const WidgetContext &ctx) {
		const auto title = AddSubsectionTitle(
			ctx.container,
			rpl::duplicate(args.title));
		if (!args.id.isEmpty() && ctx.highlights) {
			ctx.highlights->push_back({
				args.id,
				{ title.get(), SubsectionTitleHighlight() },
			});
		}
	}, [&](const SearchContext &ctx) {
		if (!args.id.isEmpty()) {
			ctx.entries->push_back({
				.id = std::move(args.id),
				.title = ResolveTitle(std::move(args.title)),
				.keywords = std::move(args.keywords),
				.section = ctx.sectionId,
			});
		}
	});
}

void SectionBuilder::addSubsectionTitle(rpl::producer<QString> text) {
	v::match(_context, [&](const WidgetContext &ctx) {
		AddSubsectionTitle(ctx.container, std::move(text));
	}, [](const SearchContext &) {
	});
}

Ui::VerticalLayout *SectionBuilder::container() const {
	return v::match(_context, [](const WidgetContext &ctx) {
		return ctx.container.get();
	}, [](const SearchContext &) -> Ui::VerticalLayout* {
		return nullptr;
	});
}

Window::SessionController *SectionBuilder::controller() const {
	return v::match(_context, [](const WidgetContext &ctx) {
		return ctx.controller.get();
	}, [](const SearchContext &) -> Window::SessionController* {
		return nullptr;
	});
}

not_null<Main::Session*> SectionBuilder::session() const {
	return v::match(_context, [](const WidgetContext &ctx) {
		return &ctx.controller->session();
	}, [](const SearchContext &ctx) {
		return ctx.session.get();
	});
}

Fn<void(Type)> SectionBuilder::showOther() const {
	return v::match(_context, [](const WidgetContext &ctx) {
		return ctx.showOther;
	}, [](const SearchContext &) -> Fn<void(Type)> {
		return nullptr;
	});
}

HighlightRegistry *SectionBuilder::highlights() const {
	return v::match(_context, [](const WidgetContext &ctx) {
		return ctx.highlights;
	}, [](const SearchContext &) -> HighlightRegistry* {
		return nullptr;
	});
}

void SectionBuilder::registerHighlight(
		QString id,
		QWidget *widget,
		HighlightArgs &&args) {
	v::match(_context, [&](const WidgetContext &ctx) {
		if (ctx.highlights && widget) {
			ctx.highlights->push_back({
				std::move(id),
				{ widget, std::move(args) },
			});
		}
	}, [](const SearchContext &) {
	});
}

} // namespace Settings::Builder
