#include "MainWindow.h"
#include "UDisks2.h"

#include <QListWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QDateTime>
#include <QTimer>
#include <QProgressDialog>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#include <QDBusConnection>

static QString humanBytes(quint64 bytes) {
  const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double b = static_cast<double>(bytes);
  int u = 0;
  while (b >= 1024.0 && u < 4) { b /= 1024.0; ++u; }
  return QString::number(b, 'f', (u == 0 ? 0 : 2)) + " " + units[u];
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), udisks_(new UDisks2(this)) {
  setWindowTitle("ffrog v1.7 - The Frogmat utility");
  resize(900, 600);

  auto* central = new QWidget(this);
  auto* root = new QVBoxLayout(central);

  auto* topRow = new QHBoxLayout();
  refreshBtn_ = new QPushButton("Refresh", this);
  topRow->addWidget(refreshBtn_);
  topRow->addStretch(1);
  root->addLayout(topRow);

  list_ = new QListWidget(this);
  list_->setSelectionMode(QAbstractItemView::SingleSelection);
  root->addWidget(new QLabel("USB removable devices (whole-disk only, e.g. /dev/sdX):", this));
  root->addWidget(list_, 1);

  auto* cfgRow = new QHBoxLayout();
  cfgRow->addWidget(new QLabel("Format:", this));
  fsCombo_ = new QComboBox(this);
  fsCombo_->addItem("FAT32 (vfat)", "vfat");
  fsCombo_->addItem("exFAT (exfat)", "exfat");
  fsCombo_->addItem("NTFS (ntfs)", "ntfs");
  fsCombo_->addItem("ext4 (ext4)", "ext4");
  cfgRow->addWidget(fsCombo_);

  cfgRow->addSpacing(12);
  cfgRow->addWidget(new QLabel("Label:", this));
  labelEdit_ = new QLineEdit(this);
  labelEdit_->setPlaceholderText("Optional (e.g. MY_USB)");
  cfgRow->addWidget(labelEdit_, 1);

  tearDownCheck_ = new QCheckBox("tear-down (cleanup stacks/mounts)", this);
  tearDownCheck_->setChecked(true);
  cfgRow->addWidget(tearDownCheck_);

  root->addLayout(cfgRow);

  auto* confirmRow = new QHBoxLayout();
  confirmRow->addWidget(new QLabel("Confirmation: type the exact device (e.g. /dev/sdb):", this));
  confirmEdit_ = new QLineEdit(this);
  confirmEdit_->setPlaceholderText("/dev/sdX");
  confirmRow->addWidget(confirmEdit_, 1);
  root->addLayout(confirmRow);

  auto* btnRow = new QHBoxLayout();
  formatBtn_ = new QPushButton("Format", this);
  wipeQuickBtn_ = new QPushButton("Wipe quick (signatures)", this);
  wipeFullBtn_ = new QPushButton("Wipe full (zero-fill)", this);
  btnRow->addWidget(formatBtn_);
  btnRow->addWidget(wipeQuickBtn_);
  btnRow->addWidget(wipeFullBtn_);
  btnRow->addStretch(1);
  root->addLayout(btnRow);

  log_ = new QTextEdit(this);
  log_->setReadOnly(true);
  root->addWidget(new QLabel("Log:", this));
  root->addWidget(log_, 1);

  setCentralWidget(central);

  connect(refreshBtn_, &QPushButton::clicked, this, &MainWindow::refreshDevices);
  connect(list_, &QListWidget::itemSelectionChanged, this, &MainWindow::onSelectionChanged);
  connect(confirmEdit_, &QLineEdit::textChanged, this, &MainWindow::onConfirmChanged);
  connect(formatBtn_, &QPushButton::clicked, this, &MainWindow::doFormat);
  connect(wipeQuickBtn_, &QPushButton::clicked, this, &MainWindow::doWipeQuick);
  connect(wipeFullBtn_, &QPushButton::clicked, this, &MainWindow::doWipeFull);

  // Auto refresh (fallback): poll periodically without spamming the log.
  pollTimer_ = new QTimer(this);
  pollTimer_->setInterval(1500);
  connect(pollTimer_, &QTimer::timeout, this, &MainWindow::refreshDevicesSilent);
  pollTimer_->start();

  // Debounce timer for udisks2 object-manager signals.
  debounceTimer_ = new QTimer(this);
  debounceTimer_->setSingleShot(true);
  debounceTimer_->setInterval(250);
  connect(debounceTimer_, &QTimer::timeout, this, &MainWindow::refreshDevicesSilent);

  // Watch hotplug/unplug via UDisks2 ObjectManager signals (best UX).
  // If this connection fails, the periodic poll above still keeps the UI updated.
  QDBusConnection::systemBus().connect(
      "org.freedesktop.UDisks2",
      "/org/freedesktop/UDisks2",
      "org.freedesktop.DBus.ObjectManager",
      "InterfacesAdded",
      this,
      SLOT(onUDisksInterfacesAdded(QDBusObjectPath,QVariantMap)));

  QDBusConnection::systemBus().connect(
      "org.freedesktop.UDisks2",
      "/org/freedesktop/UDisks2",
      "org.freedesktop.DBus.ObjectManager",
      "InterfacesRemoved",
      this,
      SLOT(onUDisksInterfacesRemoved(QDBusObjectPath,QStringList)));

  refreshDevices();
}


void MainWindow::appendLog(const QString& line) {
  const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
  log_->append("[" + ts + "] " + line);
}

void MainWindow::setBusy(bool busy, const QString& statusLine) {
  busy_ = busy;

  // Stop background refresh while doing destructive operations.
  if (pollTimer_) {
    if (busy_) pollTimer_->stop();
    else pollTimer_->start();
  }
  if (debounceTimer_ && busy_) debounceTimer_->stop();

  refreshBtn_->setEnabled(!busy_);
  list_->setEnabled(!busy_);
  fsCombo_->setEnabled(!busy_);
  labelEdit_->setEnabled(!busy_);
  tearDownCheck_->setEnabled(!busy_);
  confirmEdit_->setEnabled(!busy_);

  // Buttons: disable all while busy; re-evaluate afterwards.
  formatBtn_->setEnabled(false);
  wipeQuickBtn_->setEnabled(false);
  wipeFullBtn_->setEnabled(false);

  if (busy_) {
    if (!progress_) {
      progress_ = new QProgressDialog(this);
      progress_->setWindowTitle("Working...");
      progress_->setRange(0, 0); // indeterminate
      progress_->setCancelButton(nullptr);
      progress_->setWindowModality(Qt::ApplicationModal);
      progress_->setMinimumDuration(0);
    }
    progress_->setLabelText(statusLine.isEmpty() ? QStringLiteral("Working...") : statusLine);
    progress_->show();
  } else {
    if (progress_) {
      progress_->hide();
      progress_->deleteLater();
      progress_ = nullptr;
    }
    updateActionEnablement();
  }
}

void MainWindow::runOp(const QString& startLine,
                       const QString& okLine,
                       const QString& failPrefix,
                       std::function<OpResult()> fn) {
  if (busy_) return;

  appendLog(startLine);
  setBusy(true, startLine);

  auto* watcher = new QFutureWatcher<OpResult>(this);
  connect(watcher, &QFutureWatcher<OpResult>::finished, this, [this, watcher, okLine, failPrefix]() {
    const OpResult r = watcher->result();
    watcher->deleteLater();

    setBusy(false);

    if (r.ok) {
      appendLog(okLine);
      QMessageBox::information(this, "OK", okLine);
    } else {
      appendLog(failPrefix + r.error);
      QMessageBox::critical(this, "Failed", r.error);
    }

    refreshDevices();
  });

  watcher->setFuture(QtConcurrent::run([fn = std::move(fn)]() mutable { return fn(); }));
}

QString MainWindow::selectedBlockObject() const {
  auto* item = list_->currentItem();
  if (!item) return {};
  return item->data(Qt::UserRole).toString();
}

QString MainWindow::selectedDeviceNode() const {
  auto* item = list_->currentItem();
  if (!item) return {};
  return item->data(Qt::UserRole + 1).toString();
}

bool MainWindow::selectedReadOnly() const {
  auto* item = list_->currentItem();
  if (!item) return false;
  return item->data(Qt::UserRole + 2).toBool();
}

void MainWindow::updateActionEnablement() {
  const QString dev = selectedDeviceNode();
  const bool hasSel = !dev.isEmpty();
  const bool confirmOk = hasSel && (confirmEdit_->text().trimmed() == dev);
  const bool ro = selectedReadOnly();

  formatBtn_->setEnabled(hasSel && confirmOk && !ro);
  wipeQuickBtn_->setEnabled(hasSel && confirmOk && !ro);
  wipeFullBtn_->setEnabled(hasSel && confirmOk && !ro);

  if (ro) {
    formatBtn_->setToolTip("Device is read-only");
    wipeQuickBtn_->setToolTip("Device is read-only");
    wipeFullBtn_->setToolTip("Device is read-only");
  } else {
    formatBtn_->setToolTip({});
    wipeQuickBtn_->setToolTip({});
    wipeFullBtn_->setToolTip({});
  }
}

void MainWindow::onSelectionChanged() {
  const QString dev = selectedDeviceNode();
  confirmEdit_->setText(dev);
  updateActionEnablement();
}

void MainWindow::onConfirmChanged(const QString&) {
  updateActionEnablement();
}

void MainWindow::onUDisksInterfacesAdded(const QDBusObjectPath&, const QVariantMap&) {
  if (debounceTimer_) debounceTimer_->start();
}

void MainWindow::onUDisksInterfacesRemoved(const QDBusObjectPath&, const QStringList&) {
  if (debounceTimer_) debounceTimer_->start();
}

void MainWindow::refreshDevices() {
  refreshDevicesImpl(true);
}

void MainWindow::refreshDevicesSilent() {
  refreshDevicesImpl(false);
}

void MainWindow::refreshDevicesImpl(bool verbose) {
  const QString prevDev = selectedDeviceNode();

  list_->clear();
  QString err;
  const auto devices = udisks_->listUsbRemovable(&err);

  // NOTE: listUsbRemovable() may provide a diagnostic string even when the service is reachable
  // but no matching USB whole-disk devices are currently connected. That's not an error.
  const bool noUsbInfo = err.startsWith("UDisks2 reachable, but filter returned 0 USB whole-disk devices");
  if (!err.isEmpty()) {
    if (verbose) {
      appendLog(QString(noUsbInfo ? "INFO: " : "ERROR: ") + err);
    } else {
      // Silent refresh: never spam the log.
      // - Ignore the "no USB devices" informational message.
      // - If we have a real error, only log when it changes.
      if (noUsbInfo) {
        if (!lastAutoError_.isEmpty()) lastAutoError_.clear();
      } else {
        if (err != lastAutoError_) {
          appendLog("ERROR: " + err);
          lastAutoError_ = err;
        }
      }
    }
  } else {
    if (!lastAutoError_.isEmpty()) lastAutoError_.clear();
  }

  QStringList curDevs;
  for (const auto& d : devices) {
    curDevs.push_back(d.deviceNode);

    const QString title = QString("%1 %2 (%3)  [%4]")
                            .arg(d.vendor.trimmed())
                            .arg(d.model.trimmed())
                            .arg(humanBytes(d.sizeBytes))
                            .arg(d.deviceNode);

    auto* item = new QListWidgetItem(title, list_);
    item->setData(Qt::UserRole, d.blockObject);
    item->setData(Qt::UserRole + 1, d.deviceNode);
    item->setData(Qt::UserRole + 2, d.readOnly);
    item->setToolTip("Block: " + d.blockObject + "\nDrive: " + d.driveObject +
                     (d.serial.isEmpty() ? "" : ("\nSerial: " + d.serial)));
    if (d.readOnly) item->setText(title + "  [READONLY]");
  }

  // Try to keep the previously selected device selected.
  if (!prevDev.isEmpty()) {
    for (int i = 0; i < list_->count(); ++i) {
      auto* it = list_->item(i);
      if (it && it->data(Qt::UserRole + 1).toString() == prevDev) {
        list_->setCurrentItem(it);
        break;
      }
    }
  }

  if (verbose) {
    appendLog(QString("Found %1 USB device(s).").arg(devices.size()));
  } else {
    if (curDevs != lastDeviceNodes_) {
      appendLog(QString("Auto: now %1 USB device(s) detected.").arg(devices.size()));
      lastDeviceNodes_ = curDevs;
    }
  }

  updateActionEnablement();
}

void MainWindow::doFormat() {
  const QString block = selectedBlockObject();
  const QString dev = selectedDeviceNode();
  if (block.isEmpty() || dev.isEmpty()) return;

  const QString fsType = fsCombo_->currentData().toString();
  const QString label = labelEdit_->text().trimmed();
  const bool tearDown = tearDownCheck_->isChecked();

  const auto choice = QMessageBox::warning(
      this,
      "Confirm format",
      QString("You are about to FORMAT %1 as '%2'.\n\nThis will ERASE EVERYTHING on this device.")
          .arg(dev)
          .arg(fsType),
      QMessageBox::Cancel | QMessageBox::Ok,
      QMessageBox::Cancel);

  if (choice != QMessageBox::Ok) return;

  runOp(
      QString("Formatting %1 (%2)...").arg(dev).arg(fsType),
      QStringLiteral("OK: format complete."),
      QStringLiteral("ERROR: "),
      [block, fsType, label, tearDown]() -> OpResult {
        UDisks2 u;
        QString err;
        const bool ok = u.formatBlock(block, fsType, label, /*eraseMode*/ QString(), tearDown, &err);
        return {ok, err};
      });
}

void MainWindow::doWipeQuick() {
  const QString block = selectedBlockObject();
  const QString dev = selectedDeviceNode();
  if (block.isEmpty() || dev.isEmpty()) return;

  const bool tearDown = tearDownCheck_->isChecked();
  const auto choice = QMessageBox::warning(
      this,
      "Confirm quick wipe",
      QString("You are about to WIPE SIGNATURES (empty format) on %1.\n\nThis removes filesystem/partition signatures.")
          .arg(dev),
      QMessageBox::Cancel | QMessageBox::Ok,
      QMessageBox::Cancel);

  if (choice != QMessageBox::Ok) return;

  runOp(
      QString("Wiping signatures on %1...").arg(dev),
      QStringLiteral("OK: signatures wiped."),
      QStringLiteral("ERROR: "),
      [block, tearDown]() -> OpResult {
        UDisks2 u;
        QString err;
        const bool ok = u.wipeBlock(block, /*eraseMode*/ QString(), tearDown, &err);
        return {ok, err};
      });
}

void MainWindow::doWipeFull() {
  const QString block = selectedBlockObject();
  const QString dev = selectedDeviceNode();
  if (block.isEmpty() || dev.isEmpty()) return;

  const bool tearDown = tearDownCheck_->isChecked();
  const auto choice = QMessageBox::warning(
      this,
      "Confirm full wipe",
      QString("You are about to ZERO-FILL the entire device %1 (this may take a LONG time).\n\nThis writes zeros over everything before leaving it 'empty'.")
          .arg(dev),
      QMessageBox::Cancel | QMessageBox::Ok,
      QMessageBox::Cancel);

  if (choice != QMessageBox::Ok) return;

  runOp(
      QString("Zero-filling %1 (erase=zero)...").arg(dev),
      QStringLiteral("OK: full wipe complete."),
      QStringLiteral("ERROR: "),
      [block, tearDown]() -> OpResult {
        UDisks2 u;
        QString err;
        const bool ok = u.wipeBlock(block, /*eraseMode*/ QStringLiteral("zero"), tearDown, &err);
        return {ok, err};
      });
}
