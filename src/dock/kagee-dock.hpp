#pragma once

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QWidget>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTimer;

class KageeDock : public QWidget {
	Q_OBJECT

public:
	explicit KageeDock(QWidget *parent = nullptr);
	~KageeDock() override;

	void frontendEvent(enum obs_frontend_event event);

private slots:
	void refreshTargets();
	void onTargetSelected(int index);
	void followScene();
	void pollState();
	void rebuildPages();

private:
	enum Tab { T_SHOTS = 0, T_CAMERA, T_LAYERS, T_MOTION, T_STAGE, T_LIGHT, T_GRADE, T_FX, T_COUNT };

	QComboBox *targetCombo = nullptr;
	QCheckBox *followBox = nullptr;
	QStackedWidget *stack = nullptr;
	QStackedWidget *modeStack = nullptr;
	QScrollArea *easyPage = nullptr;
	QPushButton *modeButtons[2] = {};
	int mode = 0; /* 0 easy, 1 detailed */
	std::vector<QPushButton *> easyShotButtons; /* index 0 = live */
	std::vector<QComboBox *> easyPresetCombos;  /* index 0 = slot 1 */
	QPushButton *navButtons[8] = {};
	QScrollArea *pages[T_COUNT] = {};
	QTimer *pollTimer = nullptr;
	QTimer *rebuildTimer = nullptr;
	QTimer *targetsTimer = nullptr;

	obs_weak_source_t *target = nullptr;
	bool targetIsStage = false;
	int currentLayer = 1;

	std::vector<QPushButton *> shotButtons; /* index 0 = live */
	std::vector<QPushButton *> saveButtons;
	QPushButton *autoButton = nullptr;
	QPushButton *tapButton = nullptr;

	obs_source_t *getTarget() const;
	void setTarget(obs_source_t *source);
	void connectTarget(obs_source_t *source, bool connect);
	void setPage(Tab tab, QWidget *content);
	QWidget *buildShotsPage(obs_source_t *src);
	QWidget *buildLayersPage(obs_source_t *src);
	QWidget *buildFilterPage(obs_source_t *src, const char *filter_id);
	QWidget *buildEffectsPage(obs_source_t *src);
	QWidget *messagePage(const QString &text);
	QWidget *setupBanner(obs_source_t *src, bool always);
	void importScene();
	void callAction(const char *proc, const char *name, long long index = 0);
	void snapshot(const QString &dir);
	void showTab(int index);
	void setMode(int m);
	QWidget *buildEasyPage(obs_source_t *src);
	QWidget *buildLookSection(obs_source_t *src);
	void selfTest();

	static void onTargetSignal(void *data, calldata_t *cd);
	static void onTargetRemoved(void *data, calldata_t *cd);
	static void onGlobalSourceChange(void *data, calldata_t *cd);
};
