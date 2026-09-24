#include "prop-view.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <cmath>

static constexpr int SLIDER_STEPS = 1000;

static QColor color_from_obs(long long v)
{
	uint32_t c = (uint32_t)v;
	return QColor(c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff, (c >> 24) & 0xff);
}

static long long color_to_obs(const QColor &c)
{
	return (long long)((uint32_t)c.red() | ((uint32_t)c.green() << 8) | ((uint32_t)c.blue() << 16) |
			   ((uint32_t)c.alpha() << 24));
}

static void paint_swatch(QPushButton *b, const QColor &c)
{
	b->setText(c.name(QColor::HexArgb).toUpper());
	QColor text = c.lightnessF() > 0.55 ? Qt::black : Qt::white;
	b->setStyleSheet(QString("background-color: %1; color: %2;").arg(c.name(QColor::HexRgb), text.name()));
}

PropView::PropView(obs_source_t *source, std::set<std::string> include_, QWidget *parent)
	: QWidget(parent),
	  include(std::move(include_))
{
	weak = obs_source_get_weak_source(source);
	props = obs_source_properties(source);

	auto *form = new QFormLayout(this);
	form->setContentsMargins(0, 0, 0, 0);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	form->setRowWrapPolicy(QFormLayout::WrapLongRows);
	if (props) {
		build(props, form, include.empty());
		obs_data_t *settings = obs_source_get_settings(source);
		obs_properties_apply_settings(props, settings);
		obs_data_release(settings);
	}
	refreshValues();

	signal_handler_connect(obs_source_get_signal_handler(source), "update", onUpdate, this);
}

PropView::~PropView()
{
	obs_source_t *src = getSource();
	if (src) {
		signal_handler_disconnect(obs_source_get_signal_handler(src), "update", onUpdate, this);
		obs_source_release(src);
	}
	obs_weak_source_release(weak);
	obs_properties_destroy(props);
}

obs_source_t *PropView::getSource() const
{
	return weak ? obs_weak_source_get_source(weak) : nullptr;
}

void PropView::onUpdate(void *data, calldata_t *)
{
	QPointer<PropView> self = static_cast<PropView *>(data);
	QMetaObject::invokeMethod(
		self,
		[self]() {
			if (self)
				self->refreshValues();
		},
		Qt::QueuedConnection);
}

bool PropView::wanted(obs_property_t *p) const
{
	return include.count(obs_property_name(p)) > 0;
}

bool PropView::hasWantedDescendant(obs_properties_t *ps) const
{
	for (obs_property_t *p = obs_properties_first(ps); p; obs_property_next(&p)) {
		if (wanted(p))
			return true;
		if (obs_property_get_type(p) == OBS_PROPERTY_GROUP && hasWantedDescendant(obs_property_group_content(p)))
			return true;
	}
	return false;
}

void PropView::build(obs_properties_t *ps, QFormLayout *layout, bool all)
{
	for (obs_property_t *p = obs_properties_first(ps); p; obs_property_next(&p)) {
		bool full = all || wanted(p);
		obs_property_type type = obs_property_get_type(p);

		if (type == OBS_PROPERTY_GROUP) {
			obs_properties_t *content = obs_property_group_content(p);
			if (!full && !hasWantedDescendant(content))
				continue;
			auto *box = new QGroupBox(QString::fromUtf8(obs_property_description(p)), this);
			box->setObjectName(QString::fromUtf8(obs_property_name(p)));
			auto *inner = new QFormLayout(box);
			inner->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
			inner->setRowWrapPolicy(QFormLayout::WrapLongRows);
			if (obs_property_group_type(p) == OBS_GROUP_CHECKABLE) {
				box->setCheckable(true);
				connect(box, &QGroupBox::toggled, this, [this, p](bool on) {
					if (updating)
						return;
					obs_source_t *src = getSource();
					if (!src)
						return;
					obs_data_t *s = obs_source_get_settings(src);
					obs_data_set_bool(s, obs_property_name(p), on);
					changed(p, s);
					obs_data_release(s);
					obs_source_release(src);
				});
			}
			layout->addRow(box);
			rows.push_back({p, layout, box, nullptr, obs_property_name(p)});
			build(content, inner, full);
			continue;
		}
		if (!full)
			continue;

		QWidget *field = makeField(p, this);
		if (!field)
			continue;
		field->setObjectName(QString::fromUtf8(obs_property_name(p)));
		QLabel *label = nullptr;
		bool wide = type == OBS_PROPERTY_BUTTON ||
			    (type == OBS_PROPERTY_TEXT && obs_property_text_type(p) == OBS_TEXT_INFO);
		if (wide) {
			layout->addRow(field);
		} else {
			label = new QLabel(QString::fromUtf8(obs_property_description(p)), this);
			label->setWordWrap(true);
			label->setBuddy(field);
			layout->addRow(label, field);
		}
		rows.push_back({p, layout, field, label, obs_property_name(p)});
	}
}

QWidget *PropView::makeField(obs_property_t *p, QWidget *parent)
{
	const char *name = obs_property_name(p);
	auto withSettings = [this](auto fn) {
		if (updating)
			return;
		obs_source_t *src = getSource();
		if (!src)
			return;
		obs_data_t *s = obs_source_get_settings(src);
		fn(s);
		obs_data_release(s);
		obs_source_release(src);
	};

	switch (obs_property_get_type(p)) {
	case OBS_PROPERTY_BOOL: {
		/* text goes in the (wrapping) row label: long checkbox texts can't wrap */
		auto *cb = new QCheckBox(parent);
		connect(cb, &QCheckBox::toggled, this, [=](bool on) {
			withSettings([&](obs_data_t *s) {
				obs_data_set_bool(s, name, on);
				changed(p, s);
			});
		});
		return cb;
	}
	case OBS_PROPERTY_INT: {
		auto *w = new QWidget(parent);
		auto *h = new QHBoxLayout(w);
		h->setContentsMargins(0, 0, 0, 0);
		auto *spin = new QSpinBox(w);
		spin->setObjectName("spin");
		spin->setRange(obs_property_int_min(p), obs_property_int_max(p));
		spin->setSingleStep(obs_property_int_step(p));
		if (obs_property_int_type(p) == OBS_NUMBER_SLIDER) {
			auto *sl = new QSlider(Qt::Horizontal, w);
			sl->setObjectName("slider");
			sl->setRange(spin->minimum(), spin->maximum());
			sl->setMinimumWidth(40);
			h->addWidget(sl, 1);
			connect(sl, &QSlider::valueChanged, spin, &QSpinBox::setValue);
			connect(spin, &QSpinBox::valueChanged, sl, [sl](int v) {
				QSignalBlocker b(sl);
				sl->setValue(v);
			});
		}
		h->addWidget(spin);
		connect(spin, &QSpinBox::valueChanged, this, [=](int v) {
			withSettings([&](obs_data_t *s) {
				obs_data_set_int(s, name, v);
				changed(p, s);
			});
		});
		return w;
	}
	case OBS_PROPERTY_FLOAT: {
		auto *w = new QWidget(parent);
		auto *h = new QHBoxLayout(w);
		h->setContentsMargins(0, 0, 0, 0);
		double mn = obs_property_float_min(p), mx = obs_property_float_max(p), st = obs_property_float_step(p);
		auto *spin = new QDoubleSpinBox(w);
		spin->setObjectName("spin");
		spin->setRange(mn, mx);
		spin->setSingleStep(st);
		int decimals = st >= 1.0 ? 0 : std::min(4, (int)std::ceil(-std::log10(st)));
		spin->setDecimals(decimals);
		spin->setMinimumWidth(64);
		if (obs_property_float_type(p) == OBS_NUMBER_SLIDER && mx > mn) {
			auto *sl = new QSlider(Qt::Horizontal, w);
			sl->setObjectName("slider");
			sl->setRange(0, SLIDER_STEPS);
			sl->setMinimumWidth(40);
			h->addWidget(sl, 1);
			connect(sl, &QSlider::valueChanged, spin,
				[=](int v) { spin->setValue(mn + (mx - mn) * v / SLIDER_STEPS); });
			connect(spin, &QDoubleSpinBox::valueChanged, sl, [=](double v) {
				QSignalBlocker b(sl);
				sl->setValue((int)std::lround((v - mn) / (mx - mn) * SLIDER_STEPS));
			});
		}
		h->addWidget(spin);
		connect(spin, &QDoubleSpinBox::valueChanged, this, [=](double v) {
			withSettings([&](obs_data_t *s) {
				obs_data_set_double(s, name, v);
				changed(p, s);
			});
		});
		return w;
	}
	case OBS_PROPERTY_TEXT: {
		if (obs_property_text_type(p) == OBS_TEXT_INFO) {
			auto *l = new QLabel(QString::fromUtf8(obs_property_description(p)), parent);
			l->setWordWrap(true);
			l->setStyleSheet("font-size: 11px; padding: 4px; border-left: 2px solid palette(highlight);");
			return l;
		}
		auto *e = new QLineEdit(parent);
		connect(e, &QLineEdit::editingFinished, this, [=]() {
			withSettings([&](obs_data_t *s) {
				obs_data_set_string(s, name, e->text().toUtf8().constData());
				changed(p, s);
			});
		});
		return e;
	}
	case OBS_PROPERTY_PATH: {
		auto *w = new QWidget(parent);
		auto *h = new QHBoxLayout(w);
		h->setContentsMargins(0, 0, 0, 0);
		auto *e = new QLineEdit(w);
		e->setObjectName("path");
		e->setMinimumWidth(60);
		auto *b = new QPushButton("...", w);
		b->setFixedWidth(32);
		h->addWidget(e, 1);
		h->addWidget(b);
		auto commit = [=](const QString &path) {
			withSettings([&](obs_data_t *s) {
				obs_data_set_string(s, name, path.toUtf8().constData());
				changed(p, s);
			});
		};
		connect(e, &QLineEdit::editingFinished, this, [=]() { commit(e->text()); });
		connect(b, &QPushButton::clicked, this, [=]() {
			QString filter = QString::fromUtf8(obs_property_path_filter(p));
			QString path;
			switch (obs_property_path_type(p)) {
			case OBS_PATH_FILE_SAVE:
				path = QFileDialog::getSaveFileName(this, QString(), e->text(), filter);
				break;
			case OBS_PATH_DIRECTORY:
				path = QFileDialog::getExistingDirectory(this, QString(), e->text());
				break;
			default:
				path = QFileDialog::getOpenFileName(this, QString(), e->text(), filter);
				break;
			}
			if (!path.isEmpty()) {
				e->setText(path);
				commit(path);
			}
		});
		return w;
	}
	case OBS_PROPERTY_LIST: {
		auto *c = new QComboBox(parent);
		c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		c->setMinimumContentsLength(6);
		obs_combo_format fmt = obs_property_list_format(p);
		size_t n = obs_property_list_item_count(p);
		for (size_t i = 0; i < n; i++) {
			QString text = QString::fromUtf8(obs_property_list_item_name(p, i));
			if (fmt == OBS_COMBO_FORMAT_INT)
				c->addItem(text, QVariant::fromValue<qlonglong>(obs_property_list_item_int(p, i)));
			else if (fmt == OBS_COMBO_FORMAT_FLOAT)
				c->addItem(text, obs_property_list_item_float(p, i));
			else
				c->addItem(text, QString::fromUtf8(obs_property_list_item_string(p, i)));
		}
		connect(c, &QComboBox::currentIndexChanged, this, [=](int idx) {
			if (idx < 0)
				return;
			QVariant v = c->itemData(idx);
			withSettings([&](obs_data_t *s) {
				if (fmt == OBS_COMBO_FORMAT_INT)
					obs_data_set_int(s, name, v.toLongLong());
				else if (fmt == OBS_COMBO_FORMAT_FLOAT)
					obs_data_set_double(s, name, v.toDouble());
				else
					obs_data_set_string(s, name, v.toString().toUtf8().constData());
				changed(p, s);
			});
		});
		return c;
	}
	case OBS_PROPERTY_COLOR:
	case OBS_PROPERTY_COLOR_ALPHA: {
		bool alpha = obs_property_get_type(p) == OBS_PROPERTY_COLOR_ALPHA;
		auto *b = new QPushButton(parent);
		connect(b, &QPushButton::clicked, this, [=]() {
			obs_source_t *src = getSource();
			if (!src)
				return;
			obs_data_t *s = obs_source_get_settings(src);
			QColor cur = color_from_obs(obs_data_get_int(s, name));
			if (!alpha)
				cur.setAlpha(255);
			QColor c = QColorDialog::getColor(cur, this, QString::fromUtf8(obs_property_description(p)),
							  alpha ? QColorDialog::ShowAlphaChannel
								: QColorDialog::ColorDialogOptions());
			if (c.isValid()) {
				if (!alpha)
					c.setAlpha(255);
				obs_data_set_int(s, name, color_to_obs(c));
				paint_swatch(b, c);
				changed(p, s);
			}
			obs_data_release(s);
			obs_source_release(src);
		});
		return b;
	}
	case OBS_PROPERTY_BUTTON: {
		auto *b = new QPushButton(QString::fromUtf8(obs_property_description(p)), parent);
		connect(b, &QPushButton::clicked, this, [=]() {
			obs_source_t *src = getSource();
			if (!src)
				return;
			if (obs_property_button_clicked(p, src)) {
				refreshValues();
				refreshVisibility();
			}
			obs_source_release(src);
		});
		return b;
	}
	default:
		return nullptr;
	}
}

void PropView::loadValue(Row &row, obs_data_t *s)
{
	obs_property_t *p = row.prop;
	const char *name = row.name.c_str();
	switch (obs_property_get_type(p)) {
	case OBS_PROPERTY_BOOL:
		static_cast<QCheckBox *>(row.field)->setChecked(obs_data_get_bool(s, name));
		break;
	case OBS_PROPERTY_GROUP: {
		auto *box = static_cast<QGroupBox *>(row.field);
		if (box->isCheckable())
			box->setChecked(obs_data_get_bool(s, name));
		break;
	}
	case OBS_PROPERTY_INT: {
		auto *spin = row.field->findChild<QSpinBox *>("spin");
		spin->setValue((int)obs_data_get_int(s, name));
		if (auto *sl = row.field->findChild<QSlider *>("slider"))
			sl->setValue(spin->value());
		break;
	}
	case OBS_PROPERTY_FLOAT: {
		auto *spin = row.field->findChild<QDoubleSpinBox *>("spin");
		double v = obs_data_get_double(s, name);
		spin->setValue(v);
		if (auto *sl = row.field->findChild<QSlider *>("slider")) {
			double mn = spin->minimum(), mx = spin->maximum();
			sl->setValue((int)std::lround((v - mn) / (mx - mn) * SLIDER_STEPS));
		}
		break;
	}
	case OBS_PROPERTY_TEXT:
		if (auto *e = qobject_cast<QLineEdit *>(row.field))
			e->setText(QString::fromUtf8(obs_data_get_string(s, name)));
		break;
	case OBS_PROPERTY_PATH:
		row.field->findChild<QLineEdit *>("path")->setText(QString::fromUtf8(obs_data_get_string(s, name)));
		break;
	case OBS_PROPERTY_LIST: {
		auto *c = static_cast<QComboBox *>(row.field);
		obs_combo_format fmt = obs_property_list_format(p);
		QVariant v;
		if (fmt == OBS_COMBO_FORMAT_INT)
			v = QVariant::fromValue<qlonglong>(obs_data_get_int(s, name));
		else if (fmt == OBS_COMBO_FORMAT_FLOAT)
			v = obs_data_get_double(s, name);
		else
			v = QString::fromUtf8(obs_data_get_string(s, name));
		int idx = c->findData(v);
		if (idx < 0 && fmt == OBS_COMBO_FORMAT_FLOAT) {
			for (int i = 0; i < c->count(); i++)
				if (std::fabs(c->itemData(i).toDouble() - v.toDouble()) < 1e-4)
					idx = i;
		}
		c->setCurrentIndex(idx);
		break;
	}
	case OBS_PROPERTY_COLOR:
	case OBS_PROPERTY_COLOR_ALPHA:
		paint_swatch(static_cast<QPushButton *>(row.field), color_from_obs(obs_data_get_int(s, name)));
		break;
	default:
		break;
	}
}

void PropView::refreshValues()
{
	obs_source_t *src = getSource();
	if (!src)
		return;
	obs_data_t *s = obs_source_get_settings(src);
	/* re-run "modified" callbacks so visibility follows edits made elsewhere */
	if (props)
		obs_properties_apply_settings(props, s);
	updating = true;
	for (Row &row : rows) {
		QSignalBlocker block(row.field);
		loadValue(row, s);
	}
	updating = false;
	obs_data_release(s);
	obs_source_release(src);
	refreshVisibility();
}

void PropView::refreshVisibility()
{
	for (Row &row : rows) {
		bool vis = obs_property_visible(row.prop);
		row.layout->setRowVisible(row.field, vis);
		row.field->setEnabled(obs_property_enabled(row.prop));
	}
}

void PropView::changed(obs_property_t *p, obs_data_t *settings)
{
	bool refresh = obs_property_modified(p, settings);
	obs_source_t *src = getSource();
	if (src) {
		obs_source_update(src, nullptr);
		obs_source_release(src);
	}
	if (refresh)
		refreshVisibility();
}
