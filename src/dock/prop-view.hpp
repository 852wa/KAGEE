#pragma once

#include <obs-module.h>
#include <QWidget>
#include <QPointer>
#include <set>
#include <string>
#include <vector>

class QFormLayout;
class QLabel;

/*
 * Builds Qt widgets from a source's obs_properties and keeps them two-way synced with the
 * source settings. Only the properties named in `include` are shown (a group is shown fully
 * when its own name is included, or partially when some descendant is included); an empty
 * set shows everything.
 */
class PropView : public QWidget {
	Q_OBJECT

public:
	PropView(obs_source_t *source, std::set<std::string> include = {}, QWidget *parent = nullptr);
	~PropView() override;

	obs_source_t *getSource() const; /* new strong ref or nullptr */

public slots:
	void refreshValues();

private:
	struct Row {
		obs_property_t *prop;
		QFormLayout *layout;
		QWidget *field;
		QLabel *label;
		std::string name;
	};

	obs_weak_source_t *weak = nullptr;
	obs_properties_t *props = nullptr;
	std::set<std::string> include;
	std::vector<Row> rows;
	bool updating = false;

	bool wanted(obs_property_t *p) const;
	bool hasWantedDescendant(obs_properties_t *ps) const;
	void build(obs_properties_t *ps, QFormLayout *layout, bool all);
	QWidget *makeField(obs_property_t *p, QWidget *parent);
	void loadValue(Row &row, obs_data_t *settings);
	void refreshVisibility();
	void changed(obs_property_t *p, obs_data_t *settings);

	static void onUpdate(void *data, calldata_t *cd);
};
