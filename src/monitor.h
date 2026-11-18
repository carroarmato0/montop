#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <memory>
#include <optional>
#include <QTimer>
#include <QVariantList>

#include "gpu.h"
#include "processmodel.h"
#include "apps.h"
#include "procfs.h"

// Samples the kernel's counters on a timer and turns them into rates,
// percentages, and short histories for the window to draw. Everything comes
// from /proc and /sys; both roots are arguments so tests can hand it a
// directory of fixed files.
class Monitor : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString hostname READ hostname CONSTANT)
    Q_PROPERTY(QString kernel READ kernel CONSTANT)
    Q_PROPERTY(QString cpuModel READ cpuModel CONSTANT)
    Q_PROPERTY(int coreCount READ coreCount NOTIFY sampled)
    Q_PROPERTY(double cpuUsage READ cpuUsage NOTIFY sampled)
    Q_PROPERTY(QVariantList cores READ cores NOTIFY sampled)
    Q_PROPERTY(double cpuTemperature READ cpuTemperature NOTIFY sampled)
    Q_PROPERTY(double cpuFrequency READ cpuFrequency NOTIFY sampled)
    Q_PROPERTY(QList<qreal> loadAverage READ loadAverage NOTIFY sampled)
    Q_PROPERTY(qint64 uptime READ uptime NOTIFY sampled)
    Q_PROPERTY(double memoryTotal READ memoryTotal NOTIFY sampled)
    Q_PROPERTY(double memoryUsed READ memoryUsed NOTIFY sampled)
    Q_PROPERTY(double memoryAvailable READ memoryAvailable NOTIFY sampled)
    Q_PROPERTY(double memoryCached READ memoryCached NOTIFY sampled)
    Q_PROPERTY(double memoryFree READ memoryFree NOTIFY sampled)
    Q_PROPERTY(double swapTotal READ swapTotal NOTIFY sampled)
    Q_PROPERTY(double swapUsed READ swapUsed NOTIFY sampled)
    Q_PROPERTY(QVariantList disks READ disks NOTIFY sampled)
    Q_PROPERTY(QVariantList filesystems READ filesystems NOTIFY sampled)
    Q_PROPERTY(QVariantList networks READ networks NOTIFY sampled)
    Q_PROPERTY(int taskCount READ taskCount NOTIFY sampled)
    // [{name, driver, asleep, busy, memoryUsed, memoryTotal, appMemory, mhz,
    // temperature, watts, fan, note}], one per GPU; -1 where it's unknown.
    Q_PROPERTY(QVariantList gpus READ gpus NOTIFY sampled)
    Q_PROPERTY(bool gpuPerApp READ gpuPerApp NOTIFY sampled)
    Q_PROPERTY(QString error READ error NOTIFY sampled)
    Q_PROPERTY(ProcessModel *processes READ processes CONSTANT)
    Q_PROPERTY(int interval READ interval WRITE setInterval NOTIFY intervalChanged)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    // When the process list shows the past: the moment's time in ms since
    // the epoch, or 0 while it shows now.
    Q_PROPERTY(double rewoundTo READ rewoundTo NOTIFY rewindChanged)
    Q_PROPERTY(double historyStart READ historyStart NOTIFY sampled)

public:
    // How many samples each graph remembers.
    // Graphs scroll at a fixed speed and a wide one shows more of the past,
    // so keep enough for a full-screen graph: about four minutes at the
    // default four samples a second.
    static constexpr int HistoryLength = 1024;
    // How many process scans are kept to rewind to: ten minutes at one a
    // second.
    static constexpr int MomentCount = 600;

    explicit Monitor(const QString &procRoot = "/proc", const QString &sysRoot = "/sys", QObject *parent = nullptr);

    QString hostname() const { return m_hostname; }
    QString kernel() const { return m_kernel; }
    QString cpuModel() const { return m_cpuModel; }
    int coreCount() const { return m_cores.size(); }
    double cpuUsage() const { return m_cpuUsage; }
    QVariantList cores() const { return m_cores; }
    double cpuTemperature() const { return m_cpuTemperature; }
    double cpuFrequency() const { return m_cpuFrequency; }
    QList<qreal> loadAverage() const { return m_loadAverage; }
    qint64 uptime() const { return m_uptime; }
    double memoryTotal() const { return m_memTotal; }
    double memoryUsed() const { return m_memUsed; }
    double memoryAvailable() const { return m_memAvailable; }
    double memoryCached() const { return m_memCached; }
    double memoryFree() const { return m_memFree; }
    double swapTotal() const { return m_swapTotal; }
    double swapUsed() const { return m_swapUsed; }
    QVariantList disks() const { return m_disks; }
    QVariantList filesystems() const { return m_filesystems; }
    QVariantList networks() const { return m_networks; }
    int taskCount() const { return m_taskCount; }
    QVariantList gpus() const;
    bool gpuPerApp() const { return m_gpus->perApp(); }
    const QList<gpu::Device> &gpuDevices() const { return m_gpus->devices(); }
    // Tests stand in for NVIDIA's library.
    void setNvml(std::unique_ptr<gpu::Nvml> nvml) { m_gpus->setNvml(std::move(nvml)); }
    QString error() const { return m_error; }
    ProcessModel *processes() { return &m_processes; }
    int interval() const { return m_timer.interval(); }
    bool paused() const { return m_paused; }
    double rewoundTo() const { return m_rewoundTo; }
    double historyStart() const { return m_moments.isEmpty() ? 0 : double(m_moments.first().time); }

    // A series by name, for graphs to read straight from C++ rather than
    // through a copy into QML on every sample: "cpu", "memory",
    // "disk/<name>/read", "disk/<name>/write", "net/<name>/rx", "net/<name>/tx",
    // "gpu/<index>".
    QList<qreal> history(const QString &series) const { return m_history.value(series); }

    void setInterval(int ms);
    void setPaused(bool paused);

    Q_INVOKABLE void faster();
    Q_INVOKABLE void slower();

    // Sends SIGTERM, or SIGKILL when forced. Returns 0 or the errno, which
    // the window turns into a sentence.
    Q_INVOKABLE int signalProcess(int pid, bool force);
    // The same for every process of an app. Returns how many were sent and
    // how many failed, with the first failure's errno.
    Q_INVOKABLE QVariantMap signalProcesses(const QVariantList &pids, bool force);

    // Pausing (freezing) an app and capping its CPU, through systemd's user
    // manager so systemd stays in charge of its own units. Only the user's
    // apps and background services qualify: not the desktop's own services,
    // not the system's, and not the app Montop itself runs in. Returns an
    // empty string, or why it didn't happen in a sentence for the window.
    Q_INVOKABLE QString setAppPaused(const QString &groupKey, bool paused);
    // `percent` is of one core, as in the CPU column; 0 lifts the limit.
    Q_INVOKABLE QString setAppCpuLimit(const QString &groupKey, double percent);
    // Thaws and unlimits what this monitor paused or limited, so nothing
    // stays frozen after the window closes. The command line doesn't call
    // it: there, a pause is meant to last.
    void undoControls();
    // The program run for the above; tests hand in a fake.
    void setSystemctl(const QString &program) { m_systemctl = program; }
    const GroupInfo *findGroup(const QString &key) const;
    // An app's name and state for the window: name, frozen, cpuLimit, control.
    Q_INVOKABLE QVariantMap app(const QString &groupKey) const;

    // Rewinding: the process list shows the apps as they were at a moment
    // in the last ten minutes, so a spike can be traced to whoever caused
    // it after it's over. Sampling carries on underneath. A time of 0, or
    // one past the newest scan, returns to now.
    Q_INVOKABLE void rewindTo(double time);
    Q_INVOKABLE void rewindBy(double seconds);
    // The time of a graph sample, counted back from the newest (0), and the
    // reverse: how many samples are newer than a time. For pointing at
    // graphs.
    Q_INVOKABLE double sampleTime(int samplesBack) const;
    Q_INVOKABLE int samplesSince(double time) const;
    // The time of the app scan that covers a time, which rewinding goes to.
    Q_INVOKABLE double momentTime(double time) const;
    // The apps that used the most of `what` ("cpu", "memory", "read",
    // "write", "io", "gpu") at the scan nearest a time: [{name, value}], busiest
    // first, with the scan's own time as the first entry's "time".
    Q_INVOKABLE QVariantList blame(double time, const QString &what, int count) const;

    // The latest process scan, for the command line.
    const QList<ProcessInfo> &processList() const { return m_processList; }
    const QList<GroupInfo> &groupList() const { return m_groupList; }
    // The physical disks' "major:minor", for reading a cgroup's io.stat.
    const QSet<QString> &physicalDevices() const { return m_physicalDevices; }

    // Sizes in IEC units, with the decimal separator of the current language.
    Q_INVOKABLE static QString bytes(double value);
    Q_INVOKABLE static QString rate(double bytesPerSecond);

    // Starts sampling on the timer.
    void start();

    // Takes one full sample now, processes included. Tests call this.
    void sample();
    int processInterval() const;

signals:
    void sampled();
    void intervalChanged();
    void pausedChanged();
    void rewindChanged();

private:
    void sampleCpu();
    void sampleMemory();
    void sampleDisks(double seconds);
    void sampleFilesystems();
    void sampleNetworks(double seconds);
    void sampleGpus();
    // Adds a process's GPU clients, from its fdinfo, to its usage.
    void sampleGpuClients(const QString &base, ProcessInfo &p, bool known, double seconds,
                          QHash<QString, QHash<QString, double>> &shares, QHash<QString, quint64> &memory,
                          QHash<QString, gpu::Client> &clients);
    void tick();
    void sampleSystem();
    void sampleProcessTable();
    void sampleProcesses(double seconds, quint64 cpuTotalDelta);
    QList<GroupInfo> groupProcesses(const QList<ProcessInfo> &processes, double seconds);
    QString groupName(const apps::Identity &identity, const QStringList &memberNames);
    QString findTemperatureInput() const;
    QString controlOf(const QString &cgroup) const;
    QString refusal(const GroupInfo &group) const;
    QString systemctl(const QStringList &arguments);
    QString userName(uint uid);
    void push(const QString &series, qreal value);

    QString m_proc, m_sys;
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_lastSample = -1;
    qint64 m_lastProcessSample = -1;
    quint64 m_processCpuTotal = 0;
    bool m_paused = false;
    QString m_error;

    QString m_hostname, m_kernel, m_cpuModel, m_temperatureInput;
    QList<procfs::CpuTimes> m_prevCpu;
    double m_cpuUsage = 0, m_cpuTemperature = -1, m_cpuFrequency = 0;
    QHash<QString, QList<qreal>> m_history;
    QList<qreal> m_loadAverage;
    QVariantList m_cores;
    qint64 m_uptime = 0;

    double m_memTotal = 0, m_memUsed = 0, m_memAvailable = 0, m_memCached = 0, m_memFree = 0;
    double m_swapTotal = 0, m_swapUsed = 0;

    QHash<QString, procfs::DiskCounters> m_prevDisks;
    QVariantList m_disks, m_filesystems;
    // What the window hides a disk or filesystem by: names like sdb change
    // between boots and plugs, these don't. See diskId() and filesystemIds().
    QString m_dev;
    QHash<QString, QString> m_diskIds;    // Kernel name to stable id.
    QByteArray m_mountsText;              // The mounts the UUIDs were matched for.
    QHash<QString, QString> m_uuidOf;     // Block device name ("sdc2", "dm-0") to filesystem UUID.
    QString diskId(const QString &name);
    bool appleInternal(const QString &name);
    QHash<QString, bool> m_appleController; // Kernel name to: behind Apple's NVMe controller.
    QString blockName(const QString &device) const;

    QHash<QString, procfs::NetCounters> m_prevNet;
    QVariantList m_networks;

    struct PidHistory {
        quint64 ticks = 0;
        procfs::PidIo io;
        bool hasIo = false;
        qint64 ioAt = -1; // When io was read (m_clock ms).
        QString comm, command, name, cgroup;
    };
    QHash<int, PidHistory> m_prevPids;
    QHash<uint, QString> m_users;
    ProcessModel m_processes;
    int m_taskCount = 0;
    QList<ProcessInfo> m_processList;
    QList<GroupInfo> m_groupList;
    QHash<int, QString> m_cgroupOf;
    // Cumulative counters per cgroup path, for rates between scans.
    struct CgroupCounters {
        quint64 cpuUsec = 0;
        quint64 readBytes = 0, writeBytes = 0;
        bool hasIo = false;
        quint64 held = 0;     // From memory.stat.
        qint64 readAt = -1;   // When memory.stat and io.stat were read (m_clock ms).
    };
    QHash<QString, CgroupCounters> m_prevCgroups;
    QSet<QString> m_physicalDevices; // "major:minor" of the disks listed.
    std::optional<apps::DesktopNames> m_desktopNames;
    QHash<QString, QString> m_groupNames;

    std::unique_ptr<gpu::Gpus> m_gpus;
    // Per PID, its open GPU files as (fd, node). Listing every fd of every
    // process is what makes this costly, so a process is listed while it's
    // new, then every few scans if it uses the GPU, and rarely if not.
    struct GpuFds {
        QList<QPair<int, QString>> fds;
        int listed = 0;
    };
    QHash<int, GpuFds> m_gpuFds;
    QHash<QString, gpu::Client> m_prevClients; // By device and client id.
    int m_scanCount = 0;
    long m_pageSize = 4096;

    QString m_systemctl = "systemctl";
    QString m_selfCgroup;
    uint m_uid = 0;
    QSet<QString> m_pausedUnits, m_limitedUnits;

    QList<qint64> m_sampleTimes; // Epoch ms, aligned with every history.
    struct Moment {
        qint64 time = 0;
        QList<GroupInfo> groups;
        QList<ProcessInfo> processes; // Only the ones worth looking back at.
    };
    QList<Moment> m_moments;
    double m_rewoundTo = 0;
    int momentAt(double time) const;
    void recordMoment();
    QString intern(const QString &text);
    QSet<QString> m_strings;
};
