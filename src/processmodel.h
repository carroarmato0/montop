#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QSet>
#include <QString>
#include <QtQml/qqmlregistration.h>

struct ProcessInfo {
    int pid = 0;
    int ppid = 0;
    QString name;
    QString command;
    QString user;
    QString group;          // The key of the app it belongs to.
    char state = '?';
    int threads = 0;
    double cpu = 0;         // Percent of one core, like htop and top.
    quint64 memory = 0;     // Resident bytes.
    double memoryShare = 0; // Fraction of physical memory.
    // Bytes per second, or -1 when /proc/<pid>/io isn't ours to read.
    double readRate = -1;
    double writeRate = -1;
    bool kernel = false;
    // Percent of one GPU, like CPU% of one core, and what its GPU buffers hold.
    double gpu = 0;
    quint64 gpuMemory = 0;
};

// One app: every process in the cgroups that belong to it.
struct GroupInfo {
    QString key;
    QString name;
    QString kind;
    QStringList units;
    QString user;
    QList<int> pids;
    int threads = 0;
    double cpu = 0;
    quint64 memory = 0;
    double memoryShare = 0;
    double readRate = -1;
    double writeRate = -1;
    // Pausing and limiting go through systemd's user manager, unit by unit.
    QStringList controlUnits; // As systemd names them, escapes and all.
    // Why it can't be paused or limited, or empty when it can: "self"
    // (Montop runs inside it), "session" (the desktop needs it), "system"
    // (it isn't the user's), or "none" (no cgroup of its own).
    QString control;
    bool frozen = false;
    double cpuLimit = 0; // Percent of one core, like cpu; 0 for none.
    double gpu = 0;
    quint64 gpuMemory = 0;
};

// The process table, as apps with their processes beneath them or as a flat
// list, filtered and sorted in C++ so the list in QML stays a plain view.
// Rows are identified by a key ("g:<app>" or "p:<pid>"), and updates keep
// the rows the view already has and report a re-sort as a layout change, so
// the list keeps its scroll position and selection while numbers move.
class ProcessModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by the monitor")
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int total READ total NOTIFY countChanged)
    Q_PROPERTY(int groupCount READ groupCount NOTIFY countChanged)
    Q_PROPERTY(Column sortColumn READ sortColumn WRITE setSortColumn NOTIFY sortChanged)
    Q_PROPERTY(bool descending READ descending WRITE setDescending NOTIFY sortChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(bool showKernelThreads READ showKernelThreads WRITE setShowKernelThreads NOTIFY filterChanged)
    Q_PROPERTY(bool grouped READ grouped WRITE setGrouped NOTIFY groupedChanged)

public:
    enum Column { Pid, Name, User, Cpu, Memory, Read, Write, Threads, Gpu };
    Q_ENUM(Column)

    enum Role {
        KeyRole = Qt::UserRole + 1,
        IsGroupRole,
        DepthRole,
        ExpandedRole,
        ProcessCountRole,
        KindRole,
        UnitRole,
        PidRole,
        PpidRole,
        NameRole,
        CommandRole,
        UserRole,
        StateRole,
        ThreadsRole,
        CpuRole,
        MemoryRole,
        MemoryShareRole,
        ReadRateRole,
        WriteRateRole,
        KernelRole,
        GroupKeyRole,
        FrozenRole,
        CpuLimitRole,
        ControlRole,
        GpuRole,
        GpuMemoryRole,
    };

    explicit ProcessModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int total() const;
    int groupCount() const;
    Column sortColumn() const { return m_column; }
    bool descending() const { return m_descending; }
    QString filter() const { return m_filter; }
    bool showKernelThreads() const { return m_showKernel; }
    bool grouped() const { return m_grouped; }

    void setSortColumn(Column column);
    void setDescending(bool descending);
    void setFilter(const QString &filter);
    void setShowKernelThreads(bool show);
    void setGrouped(bool grouped);
    // Whether any GPU reports usage per app; without it, sorting skips GPU.
    void setGpuAvailable(bool available) { m_gpuAvailable = available; }

    // Clicking a header sorts by it; clicking the same header again flips it.
    Q_INVOKABLE void sortBy(Column column);
    Q_INVOKABLE void cycleSort();
    Q_INVOKABLE int rowOf(const QString &key) const;
    Q_INVOKABLE QString keyAt(int row) const;
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE bool isExpanded(const QString &groupKey) const { return m_expanded.contains(groupKey); }
    Q_INVOKABLE void setExpanded(const QString &groupKey, bool expanded);
    // The group row a process row sits under, or an empty string.
    Q_INVOKABLE QString parentKey(const QString &key) const;

    void update(const QList<ProcessInfo> &processes, const QList<GroupInfo> &groups);
    // Repaints rows after an app was paused or limited, without a new scan.
    void refresh(const QList<GroupInfo> &groups);

    // Sorting and filtering, shared with the command line.
    static bool lessThan(const ProcessInfo &a, const ProcessInfo &b, Column column, bool descending);
    static bool lessThan(const GroupInfo &a, const GroupInfo &b, Column column, bool descending);

signals:
    void countChanged();
    void sortChanged();
    void filterChanged();
    void groupedChanged();

private:
    struct Row {
        QString key;
        bool group = false;
        int index = 0; // Into m_groups or m_all.
        int depth = 0;
    };

    QList<Row> visible() const;
    void apply();
    bool matches(const ProcessInfo &p) const;
    bool matches(const GroupInfo &g) const;

    QList<ProcessInfo> m_all;
    QList<GroupInfo> m_groups;
    QHash<int, int> m_byPid;
    QHash<QString, int> m_groupIndex;
    QList<Row> m_rows;
    QSet<QString> m_expanded;
    Column m_column = Cpu;
    bool m_descending = true;
    QString m_filter;
    bool m_showKernel = false;
    bool m_grouped = true;
    bool m_gpuAvailable = false;
};
