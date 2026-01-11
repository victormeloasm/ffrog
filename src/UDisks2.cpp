#include "UDisks2.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusObjectPath>
#include <QVariantMap>
#include <QByteArray>
#include <QRegularExpression>
#include <limits>

static constexpr const char* kService = "org.freedesktop.UDisks2";
static constexpr const char* kManagerPath = "/org/freedesktop/UDisks2/Manager";
static constexpr const char* kManagerIface = "org.freedesktop.UDisks2.Manager";
static constexpr const char* kPropsIface = "org.freedesktop.DBus.Properties";

UDisks2::UDisks2(QObject* parent) : QObject(parent) {}

QVariant UDisks2::getProp(const QString& objPath, const QString& iface, const QString& prop, bool* ok) const {
  QDBusInterface props(kService, objPath, kPropsIface, QDBusConnection::systemBus());
  if (!props.isValid()) {
    if (ok) *ok = false;
    return {};
  }
  QDBusReply<QVariant> reply = props.call("Get", iface, prop);
  if (!reply.isValid()) {
    if (ok) *ok = false;
    return {};
  }
  if (ok) *ok = true;
  return reply.value();
}

QString UDisks2::bytesToString(const QVariant& v) {
  // UDisks properties like Device/PreferredDevice are of type 'ay' (byte array).
  const QByteArray ba = v.toByteArray();
  if (ba.isEmpty()) return {};
  QByteArray tmp = ba;
  // Remove trailing NUL if present.
  if (!tmp.isEmpty() && tmp.back() == '\0') tmp.chop(1);
  return QString::fromLocal8Bit(tmp.constData());
}

QVector<UDisks2::UsbDevice> UDisks2::listUsbRemovable(QString* error) const {
  QVector<UsbDevice> out;

  // Debug counters (useful when UDisks2 is reachable but our filters yield 0).
  int blocks = 0;
  int partitions = 0;
  int noDrive = 0;
  int nonUsb = 0;
  int hinted = 0;
  int noDev = 0;
  int notWholeDisk = 0;

  QDBusInterface mgr(kService, kManagerPath, kManagerIface, QDBusConnection::systemBus());
  if (!mgr.isValid()) {
    if (error) *error = "Can't talk to udisksd on the system D-Bus. Is the udisks2 service running?";
    return out;
  }

  // Get all Block objects.
  QDBusReply<QList<QDBusObjectPath>> blocksReply = mgr.call("GetBlockDevices", QVariantMap{});
  if (!blocksReply.isValid()) {
    if (error) *error = "GetBlockDevices failed: " + blocksReply.error().message();
    return out;
  }

  for (const QDBusObjectPath& bop : blocksReply.value()) {
    ++blocks;
    const QString blockPath = bop.path();

    // IMPORTANT: Do NOT use QDBusInterface::isValid() to test if an interface exists.
    // Many object paths are valid while a specific interface may not be present.
    // Instead, try reading a property from that interface.
    {
      bool okPart = false;
      (void)getProp(blockPath, "org.freedesktop.UDisks2.Partition", "Number", &okPart);
      if (okPart) {
        ++partitions;
        continue; // it's a partition like /dev/sdX1
      }
    }

    bool okDrive = false;
    const QVariant driveVar = getProp(blockPath, "org.freedesktop.UDisks2.Block", "Drive", &okDrive);
    if (!okDrive) {
      ++noDrive;
      continue;
    }
    const QDBusObjectPath driveObj = qvariant_cast<QDBusObjectPath>(driveVar);
    const QString drivePath = driveObj.path();
    if (drivePath.isEmpty() || drivePath == "/") {
      ++noDrive;
      continue;
    }

    // Filter to USB devices.
    // NOTE: Some USB pendrives report Removable/MediaRemovable = false in practice, so we rely
    // primarily on ConnectionBus containing "usb".
    const QString conn = getProp(drivePath, "org.freedesktop.UDisks2.Drive", "ConnectionBus").toString();
    if (!conn.contains("usb", Qt::CaseInsensitive)) {
      ++nonUsb;
      continue;
    }

    // Skip devices udisks marks as system/ignore (extra safety).
    const bool hintSystem = getProp(blockPath, "org.freedesktop.UDisks2.Block", "HintSystem").toBool();
    const bool hintIgnore = getProp(blockPath, "org.freedesktop.UDisks2.Block", "HintIgnore").toBool();
    if (hintSystem || hintIgnore) {
      ++hinted;
      continue;
    }

    UsbDevice dev;
    dev.blockObject = blockPath;
    dev.driveObject = drivePath;
    dev.deviceNode = bytesToString(getProp(blockPath, "org.freedesktop.UDisks2.Block", "PreferredDevice"));
    if (dev.deviceNode.isEmpty()) dev.deviceNode = bytesToString(getProp(blockPath, "org.freedesktop.UDisks2.Block", "Device"));
    if (dev.deviceNode.isEmpty()) {
      // Fallback: derive from the block object basename, e.g. .../block_devices/sdb -> /dev/sdb
      const QString base = blockPath.section('/', -1);
      if (!base.isEmpty()) dev.deviceNode = "/dev/" + base;
    }

    // Safety: only accept whole-disk nodes. We intentionally avoid partitions like /dev/sdb1.
    // This is the "format the whole stick" use-case.
    // Accept sdX (letters only) and nvmeXnY (whole namespace).
    const QString bn = dev.deviceNode.section('/', -1);
    const bool isWholeDisk = bn.contains(QRegularExpression("^sd[a-z]+$")) ||
                             bn.contains(QRegularExpression("^nvme\\d+n\\d+$")) ||
                             bn.contains(QRegularExpression("^mmcblk\\d+$"));
    if (!isWholeDisk) {
      ++notWholeDisk;
      continue;
    }
    dev.sizeBytes = getProp(blockPath, "org.freedesktop.UDisks2.Block", "Size").toULongLong();
    dev.readOnly = getProp(blockPath, "org.freedesktop.UDisks2.Block", "ReadOnly").toBool();
    dev.vendor = getProp(drivePath, "org.freedesktop.UDisks2.Drive", "Vendor").toString();
    dev.model = getProp(drivePath, "org.freedesktop.UDisks2.Drive", "Model").toString();
    dev.serial = getProp(drivePath, "org.freedesktop.UDisks2.Drive", "Serial").toString();

    // Extra safety: only show /dev/* nodes (ignore weird backends)
    if (dev.deviceNode.startsWith("/dev/")) {
      out.push_back(dev);
    } else {
      ++noDev;
    }
  }

  if (out.isEmpty() && error) {
    *error = QString("UDisks2 reachable, but filter returned 0 USB whole-disk devices. "
                     "blocks=%1 partitions=%2 noDrive=%3 nonUsb=%4 hinted=%5 notWholeDisk=%6 noDev=%7")
                 .arg(blocks)
                 .arg(partitions)
                 .arg(noDrive)
                 .arg(nonUsb)
                 .arg(hinted)
                 .arg(notWholeDisk)
                 .arg(noDev);
  }
  return out;
}

bool UDisks2::unmountIfMounted(const QString& blockObject, QString* error) const {
  // Do NOT use QDBusInterface::isValid() to test for interface presence.
  // Instead, try to read a property from that interface.
  bool okFs = false;
  (void)getProp(blockObject, "org.freedesktop.UDisks2.Filesystem", "MountPoints", &okFs);
  if (!okFs) {
    // Not a filesystem (or interface not present). That's fine.
    return true;
  }

  QDBusInterface fs(kService, blockObject, "org.freedesktop.UDisks2.Filesystem", QDBusConnection::systemBus());
  QDBusReply<void> reply = fs.call("Unmount", QVariantMap{});
  if (!reply.isValid()) {
    // If already unmounted, udisks may complain; treat common cases as non-fatal.
    const QString msg = reply.error().message();
    if (msg.contains("Not mounted", Qt::CaseInsensitive) || msg.contains("not mounted", Qt::CaseInsensitive)) {
      return true;
    }
    if (error) *error = "Unmount failed: " + msg;
    return false;
  }
  return true;
}

bool UDisks2::unmountAllOnSameDrive(const QString& blockObject, QString* error) const {
  // Always try unmount on the block itself first (covers "superfloppy" USB sticks).
  if (!unmountIfMounted(blockObject, error)) return false;

  bool okDrive = false;
  const QVariant driveVar = getProp(blockObject, "org.freedesktop.UDisks2.Block", "Drive", &okDrive);
  if (!okDrive) return true;
  const QString drivePath = qvariant_cast<QDBusObjectPath>(driveVar).path();
  if (drivePath.isEmpty() || drivePath == "/") return true;

  QDBusInterface mgr(kService, kManagerPath, kManagerIface, QDBusConnection::systemBus());
  if (!mgr.isValid()) return true; // already checked in other calls; best-effort.
  QDBusReply<QList<QDBusObjectPath>> blocksReply = mgr.call("GetBlockDevices", QVariantMap{});
  if (!blocksReply.isValid()) return true;

  for (const QDBusObjectPath& bop : blocksReply.value()) {
    const QString p = bop.path();
    bool okDrv2 = false;
    const QVariant drv2 = getProp(p, "org.freedesktop.UDisks2.Block", "Drive", &okDrv2);
    if (!okDrv2) continue;
    const QString drv2Path = qvariant_cast<QDBusObjectPath>(drv2).path();
    if (drv2Path != drivePath) continue;

    // Unmount any filesystem present on that block (partitions, etc.).
    if (!unmountIfMounted(p, error)) return false;
  }

  return true;
}

QString UDisks2::pickPrimaryPartitionBlock(const QString& blockObject) const {
  bool okDrive = false;
  const QVariant driveVar = getProp(blockObject, "org.freedesktop.UDisks2.Block", "Drive", &okDrive);
  if (!okDrive) return {};
  const QString drivePath = qvariant_cast<QDBusObjectPath>(driveVar).path();
  if (drivePath.isEmpty() || drivePath == "/") return {};

  QDBusInterface mgr(kService, kManagerPath, kManagerIface, QDBusConnection::systemBus());
  if (!mgr.isValid()) return {};
  QDBusReply<QList<QDBusObjectPath>> blocksReply = mgr.call("GetBlockDevices", QVariantMap{});
  if (!blocksReply.isValid()) return {};

  int bestNum = std::numeric_limits<int>::max();
  QString bestPath;

  for (const QDBusObjectPath& bop : blocksReply.value()) {
    const QString p = bop.path();

    bool okDrv2 = false;
    const QVariant drv2 = getProp(p, "org.freedesktop.UDisks2.Block", "Drive", &okDrv2);
    if (!okDrv2) continue;
    const QString drv2Path = qvariant_cast<QDBusObjectPath>(drv2).path();
    if (drv2Path != drivePath) continue;

    bool okPart = false;
    const QVariant numVar = getProp(p, "org.freedesktop.UDisks2.Partition", "Number", &okPart);
    if (!okPart) continue;
    const int num = numVar.toInt();
    if (num <= 0) continue;
    if (num < bestNum) {
      bestNum = num;
      bestPath = p;
    }
  }

  return bestPath;
}

bool UDisks2::formatBlock(const QString& blockObject,
                          const QString& fsType,
                          const QString& label,
                          const QString& eraseMode,
                          bool tearDown,
                          QString* error) const {
  // If the disk has partitions (common), format the primary partition instead of the whole disk.
  // This behaves more like "normal" desktop format tools.
  const QString primaryPart = pickPrimaryPartitionBlock(blockObject);
  const QString fmtTarget = primaryPart.isEmpty() ? blockObject : primaryPart;

  if (!unmountAllOnSameDrive(blockObject, error)) return false;

  QDBusInterface blk(kService, fmtTarget, "org.freedesktop.UDisks2.Block", QDBusConnection::systemBus());
  if (!blk.isValid()) {
    if (error) *error = "org.freedesktop.UDisks2.Block interface not available for: " + fmtTarget;
    return false;
  }

  QVariantMap opts;
  if (!label.isEmpty()) opts.insert("label", label);
  if (!eraseMode.isEmpty()) opts.insert("erase", eraseMode);
  opts.insert("take-ownership", true);
  opts.insert("update-partition-type", true);
  if (tearDown) opts.insert("tear-down", true);

  QDBusReply<void> reply = blk.call("Format", fsType, opts);
  if (!reply.isValid()) {
    if (error) *error = "Format failed: " + reply.error().message();
    return false;
  }

  // Optional rescan
  blk.call("Rescan", QVariantMap{});
  return true;
}

bool UDisks2::wipeBlock(const QString& blockObject, const QString& eraseMode, bool tearDown, QString* error) const {
  if (!unmountAllOnSameDrive(blockObject, error)) return false;

  QDBusInterface blk(kService, blockObject, "org.freedesktop.UDisks2.Block", QDBusConnection::systemBus());
  if (!blk.isValid()) {
    if (error) *error = "org.freedesktop.UDisks2.Block interface not available for: " + blockObject;
    return false;
  }

  QVariantMap opts;
  if (!eraseMode.isEmpty()) opts.insert("erase", eraseMode);
  if (tearDown) opts.insert("tear-down", true);

  QDBusReply<void> reply = blk.call("Format", QStringLiteral("empty"), opts);
  if (!reply.isValid()) {
    if (error) *error = "Wipe (empty) failed: " + reply.error().message();
    return false;
  }

  blk.call("Rescan", QVariantMap{});
  return true;
}
