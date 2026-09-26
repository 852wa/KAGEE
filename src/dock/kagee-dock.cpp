#include "kagee-dock.hpp"
#include "prop-view.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QButtonGroup>
#include <util/config-file.h>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QAbstractItemView>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QPainter>
#include <QStyle>
#include <cmath>
#include <functional>
#include <string>

#define T_(s) QString::fromUtf8(obs_module_text(s))

static const char *STAGE_ID = "kagee_stage";
static const char *ADJUST_ID = "kagee_adjustment_layer";
static const char *LIGHT_ID = "kagee_light_filter";
static const char *GRADE_ID = "kagee_grade_filter";
static const char *FX_IDS[] = {"kagee_lens_filter", "kagee_glow_filter",   "kagee_blur_filter",
			       "kagee_retro_filter", "kagee_glitch_filter", "kagee_trail_filter"};
static constexpr int MAX_SHOTS = 8;
static constexpr int MAX_LAYERS = 8;

/* Small vector thumbnails keep the first-run guide crisp at any dock scale. */
class SetupStepIcon final : public QWidget {
	int step;

public:
	explicit SetupStepIcon(int number, QWidget *parent = nullptr) : QWidget(parent), step(number)
	{
		setFixedSize(62, 42);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(QPen(QColor(105, 135, 255), 2));
		p.setBrush(QColor(36, 41, 55));
		p.drawRoundedRect(QRectF(1, 1, width() - 2, height() - 2), 6, 6);
		const QRectF screen(8, 7, 46, 28);
		p.setPen(QPen(QColor(115, 126, 153), 1));
		p.setBrush(QColor(22, 26, 35));
		p.drawRoundedRect(screen, 3, 3);
		if (step == 1) {
			p.setPen(QPen(QColor(100, 170, 255), 2));
			p.drawLine(13, 28, 27, 13);
			p.drawLine(27, 13, 49, 26);
			p.setBrush(QColor(255, 184, 86));
			p.setPen(Qt::NoPen);
			p.drawEllipse(QPointF(31, 19), 3, 3);
		} else if (step == 2) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(66, 103, 214));
			p.drawRoundedRect(QRectF(13, 12, 32, 6), 2, 2);
			p.setBrush(QColor(109, 166, 235));
			p.drawRoundedRect(QRectF(18, 21, 25, 6), 2, 2);
			p.setBrush(QColor(255, 184, 86));
			p.drawEllipse(QPointF(31, 30), 3, 3);
		} else {
			for (int i = 0; i < 3; ++i) {
				p.setPen(Qt::NoPen);
				p.setBrush(i == 1 ? QColor(255, 184, 86) : QColor(66, 103, 214));
				p.drawRoundedRect(QRectF(13 + i * 13, 13, 10, 15), 2, 2);
			}
		}
	}
};

static bool is_kagee_target(obs_source_t *src)
{
	const char *id = obs_source_get_unversioned_id(src);
	return id && (strcmp(id, STAGE_ID) == 0 || strcmp(id, ADJUST_ID) == 0);
}

template<typename F> static void invoke_queued(QObject *obj, F fn)
{
	QPointer<QObject> guard(obj);
	QMetaObject::invokeMethod(
		obj,
		[guard, fn]() {
			if (guard)
				fn();
		},
		Qt::QueuedConnection);
}

/* ------------------------------------------------------------------------- */

KageeDock::KageeDock(QWidget *parent) : QWidget(parent)
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(4, 4, 4, 4);

	auto *top = new QHBoxLayout();
	targetCombo = new QComboBox(this);
	targetCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	followBox = new QCheckBox(T_("Dock.Follow"), this);
	targetCombo->setMinimumContentsLength(8);
	followBox->setChecked(true);
	followBox->setToolTip(T_("Dock.FollowTip"));
	top->addWidget(new QLabel(T_("Dock.Target"), this));
	top->addWidget(targetCombo, 1);
	top->addWidget(followBox);
	root->addLayout(top);

	/* easy / detailed mode */
	auto *modeRow = new QHBoxLayout();
	modeRow->setSpacing(2);
	auto *modeGroup = new QButtonGroup(this);
	const char *mode_keys[2] = {"Dock.ModeEasy", "Dock.ModeDetail"};
	for (int i = 0; i < 2; i++) {
		modeButtons[i] = new QPushButton(T_(mode_keys[i]), this);
		modeButtons[i]->setCheckable(true);
		modeGroup->addButton(modeButtons[i], i);
		modeRow->addWidget(modeButtons[i]);
	}
	connect(modeGroup, &QButtonGroup::idClicked, this, &KageeDock::setMode);
	root->addLayout(modeRow);

	modeStack = new QStackedWidget(this);
	easyPage = new QScrollArea(modeStack);
	easyPage->setWidgetResizable(true);
	easyPage->setFrameShape(QFrame::NoFrame);
	easyPage->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	modeStack->addWidget(easyPage);
	auto *detail = new QWidget(modeStack);
	auto *detailLayout = new QVBoxLayout(detail);
	detailLayout->setContentsMargins(0, 0, 0, 0);
	modeStack->addWidget(detail);

	/* two rows of tab buttons: fits narrow docks better than a scrolling tab bar */
	auto *nav = new QGridLayout();
	nav->setSpacing(2);
	auto *navGroup = new QButtonGroup(this);
	navGroup->setExclusive(true);
	stack = new QStackedWidget(this);
	const char *tab_keys[T_COUNT] = {"Dock.Tab.Shots", "Dock.Tab.Camera", "Dock.Tab.Layers", "Dock.Tab.Motion",
					 "Dock.Tab.Stage", "Dock.Tab.Light",  "Dock.Tab.Grade",  "Dock.Tab.Fx"};
	for (int i = 0; i < T_COUNT; i++) {
		auto *b = new QPushButton(T_(tab_keys[i]), this);
		b->setCheckable(true);
		b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		b->setMinimumWidth(0);
		navGroup->addButton(b, i);
		nav->addWidget(b, i / 4, i % 4);
		navButtons[i] = b;

		pages[i] = new QScrollArea(stack);
		pages[i]->setWidgetResizable(true);
		pages[i]->setFrameShape(QFrame::NoFrame);
		pages[i]->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		stack->addWidget(pages[i]);
	}
	connect(navGroup, &QButtonGroup::idClicked, this, &KageeDock::showTab);
	navButtons[0]->setChecked(true);
	detailLayout->addLayout(nav);
	detailLayout->addWidget(stack, 1);
	root->addWidget(modeStack, 1);

	config_t *cfg = obs_frontend_get_user_config();
	config_set_default_int(cfg, "Kagee", "DockMode", 0);
	setMode((int)config_get_int(cfg, "Kagee", "DockMode"));

	pollTimer = new QTimer(this);
	pollTimer->setInterval(150);
	connect(pollTimer, &QTimer::timeout, this, &KageeDock::pollState);
	pollTimer->start();

	rebuildTimer = new QTimer(this);
	rebuildTimer->setSingleShot(true);
	rebuildTimer->setInterval(80);
	connect(rebuildTimer, &QTimer::timeout, this, &KageeDock::rebuildPages);

	targetsTimer = new QTimer(this);
	targetsTimer->setSingleShot(true);
	targetsTimer->setInterval(300);
	connect(targetsTimer, &QTimer::timeout, this, &KageeDock::refreshTargets);

	connect(targetCombo, &QComboBox::currentIndexChanged, this, &KageeDock::onTargetSelected);
	connect(followBox, &QCheckBox::toggled, this, [this](bool on) {
		if (on)
			followScene();
	});

	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_connect(sh, "source_create", onGlobalSourceChange, this);
	signal_handler_connect(sh, "source_destroy", onGlobalSourceChange, this);
	signal_handler_connect(sh, "source_rename", onGlobalSourceChange, this);

	rebuildPages();
}

KageeDock::~KageeDock()
{
	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_disconnect(sh, "source_create", onGlobalSourceChange, this);
	signal_handler_disconnect(sh, "source_destroy", onGlobalSourceChange, this);
	signal_handler_disconnect(sh, "source_rename", onGlobalSourceChange, this);
	setTarget(nullptr);
}

void KageeDock::frontendEvent(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING: {
		refreshTargets();
		followScene();
		QString dir = qEnvironmentVariable("KAGEE_DOCK_SNAPSHOT");
		if (!dir.isEmpty())
			QTimer::singleShot(3500, this, [this, dir]() { snapshot(dir); });
		if (!qEnvironmentVariable("KAGEE_DOCK_SELFTEST").isEmpty())
			QTimer::singleShot(6000, this, &KageeDock::selfTest);
		break;
	}
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		refreshTargets();
		followScene();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		followScene();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_EXIT:
		/* sources are about to go away: drop every reference now */
		setTarget(nullptr);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* target handling                                                           */

obs_source_t *KageeDock::getTarget() const
{
	return target ? obs_weak_source_get_source(target) : nullptr;
}

void KageeDock::connectTarget(obs_source_t *src, bool on)
{
	signal_handler_t *sh = obs_source_get_signal_handler(src);
	const char *sigs[] = {"filter_add", "filter_remove", "reorder_filters"};
	for (const char *sig : sigs) {
		if (on)
			signal_handler_connect(sh, sig, onTargetSignal, this);
		else
			signal_handler_disconnect(sh, sig, onTargetSignal, this);
	}
	if (on)
		signal_handler_connect(sh, "remove", onTargetRemoved, this);
	else
		signal_handler_disconnect(sh, "remove", onTargetRemoved, this);
}

void KageeDock::setTarget(obs_source_t *src)
{
	obs_source_t *old = getTarget();
	if (old == src && src) {
		obs_source_release(old);
		return;
	}
	if (old) {
		connectTarget(old, false);
		obs_source_release(old);
	}
	obs_weak_source_release(target);
	target = nullptr;
	targetIsStage = false;

	if (src) {
		target = obs_source_get_weak_source(src);
		targetIsStage = strcmp(obs_source_get_unversioned_id(src), STAGE_ID) == 0;
		connectTarget(src, true);
	}
	rebuildPages();
}

void KageeDock::onTargetSignal(void *data, calldata_t *)
{
	auto *self = static_cast<KageeDock *>(data);
	invoke_queued(self, [self]() { self->rebuildTimer->start(); });
}

void KageeDock::onTargetRemoved(void *data, calldata_t *)
{
	auto *self = static_cast<KageeDock *>(data);
	invoke_queued(self, [self]() {
		self->setTarget(nullptr);
		self->targetsTimer->start();
	});
}

void KageeDock::onGlobalSourceChange(void *data, calldata_t *cd)
{
	obs_source_t *src = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (src && !is_kagee_target(src))
		return;
	auto *self = static_cast<KageeDock *>(data);
	invoke_queued(self, [self]() { self->targetsTimer->start(); });
}

void KageeDock::refreshTargets()
{
	obs_source_t *cur = getTarget();
	QString curName = cur ? QString::fromUtf8(obs_source_get_name(cur)) : QString();
	obs_source_release(cur);

	QSignalBlocker block(targetCombo);
	targetCombo->clear();
	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			if (is_kagee_target(src) && !obs_source_removed(src)) {
				auto *combo = static_cast<QComboBox *>(param);
				QString name = QString::fromUtf8(obs_source_get_name(src));
				QString kind = QString::fromUtf8(obs_source_get_display_name(obs_source_get_id(src)));
				combo->addItem(QString("%1  (%2)").arg(name, kind), name);
			}
			return true;
		},
		targetCombo);

	int idx = targetCombo->findData(curName);
	targetCombo->setCurrentIndex(idx);
	if (idx < 0 && targetCombo->count() > 0 && !curName.isEmpty())
		curName.clear();
	if (curName.isEmpty()) {
		if (targetCombo->count() > 0) {
			targetCombo->setCurrentIndex(0);
			onTargetSelected(0);
		} else {
			setTarget(nullptr);
		}
	}
}

void KageeDock::onTargetSelected(int index)
{
	if (index < 0)
		return;
	QString name = targetCombo->itemData(index).toString();
	obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
	setTarget(src);
	obs_source_release(src);
}

struct find_ctx {
	obs_source_t *stage = nullptr;
	obs_source_t *adjust = nullptr;
};

static bool find_in_scene(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<find_ctx *>(param);
	obs_source_t *src = obs_sceneitem_get_source(item);
	const char *id = obs_source_get_unversioned_id(src);
	if (!ctx->stage && strcmp(id, STAGE_ID) == 0)
		ctx->stage = src;
	else if (!ctx->adjust && strcmp(id, ADJUST_ID) == 0)
		ctx->adjust = src;
	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, find_in_scene, param);
	return true;
}

void KageeDock::followScene()
{
	if (!followBox->isChecked())
		return;
	/* in studio mode edit what is being prepared (preview), otherwise the live scene */
	obs_source_t *scene_src = obs_frontend_get_current_preview_scene();
	if (!scene_src)
		scene_src = obs_frontend_get_current_scene();
	if (!scene_src)
		return;
	find_ctx ctx;
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (scene)
		obs_scene_enum_items(scene, find_in_scene, &ctx);
	obs_source_t *pick = ctx.stage ? ctx.stage : ctx.adjust;
	if (pick) {
		int idx = targetCombo->findData(QString::fromUtf8(obs_source_get_name(pick)));
		if (idx < 0) {
			refreshTargets();
			idx = targetCombo->findData(QString::fromUtf8(obs_source_get_name(pick)));
		}
		if (idx >= 0 && idx != targetCombo->currentIndex())
			targetCombo->setCurrentIndex(idx);
	}
	obs_source_release(scene_src);
}

/* ------------------------------------------------------------------------- */
/* pages                                                                     */

void KageeDock::setPage(Tab tab, QWidget *content)
{
	pages[tab]->setWidget(content); /* deletes the previous page */
}

QWidget *KageeDock::messagePage(const QString &text)
{
	auto *w = new QWidget();
	auto *v = new QVBoxLayout(w);
	auto *l = new QLabel(text, w);
	l->setWordWrap(true);
	l->setAlignment(Qt::AlignCenter);
	v->addStretch();
	v->addWidget(l);
	v->addStretch();
	return w;
}

static QWidget *with_view(QWidget *head, PropView *view)
{
	auto *w = new QWidget();
	auto *v = new QVBoxLayout(w);
	if (head)
		v->addWidget(head);
	view->setParent(w);
	v->addWidget(view);
	v->addStretch();
	return w;
}

void KageeDock::rebuildPages()
{
	shotButtons.clear();
	saveButtons.clear();
	easyShotButtons.clear();
	easyPresetCombos.clear();
	autoButton = tapButton = nullptr;

	obs_source_t *src = getTarget();
	if (!src) {
		for (int i = 0; i < T_COUNT; i++)
			setPage((Tab)i, messagePage(T_("Dock.NoTarget")));
		easyPage->setWidget(messagePage(T_("Dock.NoTarget")));
		return;
	}
	easyPage->setWidget(buildEasyPage(src));

	if (targetIsStage) {
		setPage(T_SHOTS, buildShotsPage(src));
		setPage(T_CAMERA, with_view(nullptr, new PropView(src, {"grp_cam"})));
		setPage(T_LAYERS, buildLayersPage(src));
		setPage(T_MOTION, with_view(nullptr, new PropView(src, {"grp_motion", "grp_auto", "grp_fx"})));
		setPage(T_STAGE, with_view(nullptr, new PropView(src, {"grp_stage", "dof_on"})));
	} else {
		for (int i = T_SHOTS; i <= T_STAGE; i++)
			setPage((Tab)i, messagePage(T_("Dock.StageOnly")));
	}
	setPage(T_LIGHT, buildFilterPage(src, LIGHT_ID));
	setPage(T_GRADE, buildFilterPage(src, GRADE_ID));
	setPage(T_FX, buildEffectsPage(src));

	for (int i = T_SHOTS; i <= T_STAGE; i++)
		navButtons[i]->setEnabled(targetIsStage);
	if (!targetIsStage && stack->currentIndex() <= T_STAGE)
		showTab(T_LIGHT);

	obs_source_release(src);
}

void KageeDock::callAction(const char *proc, const char *name, long long index)
{
	obs_source_t *src = getTarget();
	if (!src)
		return;
	calldata_t cd;
	calldata_init(&cd);
	if (name)
		calldata_set_string(&cd, "name", name);
	calldata_set_int(&cd, "index", index);
	proc_handler_call(obs_source_get_proc_handler(src), proc, &cd);
	calldata_free(&cd);
	obs_source_release(src);
}

void KageeDock::importScene()
{
	obs_source_t *src = getTarget();
	if (!src)
		return;
	calldata_t cd;
	calldata_init(&cd);
	proc_handler_call(obs_source_get_proc_handler(src), "kagee_import_scene", &cd);
	long long n = calldata_int(&cd, "count");
	calldata_free(&cd);
	obs_source_release(src);
	if (n <= 0)
		blog(LOG_INFO, "[Kagee] nothing to import (no visible video items below the stage)");
	rebuildTimer->start();
}

/* Shown while the stage has no layers: the single most important first step. */
QWidget *KageeDock::setupBanner(obs_source_t *src, bool always)
{
	calldata_t cd;
	calldata_init(&cd);
	proc_handler_call(obs_source_get_proc_handler(src), "kagee_layers", &cd);
	long long configured = calldata_int(&cd, "configured");
	calldata_free(&cd);
	if (configured > 0 && !always)
		return nullptr;

	auto *box = new QGroupBox(configured > 0 ? T_("Dock.ImportTitle") : T_("Dock.SetupTitle"));
	box->setObjectName("setup_banner");
	auto *v = new QVBoxLayout(box);
	if (configured == 0) {
		auto *steps = new QHBoxLayout();
		steps->setSpacing(4);
		const char *keys[] = {"Dock.StepAdd", "Dock.StepImport", "Dock.StepShot"};
		for (int i = 0; i < 3; ++i) {
			auto *item = new QWidget(box);
			auto *layout = new QVBoxLayout(item);
			layout->setContentsMargins(2, 2, 2, 2);
			layout->setSpacing(3);
			auto *icon = new SetupStepIcon(i + 1, item);
			layout->addWidget(icon, 0, Qt::AlignHCenter);
			auto *caption = new QLabel(T_(keys[i]), item);
			caption->setWordWrap(true);
			caption->setAlignment(Qt::AlignCenter);
			caption->setStyleSheet("font-size: 10px;"); /* colour follows the OBS theme */
			layout->addWidget(caption);
			steps->addWidget(item, 1);
		}
		v->addLayout(steps);
	}
	auto *l = new QLabel(configured > 0 ? T_("Dock.ImportText") : T_("Dock.SetupText"), box);
	l->setWordWrap(true);
	auto *b = new QPushButton(T_("Dock.Import"), box);
	b->setObjectName("import_btn");
	b->setMinimumHeight(34);
	connect(b, &QPushButton::clicked, this, [this]() { invoke_queued(this, [this]() { importScene(); }); });
	v->addWidget(l);
	v->addWidget(b);
	return box;
}

QWidget *KageeDock::buildShotsPage(obs_source_t *src)
{
	auto *w = new QWidget();
	auto *v = new QVBoxLayout(w);
	if (QWidget *banner = setupBanner(src, false)) {
		banner->setParent(w);
		v->addWidget(banner);
	}

	auto *grid = new QGridLayout();
	grid->setSpacing(6);
	for (int i = 0; i <= MAX_SHOTS; i++) {
		auto *cell = new QVBoxLayout();
		cell->setSpacing(2);
		auto *go = new QPushButton(i == 0 ? T_("Dock.Live") : QString::number(i), w);
		go->setCheckable(true);
		go->setMinimumHeight(46);
		go->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		go->setToolTip(i == 0 ? T_("Dock.LiveTip") : T_("Dock.GoTip"));
		QFont f = go->font();
		f.setPointSizeF(f.pointSizeF() * 1.4);
		f.setBold(true);
		go->setFont(f);
		connect(go, &QPushButton::clicked, this, [this, i]() { callAction("kagee_shot", nullptr, i); });
		cell->addWidget(go);
		shotButtons.push_back(go);

		if (i > 0) {
			auto *save = new QPushButton(T_("Dock.Save"), w);
			save->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
			save->setToolTip(T_("Dock.SaveTip"));
			connect(save, &QPushButton::clicked, this,
				[this, i]() { callAction("kagee_save_shot", nullptr, i); });
			cell->addWidget(save);
			saveButtons.push_back(save);
		} else {
			auto *spacer = new QLabel(T_("Dock.LiveHint"), w);
			spacer->setAlignment(Qt::AlignCenter);
			spacer->setStyleSheet("font-size: 10px;");
			cell->addWidget(spacer);
		}
		grid->addLayout(cell, i / 3, i % 3);
	}
	v->addLayout(grid);

	auto *actions = new QGridLayout();
	struct btn {
		const char *label, *action;
	} defs[] = {{"Dock.Prev", "prev"},     {"Dock.Next", "next"},   {"Dock.Random", "random"},
		    {"Dock.Impact", "impact"}, {"Dock.Flash", "flash"}, {"Dock.Tap", "tap"}};
	for (int i = 0; i < 6; i++) {
		auto *b = new QPushButton(T_(defs[i].label), w);
		b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		const char *act = defs[i].action;
		connect(b, &QPushButton::clicked, this, [this, act]() { callAction("kagee_action", act); });
		actions->addWidget(b, i / 3, i % 3);
		if (strcmp(act, "tap") == 0)
			tapButton = b;
	}
	autoButton = new QPushButton(T_("Dock.Auto"), w);
	autoButton->setCheckable(true);
	connect(autoButton, &QPushButton::clicked, this, [this]() { callAction("kagee_action", "auto"); });
	actions->addWidget(autoButton, 2, 0, 1, 3);
	v->addLayout(actions);

	auto *view = new PropView(src, {"trans_dur", "trans_ease", "edit_smooth", "auto_mode", "auto_interval",
					"auto_beats", "auto_order", "bpm"});
	view->setParent(w);
	v->addWidget(view);
	v->addStretch();
	pollState();
	return w;
}

QWidget *KageeDock::buildLayersPage(obs_source_t *src)
{
	auto *w = new QWidget();
	auto *v = new QVBoxLayout(w);
	auto *row = new QHBoxLayout();
	row->setSpacing(2);
	for (int i = 1; i <= MAX_LAYERS; i++) {
		auto *b = new QPushButton(QString::number(i), w);
		b->setObjectName(QString("layer_btn_%1").arg(i));
		b->setCheckable(true);
		b->setChecked(i == currentLayer);
		b->setMinimumWidth(0);
		b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		connect(b, &QPushButton::clicked, this, [this, i]() {
			currentLayer = i;
			/* rebuild after this click returns: the button belongs to the page being replaced */
			invoke_queued(this, [this]() {
				obs_source_t *s = getTarget();
				if (s) {
					setPage(T_LAYERS, buildLayersPage(s));
					obs_source_release(s);
				}
			});
		});
		row->addWidget(b);
	}
	v->addLayout(row);
	auto *hint = new QLabel(T_("Dock.LayerHint"), w);
	hint->setWordWrap(true);
	hint->setStyleSheet("font-size: 11px;");
	v->addWidget(hint);

	if (QWidget *banner = setupBanner(src, true)) {
		banner->setParent(w);
		v->addWidget(banner);
	}

	std::string key = "l" + std::to_string(currentLayer) + "_on";
	auto *view = new PropView(src, {key, "layer_count"});
	view->setParent(w);
	v->addWidget(view);
	v->addStretch();
	return w;
}

struct filter_find {
	const char *id;
	obs_source_t *found;
};

QWidget *KageeDock::buildFilterPage(obs_source_t *src, const char *filter_id)
{
	filter_find ff = {filter_id, nullptr};
	obs_source_enum_filters(
		src,
		[](obs_source_t *, obs_source_t *child, void *param) {
			auto *f = static_cast<filter_find *>(param);
			if (!f->found && strcmp(obs_source_get_unversioned_id(child), f->id) == 0)
				f->found = obs_source_get_ref(child);
		},
		&ff);

	QString kind = QString::fromUtf8(obs_source_get_display_name(filter_id));
	if (!ff.found) {
		auto *w = new QWidget();
		auto *v = new QVBoxLayout(w);
		auto *l = new QLabel(T_("Dock.NoFilter").arg(kind), w);
		l->setWordWrap(true);
		auto *add = new QPushButton(T_("Dock.AddFilter").arg(kind), w);
		std::string id = filter_id;
		connect(add, &QPushButton::clicked, this, [this, id, kind]() {
			obs_source_t *t = getTarget();
			if (!t)
				return;
			QString name = kind;
			for (int n = 2;; n++) {
				obs_source_t *dup = obs_source_get_filter_by_name(t, name.toUtf8().constData());
				if (!dup)
					break;
				obs_source_release(dup);
				name = QString("%1 %2").arg(kind).arg(n);
			}
			obs_source_t *f = obs_source_create(id.c_str(), name.toUtf8().constData(), nullptr, nullptr);
			if (f) {
				obs_source_filter_add(t, f);
				obs_source_release(f);
			}
			obs_source_release(t);
		});
		v->addWidget(l);
		v->addWidget(add);
		v->addStretch();
		return w;
	}

	auto *head = new QWidget();
	auto *h = new QHBoxLayout(head);
	h->setContentsMargins(0, 0, 0, 0);
	auto *enabled = new QCheckBox(T_("Dock.Enabled"), head);
	enabled->setChecked(obs_source_enabled(ff.found));
	obs_weak_source_t *wf = obs_source_get_weak_source(ff.found);
	connect(enabled, &QCheckBox::toggled, head, [wf](bool on) {
		obs_source_t *f = obs_weak_source_get_source(wf);
		if (f) {
			obs_source_set_enabled(f, on);
			obs_source_release(f);
		}
	});
	connect(head, &QObject::destroyed, [wf]() { obs_weak_source_release(wf); });
	auto *remove = new QPushButton(T_("Dock.Remove"), head);
	obs_weak_source_t *wf2 = obs_source_get_weak_source(ff.found);
	connect(remove, &QPushButton::clicked, this, [this, wf2]() {
		obs_source_t *f = obs_weak_source_get_source(wf2);
		obs_source_t *t = getTarget();
		if (f && t)
			obs_source_filter_remove(t, f);
		obs_source_release(f);
		obs_source_release(t);
	});
	connect(head, &QObject::destroyed, [wf2]() { obs_weak_source_release(wf2); });
	h->addWidget(enabled, 1);
	h->addWidget(remove);

	QWidget *page = with_view(head, new PropView(ff.found));
	obs_source_release(ff.found);
	return page;
}

QWidget *KageeDock::buildEffectsPage(obs_source_t *src)
{
	auto *w = new QWidget();
	auto *v = new QVBoxLayout(w);

	auto *addRow = new QHBoxLayout();
	auto *kinds = new QComboBox(w);
	kinds->setObjectName("fx_kinds");
	for (const char *id : FX_IDS)
		kinds->addItem(QString::fromUtf8(obs_source_get_display_name(id)), QString::fromUtf8(id));
	auto *add = new QPushButton(T_("Dock.Add"), w);
	add->setObjectName("fx_add");
	connect(add, &QPushButton::clicked, this, [this, kinds]() {
		obs_source_t *t = getTarget();
		if (!t)
			return;
		QByteArray id = kinds->currentData().toString().toUtf8();
		QString kind = kinds->currentText();
		QString name = kind;
		for (int n = 2;; n++) {
			obs_source_t *dup = obs_source_get_filter_by_name(t, name.toUtf8().constData());
			if (!dup)
				break;
			obs_source_release(dup);
			name = QString("%1 %2").arg(kind).arg(n);
		}
		obs_source_t *f = obs_source_create(id.constData(), name.toUtf8().constData(), nullptr, nullptr);
		if (f) {
			obs_source_filter_add(t, f);
			obs_source_release(f);
		}
		obs_source_release(t);
	});
	addRow->addWidget(kinds, 1);
	addRow->addWidget(add);
	v->addLayout(addRow);

	std::vector<obs_source_t *> filters;
	obs_source_enum_filters(
		src,
		[](obs_source_t *, obs_source_t *child, void *param) {
			const char *id = obs_source_get_unversioned_id(child);
			for (const char *fx : FX_IDS)
				if (strcmp(id, fx) == 0)
					static_cast<std::vector<obs_source_t *> *>(param)->push_back(obs_source_get_ref(child));
		},
		&filters);

	if (filters.empty()) {
		auto *l = new QLabel(T_("Dock.NoEffects"), w);
		l->setWordWrap(true);
		v->addWidget(l);
	}
	for (obs_source_t *f : filters) {
		auto *box = new QGroupBox(QString::fromUtf8(obs_source_get_name(f)), w);
		box->setCheckable(true);
		box->setChecked(obs_source_enabled(f));
		obs_weak_source_t *wf = obs_source_get_weak_source(f);
		connect(box, &QGroupBox::toggled, box, [wf](bool on) {
			obs_source_t *s = obs_weak_source_get_source(wf);
			if (s) {
				obs_source_set_enabled(s, on);
				obs_source_release(s);
			}
		});
		auto *bl = new QVBoxLayout(box);
		auto *remove = new QPushButton(T_("Dock.Remove"), box);
		connect(remove, &QPushButton::clicked, this, [this, wf]() {
			obs_source_t *s = obs_weak_source_get_source(wf);
			obs_source_t *t = getTarget();
			if (s && t)
				obs_source_filter_remove(t, s);
			obs_source_release(s);
			obs_source_release(t);
		});
		connect(box, &QObject::destroyed, [wf]() { obs_weak_source_release(wf); });
		auto *view = new PropView(f);
		view->setParent(box);
		bl->addWidget(view);
		bl->addWidget(remove, 0, Qt::AlignRight);
		v->addWidget(box);
		obs_source_release(f);
	}
	v->addStretch();
	return w;
}

/* ------------------------------------------------------------------------- */

void KageeDock::showTab(int index)
{
	stack->setCurrentIndex(index);
	navButtons[index]->setChecked(true);
}

void KageeDock::pollState()
{
	if (!targetIsStage || (shotButtons.empty() && easyShotButtons.empty()))
		return;
	obs_source_t *src = getTarget();
	if (!src)
		return;

	calldata_t cd;
	calldata_init(&cd);
	proc_handler_call(obs_source_get_proc_handler(src), "kagee_state", &cd);
	long long cur = calldata_int(&cd, "shot");
	bool autoOn = calldata_bool(&cd, "auto");
	double bpm = calldata_float(&cd, "bpm");
	calldata_free(&cd);

	obs_data_t *s = obs_source_get_settings(src);
	for (int i = 0; i < (int)shotButtons.size(); i++) {
		QPushButton *b = shotButtons[i];
		bool valid = true;
		if (i > 0) {
			std::string key = "shot" + std::to_string(i) + "_valid";
			valid = obs_data_get_bool(s, key.c_str());
		}
		b->setChecked(i == cur);
		b->setEnabled(valid);
		if (i > 0)
			b->setText(valid ? QString::number(i) : QString("%1 %2").arg(i).arg(T_("Dock.Empty")));
	}
	for (int i = 0; i < (int)easyShotButtons.size(); i++) {
		QPushButton *b = easyShotButtons[i];
		bool valid = true;
		if (i > 0) {
			std::string key = "shot" + std::to_string(i) + "_valid";
			valid = obs_data_get_bool(s, key.c_str());
		}
		b->setChecked(i == cur);
		b->setEnabled(valid);
		/* restyle only when the saved/empty state changes (this runs every 150 ms) */
		QVariant prev = b->property("savedShot");
		if (i > 0 && (!prev.isValid() || prev.toBool() != valid)) {
			b->setToolTip(valid ? T_("Dock.ShotSavedTip") : T_("Dock.ShotEmptyTip"));
			b->setProperty("savedShot", valid);
			b->style()->unpolish(b);
			b->style()->polish(b);
		}
	}
	for (int i = 0; i < (int)easyPresetCombos.size(); i++) {
		QComboBox *combo = easyPresetCombos[i];
		if (combo->view()->isVisible())
			continue; /* don't fight the user while the list is open */
		std::string vkey = "shot" + std::to_string(i + 1) + "_valid";
		std::string pkey = "shot" + std::to_string(i + 1) + "_preset";
		long long preset = obs_data_get_bool(s, vkey.c_str()) ? obs_data_get_int(s, pkey.c_str()) : -1;
		int idx = combo->findData(QVariant::fromValue<qlonglong>(preset));
		QSignalBlocker block(combo);
		combo->setCurrentIndex(idx < 0 ? 0 : idx);
	}
	obs_data_release(s);
	if (autoButton)
		autoButton->setChecked(autoOn);
	if (tapButton)
		tapButton->setText(QString("%1  %2 BPM").arg(T_("Dock.Tap")).arg(bpm, 0, 'f', 1));
	obs_source_release(src);
}

/* Debug aid: KAGEE_DOCK_SNAPSHOT=<dir> saves a PNG of every tab after startup. */
void KageeDock::snapshot(const QString &dir)
{
	QDir().mkpath(dir);
	for (QWidget *p = parentWidget(); p; p = p->parentWidget()) {
		if (auto *dock = qobject_cast<QDockWidget *>(p)) {
			dock->setFloating(true);
			dock->show();
			dock->resize(460, 980);
			break;
		}
	}
	for (int i = 0; i < T_COUNT; i++) {
		showTab(i);
		QWidget *content = pages[i]->widget();
		grab().save(QString("%1/tab_%2.png").arg(dir).arg(i));
		if (content) {
			content->grab().save(QString("%1/tab_%2_full.png").arg(dir).arg(i));
		}
	}
	if (QWidget *easy = easyPage->widget()) {
		int wdt = easyPage->viewport()->width();
		easy->resize(wdt, easy->heightForWidth(wdt) > 0 ? easy->heightForWidth(wdt) : easy->sizeHint().height());
		easy->grab().save(QString("%1/easy_full.png").arg(dir));
	}
	showTab(0);
	blog(LOG_INFO, "[Kagee] dock snapshots written to %s", dir.toUtf8().constData());
}

/* ------------------------------------------------------------------------- */
/* Debug aid: KAGEE_DOCK_SELFTEST=1 drives the real widgets and logs PASS/FAIL. */

namespace {
struct SelfTest {
	int passed = 0, failed = 0;
	void check(const char *name, bool ok)
	{
		(ok ? passed : failed)++;
		blog(LOG_INFO, "[Kagee] selftest %s  %s", ok ? "PASS" : "FAIL", name);
	}
};
} // namespace

void KageeDock::selfTest()
{
	auto *t = new SelfTest();
	auto later = [this](int ms, std::function<void()> fn) { QTimer::singleShot(ms, this, fn); };
	auto page = [this](Tab tab) { return pages[tab]->widget(); };
	auto setting_double = [this](const char *key) {
		obs_source_t *src = getTarget();
		obs_data_t *s = obs_source_get_settings(src);
		double v = obs_data_get_double(s, key);
		obs_data_release(s);
		obs_source_release(src);
		return v;
	};

	if (!targetIsStage) {
		t->check("target is a stage", false);
		return;
	}

	/* 1. dock -> source */
	showTab(T_CAMERA);
	QWidget *camx = page(T_CAMERA)->findChild<QWidget *>("cam_x");
	auto *spin = camx ? camx->findChild<QDoubleSpinBox *>("spin") : nullptr;
	t->check("camera tab has a Pan X field", spin != nullptr);
	if (spin)
		spin->setValue(123.0);

	later(400, [=]() {
		t->check("dock edit reaches the source (cam_x == 123)", std::fabs(setting_double("cam_x") - 123.0) < 0.01);

		/* 2. source -> dock (e.g. edits from the properties window / websocket) */
		obs_source_t *src = getTarget();
		obs_data_t *d = obs_data_create();
		obs_data_set_double(d, "cam_y", -77.0);
		obs_source_update(src, d);
		obs_data_release(d);
		obs_source_release(src);

		later(400, [=]() {
			QWidget *camy = page(T_CAMERA)->findChild<QWidget *>("cam_y");
			auto *sy = camy ? camy->findChild<QDoubleSpinBox *>("spin") : nullptr;
			t->check("source edit shows in the dock (cam_y == -77)", sy && std::fabs(sy->value() + 77.0) < 0.01);

			/* 3. shot buttons */
			showTab(T_SHOTS);
			if (shotButtons.size() > 2)
				shotButtons[2]->click();
			later(400, [=]() {
				obs_source_t *s2 = getTarget();
				calldata_t cd;
				calldata_init(&cd);
				proc_handler_call(obs_source_get_proc_handler(s2), "kagee_state", &cd);
				t->check("shot button 2 moves the camera to shot 2", calldata_int(&cd, "shot") == 2);
				calldata_free(&cd);
				obs_source_release(s2);

				if (saveButtons.size() > 3)
					saveButtons[3]->click(); /* slot 4 */
				later(400, [=]() {
					obs_source_t *s3 = getTarget();
					obs_data_t *st = obs_source_get_settings(s3);
					t->check("save button stores slot 4", obs_data_get_bool(st, "shot4_valid"));
					obs_data_release(st);
					obs_source_release(s3);
					later(300, [=]() {
						t->check("slot 4 button becomes usable", shotButtons.size() > 4 && shotButtons[4]->isEnabled());

						/* 4. add a filter from the effects tab */
						showTab(T_FX);
						auto *kinds = page(T_FX)->findChild<QComboBox *>("fx_kinds");
						auto *add = page(T_FX)->findChild<QPushButton *>("fx_add");
						if (kinds && add) {
							kinds->setCurrentIndex(kinds->findData(QString("kagee_glitch_filter")));
							add->click();
						}
						later(600, [=]() {
							QString name = QString::fromUtf8(obs_source_get_display_name("kagee_glitch_filter"));
							obs_source_t *s4 = getTarget();
							obs_source_t *f = obs_source_get_filter_by_name(s4, name.toUtf8().constData());
							t->check("effects tab adds a filter to the source", f != nullptr);
							bool shown = false;
							for (auto *box : page(T_FX)->findChildren<QGroupBox *>())
								shown |= box->title() == name;
							t->check("effects tab rebuilds and shows the new filter", shown);
							if (f) {
								obs_source_filter_remove(s4, f);
								obs_source_release(f);
							}
							obs_source_release(s4);

							/* 5. layer selector */
							showTab(T_LAYERS);
							if (auto *b = page(T_LAYERS)->findChild<QPushButton *>("layer_btn_3"))
								b->click();
							later(300, [=]() {
								t->check("layer 3 selector shows layer 3",
									 page(T_LAYERS)->findChild<QGroupBox *>("l3_on") != nullptr);
								/* 6. easy mode: preset dropdown + auto shots */
								setMode(0);
								QComboBox *combo = easyPresetCombos.size() > 2 ? easyPresetCombos[2] : nullptr;
								int idx = combo ? combo->findData(QVariant::fromValue<qlonglong>(4)) : -1;
								t->check("easy mode has per-shot preset lists", combo && idx >= 0);
								if (combo && idx >= 0) {
									combo->setCurrentIndex(idx);
									emit combo->activated(idx);
								}
								later(500, [=]() {
									obs_source_t *s5 = getTarget();
									obs_data_t *st5 = obs_source_get_settings(s5);
									calldata_t cd5;
									calldata_init(&cd5);
									proc_handler_call(obs_source_get_proc_handler(s5), "kagee_state", &cd5);
									t->check("choosing a preset fills shot 3 and plays it",
										 obs_data_get_int(st5, "shot3_preset") == 4 &&
											 calldata_int(&cd5, "shot") == 3);
									calldata_free(&cd5);
									obs_data_release(st5);
									obs_source_release(s5);
									if (auto *b = easyPage->widget()->findChild<QPushButton *>("easy_auto_shots"))
										b->click();
									later(500, [=]() {
										obs_source_t *s6 = getTarget();
										obs_data_t *st6 = obs_source_get_settings(s6);
										int valid = 0;
										for (int k = 1; k <= 8; k++) {
											std::string key = "shot" + std::to_string(k) + "_valid";
											valid += obs_data_get_bool(st6, key.c_str()) ? 1 : 0;
										}
										t->check("auto button creates 8 shots", valid == 8);
										obs_data_release(st6);
										obs_source_release(s6);
										blog(LOG_INFO, "[Kagee] selftest done: %d passed, %d failed",
										     t->passed, t->failed);
										delete t;
									});
								});
							});
						});
					});
				});
			});
		});
	});
}

/* ------------------------------------------------------------------------- */
/* easy mode                                                                 */

static const char *PRESET_KEYS[] = {"Preset.Front", "Preset.Close", "Preset.Wide",  "Preset.Left",
				    "Preset.Right", "Preset.Low",   "Preset.High",  "Preset.Dutch",
				    "Preset.Truck", "Preset.Tele",  "Preset.WideAngle", "Preset.Drama"};
static const char *LOOK_KEYS[] = {"Grade.P.TealOrange", "Grade.P.Warm",   "Grade.P.Cool", "Grade.P.Vivid",
				  "Grade.P.Vintage",    "Grade.P.Matte",  "Grade.P.Bleach", "Grade.P.Mono",
				  "Grade.P.Sepia",      "Grade.P.Neon"};

void KageeDock::setMode(int m)
{
	mode = m == 1 ? 1 : 0;
	modeStack->setCurrentIndex(mode);
	modeButtons[mode]->setChecked(true);
	config_t *cfg = obs_frontend_get_user_config();
	config_set_int(cfg, "Kagee", "DockMode", mode);
}

static obs_source_t *find_filter(obs_source_t *src, const char *id)
{
	filter_find ff = {id, nullptr};
	obs_source_enum_filters(
		src,
		[](obs_source_t *, obs_source_t *child, void *param) {
			auto *f = static_cast<filter_find *>(param);
			if (!f->found && strcmp(obs_source_get_unversioned_id(child), f->id) == 0)
				f->found = obs_source_get_ref(child);
		},
		&ff);
	return ff.found;
}

/* returns a new reference to the filter, creating it (named after its kind) if needed */
static obs_source_t *ensure_filter(obs_source_t *src, const char *id)
{
	obs_source_t *f = find_filter(src, id);
	if (f)
		return f;
	const char *kind = obs_source_get_display_name(id);
	f = obs_source_create(id, kind, nullptr, nullptr);
	if (f)
		obs_source_filter_add(src, f);
	return f;
}

QWidget *KageeDock::buildLookSection(obs_source_t *src)
{
	auto *box = new QGroupBox(T_("Dock.Look"));
	auto *form = new QFormLayout(box);
	form->setRowWrapPolicy(QFormLayout::WrapLongRows);

	/* colour look = the Color Grade filter's preset */
	auto *look = new QComboBox(box);
	look->setObjectName("easy_look");
	look->addItem(T_("None"), 0);
	for (int i = 0; i < 10; i++)
		look->addItem(T_(LOOK_KEYS[i]), i + 1);
	obs_source_t *grade = find_filter(src, "kagee_grade_filter");
	if (grade) {
		obs_data_t *gs = obs_source_get_settings(grade);
		int preset = obs_source_enabled(grade) ? (int)obs_data_get_int(gs, "preset") : 0;
		look->setCurrentIndex(look->findData(preset) < 0 ? 0 : look->findData(preset));
		obs_data_release(gs);
		obs_source_release(grade);
	}
	connect(look, &QComboBox::currentIndexChanged, this, [this, look](int) {
		obs_source_t *t = getTarget();
		if (!t)
			return;
		int preset = look->currentData().toInt();
		obs_source_t *f = preset ? ensure_filter(t, "kagee_grade_filter") : find_filter(t, "kagee_grade_filter");
		if (f) {
			if (preset) {
				obs_data_t *d = obs_data_create();
				obs_data_set_int(d, "preset", preset);
				obs_source_update(f, d);
				obs_data_release(d);
			}
			obs_source_set_enabled(f, preset != 0);
			obs_source_release(f);
		}
		obs_source_release(t);
	});
	form->addRow(T_("Dock.LookPreset"), look);

	/* lighting on/off = the Lighting filter's enabled state */
	auto *light = new QCheckBox(box);
	light->setObjectName("easy_light");
	obs_source_t *lf = find_filter(src, "kagee_light_filter");
	light->setChecked(lf && obs_source_enabled(lf));
	obs_source_release(lf);
	connect(light, &QCheckBox::toggled, this, [this](bool on) {
		obs_source_t *t = getTarget();
		if (!t)
			return;
		obs_source_t *f = on ? ensure_filter(t, "kagee_light_filter") : find_filter(t, "kagee_light_filter");
		if (f) {
			obs_source_set_enabled(f, on);
			obs_source_release(f);
		}
		obs_source_release(t);
	});
	form->addRow(T_("Dock.LightOn"), light);
	auto *hint = new QLabel(T_("Dock.LookHint"), box);
	hint->setWordWrap(true);
	hint->setStyleSheet("font-size: 11px;");
	form->addRow(hint);
	return box;
}

QWidget *KageeDock::buildEasyPage(obs_source_t *src)
{
	auto *w = new QWidget();
	auto *v = new QVBoxLayout(w);

	if (!targetIsStage) {
		QWidget *look = buildLookSection(src);
		look->setParent(w);
		v->addWidget(look);
		auto *l = new QLabel(T_("Dock.EasyAdjust"), w);
		l->setWordWrap(true);
		v->addWidget(l);
		v->addStretch();
		return w;
	}

	if (QWidget *banner = setupBanner(src, false)) {
		banner->setParent(w);
		v->addWidget(banner);
	}

	/* shots */
	auto *shots = new QGroupBox(T_("Dock.Shots"), w);
	auto *sv = new QVBoxLayout(shots);
	auto *top = new QHBoxLayout();
	auto *live = new QPushButton(T_("Dock.Live"), shots);
	live->setCheckable(true);
	live->setMinimumHeight(32);
	live->setToolTip(T_("Dock.LiveTip"));
	connect(live, &QPushButton::clicked, this, [this]() { callAction("kagee_shot", nullptr, 0); });
	easyShotButtons.push_back(live);
	auto *prev = new QPushButton(T_("Dock.Prev"), shots);
	auto *next = new QPushButton(T_("Dock.Next"), shots);
	connect(prev, &QPushButton::clicked, this, [this]() { callAction("kagee_action", "prev"); });
	connect(next, &QPushButton::clicked, this, [this]() { callAction("kagee_action", "next"); });
	for (QPushButton *b : {live, prev, next}) {
		b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		top->addWidget(b);
	}
	sv->addLayout(top);

	auto *grid = new QGridLayout();
	grid->setColumnStretch(1, 1);
	for (int i = 1; i <= 8; i++) {
		auto *go = new QPushButton(QString::number(i), shots);
		go->setCheckable(true);
		go->setMinimumSize(44, 30);
		go->setToolTip(T_("Dock.ShotEmptyTip"));
		go->setObjectName(QString("easy_shot_%1").arg(i));
		/* one stylesheet for all states; pollState only flips the savedShot property */
		go->setStyleSheet("QPushButton[savedShot=\"true\"] { border: 1px solid #6687f5; }"
				  "QPushButton:checked { background: #3157c8; border: 1px solid #8fa7ff; }");
		QFont f = go->font();
		f.setBold(true);
		go->setFont(f);
		connect(go, &QPushButton::clicked, this, [this, i]() { callAction("kagee_shot", nullptr, i); });
		easyShotButtons.push_back(go);

		auto *combo = new QComboBox(shots);
		combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		combo->setMinimumContentsLength(6);
		combo->addItem(T_("Dock.PresetPick"), QVariant::fromValue<qlonglong>(-1));
		combo->addItem(T_("Dock.PresetCustom"), QVariant::fromValue<qlonglong>(0));
		for (int k = 0; k < 12; k++)
			combo->addItem(T_(PRESET_KEYS[k]), QVariant::fromValue<qlonglong>(k + 1));
		combo->setToolTip(T_("Dock.PresetTip"));
		connect(combo, &QComboBox::activated, this, [this, combo, i](int idx) {
			long long preset = combo->itemData(idx).toLongLong();
			if (preset > 0) {
				obs_source_t *t = getTarget();
				if (!t)
					return;
				calldata_t cd;
				calldata_init(&cd);
				calldata_set_int(&cd, "index", i);
				calldata_set_int(&cd, "preset", preset);
				proc_handler_call(obs_source_get_proc_handler(t), "kagee_apply_preset", &cd);
				calldata_free(&cd);
				obs_source_release(t);
				QTimer::singleShot(0, this, &KageeDock::pollState);
				callAction("kagee_shot", nullptr, i); /* show it right away */
			}
		});
		easyPresetCombos.push_back(combo);

		auto *save = new QPushButton(T_("Dock.Save"), shots);
		save->setToolTip(T_("Dock.SaveTip"));
		connect(save, &QPushButton::clicked, this, [this, i]() { callAction("kagee_save_shot", nullptr, i); });

		grid->addWidget(go, i - 1, 0);
		grid->addWidget(combo, i - 1, 1);
		grid->addWidget(save, i - 1, 2);
	}
	sv->addLayout(grid);
	auto *autoShots = new QPushButton(T_("Dock.AutoShots"), shots);
	autoShots->setObjectName("easy_auto_shots");
	connect(autoShots, &QPushButton::clicked, this, [this]() { callAction("kagee_auto_shots", nullptr); });
	sv->addWidget(autoShots);
	auto *shotHint = new QLabel(T_("Dock.ShotHint"), shots);
	shotHint->setWordWrap(true);
	shotHint->setStyleSheet("font-size: 11px;");
	sv->addWidget(shotHint);
	v->addWidget(shots);

	/* a handful of the most useful settings, straight from the stage's properties */
	auto *view = new PropView(src, {"cam_yaw", "cam_pitch", "cam_dolly", "cam_fov", "btn_reset_cam", "trans_dur",
					"depth_scale", "dof_strength", "hand_amount", "idle_mode", "auto_mode",
					"auto_interval", "auto_beats", "bpm"});
	view->setParent(w);
	v->addWidget(view);

	QWidget *look = buildLookSection(src);
	look->setParent(w);
	v->addWidget(look);
	v->addStretch();
	return w;
}
