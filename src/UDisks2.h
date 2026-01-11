#pragma once

#include <QObject>
#include <QString>
#include <QVector>

class UDisks2 final : public QObject {
  Q_OBJECT
public:
  struct UsbDevice {
    QString blockObject;   // D-Bus object path for org.freedesktop.UDisks2.Block
    QString driveObject;   // D-Bus object path for org.freedesktop.UDisks2.Drive
    QString deviceNode;    // e.g. /dev/sdb
    QString vendor;
    QString model;
    QString serial;
    quint64 sizeBytes = 0;
    bool readOnly = false;
  };

  explicit UDisks2(QObject* parent = nullptr);

  // Lists *top-level* USB removable devices (pendrives/SD readers) only.
  // This intentionally filters out internal disks.
  QVector<UsbDevice> listUsbRemovable(QString* error = nullptr) const;

  // Best-effort unmount for any mounted filesystem on the block.
  bool unmountIfMounted(const QString& blockObject, QString* error = nullptr) const;

  // Best-effort: unmount every mounted filesystem that belongs to the same Drive as `blockObject`.
  // This is required when the UI targets a whole-disk node (/dev/sdb) but the actual filesystem
  // lives on a partition (/dev/sdb1).
  bool unmountAllOnSameDrive(const QString& blockObject, QString* error = nullptr) const;

  // If `blockObject` is a whole-disk, try to pick a primary partition on that disk.
  // Returns an empty string if none is found.
  QString pickPrimaryPartitionBlock(const QString& blockObject) const;

  // Formats the selected block with a filesystem (vfat/exfat/ext4/ntfs/etc).
  // eraseMode: "" (none) or "zero" (full zero-fill). Other UDisks modes exist.
  bool formatBlock(const QString& blockObject,
                   const QString& fsType,
                   const QString& label,
                   const QString& eraseMode,
                   bool tearDown,
                   QString* error = nullptr) const;

  // "empty" format – quick wipe of filesystem signatures; with eraseMode="zero" => full wipe.
  bool wipeBlock(const QString& blockObject,
                 const QString& eraseMode,
                 bool tearDown,
                 QString* error = nullptr) const;

private:
  QVariant getProp(const QString& objPath, const QString& iface, const QString& prop, bool* ok = nullptr) const;
  static QString bytesToString(const QVariant& v);
};
