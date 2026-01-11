#pragma once

#include <QMainWindow>
#include <QString>
#include <functional>

#include <QDBusObjectPath>
#include <QVariantMap>
#include <QStringList>

class QListWidget;
class QComboBox;
class QLineEdit;
class QCheckBox;
class QPushButton;
class QTextEdit;
class QTimer;
class QProgressDialog;

class UDisks2;

class MainWindow final : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QWidget* parent = nullptr);

private Q_SLOTS:
  void refreshDevices();        // manual (button)
  void refreshDevicesSilent();  // auto (timer / udisks signals)
  void onUDisksInterfacesAdded(const QDBusObjectPath&, const QVariantMap&);
  void onUDisksInterfacesRemoved(const QDBusObjectPath&, const QStringList&);
  void onSelectionChanged();
  void onConfirmChanged(const QString&);
  void doFormat();
  void doWipeQuick();
  void doWipeFull();

private:
  struct OpResult { bool ok = false; QString error; };

  void setBusy(bool busy, const QString& statusLine = {});
  void runOp(const QString& startLine, const QString& okLine, const QString& failPrefix, std::function<OpResult()> fn);

  void appendLog(const QString& line);
  void updateActionEnablement();
  void refreshDevicesImpl(bool verbose);
  QString selectedBlockObject() const;
  QString selectedDeviceNode() const;
  bool selectedReadOnly() const;

  UDisks2* udisks_;

  QListWidget* list_;
  QComboBox* fsCombo_;
  QLineEdit* labelEdit_;
  QCheckBox* tearDownCheck_;
  QLineEdit* confirmEdit_;

  QPushButton* refreshBtn_;
  QPushButton* formatBtn_;
  QPushButton* wipeQuickBtn_;
  QPushButton* wipeFullBtn_;

  QTextEdit* log_;

  bool busy_ = false;
  QProgressDialog* progress_ = nullptr;

  QTimer* pollTimer_ = nullptr;
  QTimer* debounceTimer_ = nullptr;
  QStringList lastDeviceNodes_;
};
