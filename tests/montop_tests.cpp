#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <unistd.h>

#include "apps.h"
#include "run.h"
#include "cli.h"
#include "gpu.h"
#include "monitor.h"
#include "processmodel.h"
#include "procfs.h"
#include "sparkline.h"
#include "theme.h"
#include "translator.h"

// Stands in for NVIDIA's library.
class FakeNvml : public gpu::Nvml {
public:
    int calls = 0, busyReads = 0, sensorReads = 0;
    void *device(const QString &pci) override { ++calls; return pci == "0000:01:00.0" ? this : nullptr; }
    QString name(void *) override { return "GeForce GTX 1080"; }
    double busy(void *) override { ++busyReads; return 0.42; }
    bool memory(void *, double &used, double &total) override { used = 1 << 30; total = 8.0 * (1 << 30); return true; }
    double temperature(void *) override { ++sensorReads; return 49; }
    double watts(void *) override { return 9.8; }
    double mhz(void *) override { return 1733; }
    double fan(void *) override { return 35; }
    QHash<int, gpu::ProcessUsage> processes(void *) override { return {{4398, {12.5, 221 << 20}}}; }
};

class MontopTests : public QObject {
    Q_OBJECT

private slots:
    void parsesStat()
    {
        const auto cpus = procfs::parseStat("cpu  100 0 50 800 50 0 0 0 0 0\n"
                                            "cpu0 60 0 20 400 20 0 0 0 0 0\n"
                                            "cpu1 40 0 30 400 30 0 0 0 0 0\n"
                                            "intr 1 2 3\n");
        QCOMPARE(cpus.size(), 3);
        QCOMPARE(cpus[0].total, quint64(1000));
        QCOMPARE(cpus[0].busy, quint64(150)); // idle and iowait are not busy
        QCOMPARE(cpus[2].busy, quint64(70));
    }

    void parsesMeminfo()
    {
        const auto info = procfs::parseMeminfo("MemTotal:       16000 kB\nMemAvailable:    4000 kB\nHugePages_Total: 0\n");
        QCOMPARE(info.value("MemTotal"), quint64(16000) * 1024);
        QCOMPARE(info.value("MemAvailable"), quint64(4000) * 1024);
        QCOMPARE(info.value("HugePages_Total"), quint64(0));
    }

    void parsesDiskstats()
    {
        const auto disks = procfs::parseDiskstats(
            " 259       0 nvme0n1 18809 22403 3455903 2866 198616 186189 24770666 100954 0 16069 106888 861 0 21600328 772\n");
        QCOMPARE(disks.size(), 1);
        QCOMPARE(disks[0].name, QString("nvme0n1"));
        QCOMPARE(disks[0].readBytes, quint64(3455903) * 512);
        QCOMPARE(disks[0].writeBytes, quint64(24770666) * 512);
        QCOMPARE(disks[0].busyMs, quint64(16069));
    }

    void parsesNetDev()
    {
        const auto nets = procfs::parseNetDev(
            "Inter-|   Receive                            |  Transmit\n"
            " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets\n"
            "    lo: 1000 10 0 0 0 0 0 0 1000 10 0 0 0 0 0 0\n"
            "wlan0: 5000 50 0 0 0 0 0 0 7000 70 0 0 0 0 0 0\n");
        QCOMPARE(nets.size(), 2);
        QCOMPARE(nets[1].name, QString("wlan0"));
        QCOMPARE(nets[1].rxBytes, quint64(5000));
        QCOMPARE(nets[1].txBytes, quint64(7000));
    }

    void parsesPidStatWithAwkwardNames()
    {
        procfs::PidStat stat;
        // A comm with spaces and a closing parenthesis must not shift fields.
        QVERIFY(procfs::parsePidStat("42 (Web (Content)) S 7 42 42 0 -1 4194560 100 0 0 0 "
                                     "30 12 0 0 20 0 9 0 5000 100000 250 18446744073709551615",
                                     stat));
        QCOMPARE(stat.pid, 42);
        QCOMPARE(stat.comm, QString("Web (Content)"));
        QCOMPARE(stat.state, 'S');
        QCOMPARE(stat.ppid, 7);
        QCOMPARE(stat.cpuTicks, quint64(42));
        QCOMPARE(stat.threads, 9);
        QCOMPARE(stat.rssPages, quint64(250));
    }

    void flattensCommandLines()
    {
        QCOMPARE(procfs::parseCmdline(QByteArray("python\0-c\0import os\nprint(1)\0", 30)),
                 QString("python -c import os print(1)"));
    }

    void parsesMountsOncePerDevice()
    {
        const auto mounts = procfs::parseMounts(
            "/dev/mapper/root /home btrfs rw 0 0\n"
            "/dev/mapper/root / btrfs rw 0 0\n"
            "proc /proc proc rw 0 0\n"
            "/dev/sda1 /mnt/my\\040disk ext4 rw 0 0\n");
        QCOMPARE(mounts.size(), 2);
        QCOMPARE(mounts[0].path, QString("/"));
        QCOMPARE(mounts[1].path, QString("/mnt/my disk"));
    }

    void modelSortsFiltersAndHidesKernelThreads()
    {
        ProcessModel model;
        model.setGrouped(false);
        model.update({process(10, "zsh", 1.0), process(11, "brave", 40.0), process(12, "kworker/0:1", 0, true),
                      process(13, "Hyprland", 5.0)}, {});
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.keyAt(0), QString("p:11")); // CPU, highest first

        model.setSortColumn(ProcessModel::Name);
        QCOMPARE(model.keyAt(0), QString("p:11")); // brave, Hyprland, zsh: A to Z, ignoring case
        QCOMPARE(model.keyAt(1), QString("p:13"));

        model.setFilter("hypr");
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.rowOf("p:13"), 0);

        model.setFilter("");
        model.setShowKernelThreads(true);
        QCOMPARE(model.rowCount(), 4);
    }

    void modelKeepsRowsAcrossUpdates()
    {
        ProcessModel model;
        model.setGrouped(false);
        model.update({process(1, "a", 1), process(2, "b", 2)}, {});
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        model.update({process(2, "b", 0), process(3, "c", 9)}, {});
        QCOMPARE(reset.count(), 0);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(model.keyAt(0), QString("p:3"));
        QCOMPARE(model.keyAt(1), QString("p:2"));
    }

    void monitorMeasuresFromProc()
    {
        QTemporaryDir root;
        const QString proc = root.filePath("proc"), sys = root.filePath("sys");
        QDir().mkpath(proc + "/100");
        QDir().mkpath(sys + "/block/sda");
        write(sys + "/block/sda/size", "1000\n");
        write(proc + "/meminfo", "MemTotal: 1000 kB\nMemFree: 200 kB\nMemAvailable: 600 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n");
        write(proc + "/loadavg", "0.50 0.25 0.10 1/100 100\n");
        write(proc + "/uptime", "3600.5 100.0\n");
        write(proc + "/net/dev", "h\nh\neth0: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
        write(proc + "/self/mounts", "");
        write(proc + "/100/cmdline", QByteArray("/usr/bin/worker\0--busy\0", 23));

        // Two CPUs; the process has used 0 ticks so far.
        write(proc + "/stat", "cpu  0 0 0 0 0 0 0 0\ncpu0 0 0 0 0 0 0 0 0\ncpu1 0 0 0 0 0 0 0 0\n");
        write(proc + "/diskstats", "8 0 sda 0 0 0 0 0 0 0 0 0 0 0\n");
        write(proc + "/100/stat", pidStat(100, "worker", 0));
        Monitor monitor(proc, sys);

        // 200 ticks pass across both CPUs, 50 busy; the process used 100 of
        // them, which is one whole core.
        write(proc + "/stat", "cpu  50 0 0 150 0 0 0 0\ncpu0 50 0 0 50 0 0 0 0\ncpu1 0 0 0 100 0 0 0 0\n");
        write(proc + "/100/stat", pidStat(100, "worker", 100));
        write(proc + "/diskstats", "8 0 sda 0 0 2048 0 0 0 0 0 0 0 0\n");
        QTest::qWait(100);
        monitor.sample();

        QCOMPARE(monitor.coreCount(), 2);
        QCOMPARE(monitor.cpuUsage(), 0.25);
        QCOMPARE(monitor.cores()[0].toMap().value("usage").toDouble(), 0.5);
        QCOMPARE(monitor.memoryUsed(), 400.0 * 1024);
        QCOMPARE(monitor.uptime(), qint64(3600));
        QCOMPARE(monitor.loadAverage().value(0), 0.5);
        QCOMPARE(monitor.disks().size(), 1);
        QVERIFY(monitor.disks()[0].toMap().value("readRate").toDouble() > 0);
        QVERIFY(monitor.error().isEmpty());

        ProcessModel *model = monitor.processes();
        QCOMPARE(model->rowCount(), 1); // No cgroup: the process is its own app.
        QCOMPARE(model->keyAt(0), QString("g:process:worker"));
        model->setGrouped(false);
        QCOMPARE(model->rowCount(), 1);
        const QVariantMap p = model->get(0);
        QCOMPARE(p.value("name").toString(), QString("worker"));
        QCOMPARE(p.value("command").toString(), QString("/usr/bin/worker --busy"));
        QCOMPARE(p.value("cpu").toDouble(), 100.0);
        QCOMPARE(p.value("readRate").toDouble(), -1.0); // No io file, so unknown.
    }

    void classifiesCgroups()
    {
        auto check = [](const char *path, const char *key, const char *kind, const char *id) {
            const apps::Identity identity = apps::classify(path);
            QCOMPARE(identity.key, QString(key));
            QCOMPARE(identity.kind, QString(kind));
            QCOMPARE(identity.id, QString(id));
        };
        const QString user = "/user.slice/user-1000.slice/user@1000.service/";
        // Escaped dashes belong to the name; literal ones separate launcher,
        // app, and the random suffix.
        check(qPrintable(user + "app.slice/app-graphical.slice/app-Hyprland-xdg\\x2dterminal\\x2dexec-a7273f50.scope"),
              "app:xdg-terminal-exec", "app", "xdg-terminal-exec");
        check(qPrintable(user + "app.slice/app-firefox-1234.scope"), "app:firefox", "app", "firefox");
        check(qPrintable(user + "app.slice/app-Hyprland-org.gnome.Nautilus@ab12.service"), "app:org.gnome.Nautilus", "app",
              "org.gnome.Nautilus");
        check(qPrintable(user + "app.slice/superwhisper.service"), "service:superwhisper", "service", "superwhisper");
        check(qPrintable(user + "session.slice/wayland-wm@hyprland.desktop.service"),
              "service:wayland-wm@hyprland.desktop", "service", "wayland-wm@hyprland.desktop");
        // Started over D-Bus: the bus address and instance change each launch.
        check(qPrintable(user + "app.slice/dbus-:1.1-org.gnome.Nautilus@2.service"), "service:org.gnome.Nautilus", "service",
              "org.gnome.Nautilus");
        check("/system.slice/NetworkManager.service", "system:NetworkManager", "system", "NetworkManager");
        check("/init.scope", "system:systemd", "system", "systemd");
        check("/user.slice/user-1000.slice/session-1.scope", "session", "session", "session");
        check("/", "kernel", "kernel", "kernel");
        QCOMPARE(apps::unescape("a\\x2db\\x40c"), QString("a-b@c"));
    }

    void readsDesktopNames()
    {
        QTemporaryDir user, system;
        write(user.filePath("foot.desktop"), "[Desktop Entry]\nName=My Foot\n");
        write(system.filePath("foot.desktop"), "[Desktop Entry]\nName=Foot\n");
        write(system.filePath("sub/org.gnome.Nautilus.desktop"),
              "[Desktop Action new]\nName=New Window\n[Desktop Entry]\nName=Files\nName[nl]=Bestanden\n");
        apps::DesktopNames names({user.path(), system.path()});
        QCOMPARE(names.name("foot"), QString("My Foot")); // The user's own entry wins.
        QCOMPARE(names.name("org.gnome.nautilus"), QString("Files"));
        QVERIFY(names.name("missing").isEmpty());
    }

    void namesOtherDesktopsComponents()
    {
        QCOMPARE(apps::componentName("org.gnome.Shell"), QString("GNOME Shell"));
        QCOMPARE(apps::componentName("org.gnome.SettingsDaemon.Power"), QString("GNOME Settings (Power)"));
        QVERIFY(apps::componentName("org.gnome.Nautilus").isEmpty());

        QTemporaryDir root;
        const QString proc = root.filePath("proc"), sys = root.filePath("sys");
        const QString user = "/user.slice/user-1000.slice/user@1000.service/";
        write(proc + "/meminfo", "MemTotal: 1000 kB\nMemAvailable: 600 kB\n");
        write(proc + "/stat", "cpu  0 0 0 0 0 0 0 0\ncpu0 0 0 0 0 0 0 0 0\n");
        write(root.filePath("share/applications/org.gnome.Nautilus.desktop"), "[Desktop Entry]\nName=Files\n");
        const QList<QPair<int, QString>> units{
            {100, user + "app.slice/dbus-:1.1-org.gnome.Nautilus@2.service"},
            {101, user + "session.slice/org.gnome.Shell@user.service"},
            {102, user + "session.slice/org.gnome.SettingsDaemon.Power.service"},
            {103, user + "app.slice/app-gnome-org.gnome.SettingsDaemon.DiskUtilityNotify-2926.scope"},
            {104, user + "app.slice/helper@main.service"},
            {105, "/system.slice/system-getty.slice/getty@tty1.service"},
        };
        for (const auto &[pid, cgroup] : units) {
            write(proc + QString("/%1/stat").arg(pid), pidStat(pid, "x", 0));
            write(proc + QString("/%1/cgroup").arg(pid), "0::" + cgroup.toUtf8() + "\n");
        }
        const QByteArray home = qgetenv("XDG_DATA_HOME"), dirs = qgetenv("XDG_DATA_DIRS");
        qputenv("XDG_DATA_HOME", root.filePath("share").toUtf8());
        qputenv("XDG_DATA_DIRS", root.filePath("share").toUtf8());
        Monitor monitor(proc, sys);
        qputenv("XDG_DATA_HOME", home);
        qputenv("XDG_DATA_DIRS", dirs);
        QHash<QString, QString> names;
        for (const GroupInfo &g : monitor.groupList())
            names.insert(g.units.value(0), g.name);
        QCOMPARE(names.value("dbus-:1.1-org.gnome.Nautilus@2.service"), QString("Files"));
        QCOMPARE(names.value("org.gnome.Shell@user.service"), QString("GNOME Shell"));
        QCOMPARE(names.value("org.gnome.SettingsDaemon.Power.service"), QString("GNOME Settings (Power)"));
        QCOMPARE(names.value("app-gnome-org.gnome.SettingsDaemon.DiskUtilityNotify-2926.scope"),
                 QString("GNOME Settings (DiskUtilityNotify)"));
        QCOMPARE(names.value("helper@main.service"), QString("helper")); // No entry: the template's name.
        QCOMPARE(names.value("getty@tty1.service"), QString("getty@tty1")); // System services keep theirs.
    }

    void modelGroupsAndExpands()
    {
        ProcessModel model;
        QList<ProcessInfo> processes{process(10, "brave", 30), process(11, "brave", 10), process(20, "foot", 1),
                                     process(21, "make", 50), process(2, "kthreadd", 0, true)};
        for (int i : {0, 1})
            processes[i].group = "app:brave";
        for (int i : {2, 3})
            processes[i].group = "app:foot";
        processes[4].group = "kernel";
        QList<GroupInfo> groups{group("app:brave", "Brave", {10, 11}, 40), group("app:foot", "Foot", {20, 21}, 51),
                                group("kernel", "Kernel", {2}, 0, "kernel")};
        model.update(processes, groups);

        // Apps only, busiest first, and no kernel until asked.
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.keyAt(0), QString("g:app:foot"));
        QCOMPARE(model.groupCount(), 2);

        model.setExpanded("app:foot", true);
        QCOMPARE(model.rowCount(), 4);
        QCOMPARE(model.keyAt(1), QString("p:21")); // make, the busier one, first
        QCOMPARE(model.data(model.index(1), ProcessModel::DepthRole).toInt(), 1);
        QCOMPARE(model.parentKey("p:21"), QString("g:app:foot"));

        // A filter that matches a process shows it under its app, opened.
        model.setExpanded("app:foot", false);
        model.setFilter("brave");
        QCOMPARE(model.rowCount(), 1); // The app's own name matched: it stays closed.
        model.setFilter("make");
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.keyAt(1), QString("p:21"));

        model.setFilter("");
        model.setGrouped(false);
        QCOMPARE(model.rowCount(), 4);
        QCOMPARE(model.keyAt(0), QString("p:21"));
    }

    void monitorGroupsByCgroup()
    {
        QTemporaryDir root;
        const QString proc = root.filePath("proc"), sys = root.filePath("sys");
        const QString scope = "/user.slice/user-1000.slice/user@1000.service/app.slice/app-Hyprland-testapp-1a2b.scope";
        write(proc + "/meminfo", "MemTotal: 1000 kB\nMemAvailable: 600 kB\n");
        write(proc + "/stat", "cpu  0 0 0 0 0 0 0 0\ncpu0 0 0 0 0 0 0 0 0\n");
        write(sys + "/block/sda/size", "1000\n");
        write(sys + "/block/sda/dev", "8:0\n");
        write(proc + "/diskstats", "8 0 sda 0 0 0 0 0 0 0 0 0 0 0\n");
        for (int pid : {100, 101}) {
            write(proc + QString("/%1/stat").arg(pid), pidStat(pid, "testapp", 0));
            write(proc + QString("/%1/cgroup").arg(pid), "0::" + scope.toUtf8() + "\n");
        }
        write(sys + "/fs/cgroup" + scope + "/cpu.stat", "usage_usec 1000000\n");
        write(sys + "/fs/cgroup" + scope + "/memory.stat", "anon 4096\nfile 999999\nshmem 1024\nkernel 1024\n");
        write(sys + "/fs/cgroup" + scope + "/io.stat", "8:0 rbytes=0 wbytes=0\n253:0 rbytes=0 wbytes=0\n");
        Monitor monitor(proc, sys);

        // Half a second of CPU over about a tenth of a second would be far
        // more than the processes' own (zero) ticks: the cgroup counts
        // children that already exited.
        write(sys + "/fs/cgroup" + scope + "/cpu.stat", "usage_usec 1050000\n");
        write(sys + "/fs/cgroup" + scope + "/io.stat", "8:0 rbytes=1048576 wbytes=0\n253:0 rbytes=1048576 wbytes=0\n");
        write(proc + "/stat", "cpu  10 0 0 10 0 0 0 0\ncpu0 10 0 0 10 0 0 0 0\n");
        QTest::qWait(100);
        monitor.sample();

        QCOMPARE(monitor.groupList().size(), 1);
        const GroupInfo g = monitor.groupList().first();
        QCOMPARE(g.key, QString("app:testapp"));
        QCOMPARE(g.kind, QString("app"));
        QCOMPARE(g.pids.size(), 2);
        QCOMPARE(g.memory, quint64(6144)); // anon + shmem + kernel, not the page cache
        QVERIFY(g.cpu > 20);
        // Only the physical disk counts, not the volume stacked on it.
        QVERIFY(g.readRate > 0 && g.readRate < 1048576 * 20);

        const QJsonObject snapshot = snapshotJson(monitor, 5, "cpu");
        QCOMPARE(snapshot["apps"].toArray().size(), 1);
        const QJsonObject app = snapshot["apps"].toArray().first().toObject();
        QCOMPARE(app["processes"].toInt(), 2);
        QCOMPARE(app["kind"].toString(), QString("app"));
        QCOMPARE(app["top_processes"].toArray().size(), 2);
        QCOMPARE(snapshot["processes"].toArray().first().toObject()["app"].toString(), g.name);
        QVERIFY(snapshot["memory"].toObject()["total_bytes"].toDouble() > 0);
        QVERIFY(snapshot["cpu"].toObject()["temperature_celsius"].isNull());
    }

    void monitorPausesAndLimitsApps()
    {
        QTemporaryDir root;
        const QString proc = root.filePath("proc"), sys = root.filePath("sys");
        const QString manager = QString("/user.slice/user-%1.slice/user@%1.service").arg(getuid());
        const QString app = manager + "/app.slice/app-Hyprland-busy-1a2b.scope";
        const QString compositor = manager + "/session.slice/wayland-wm@hyprland.desktop.service";
        const QString system = "/system.slice/upower.service";
        write(proc + "/meminfo", "MemTotal: 1000 kB\nMemAvailable: 600 kB\n");
        write(proc + "/stat", "cpu  0 0 0 0 0 0 0 0\ncpu0 0 0 0 0 0 0 0 0\n");
        const QList<QPair<int, QString>> processes{{100, app}, {200, compositor}, {300, system}};
        for (const auto &[pid, cgroup] : processes) {
            write(proc + QString("/%1/stat").arg(pid), pidStat(pid, "p" + QByteArray::number(pid), 0));
            write(proc + QString("/%1/cgroup").arg(pid), "0::" + cgroup.toUtf8() + "\n");
            write(sys + "/fs/cgroup" + cgroup + "/cpu.stat", "usage_usec 0\n");
        }
        write(sys + "/fs/cgroup" + app + "/cpu.max", "50000 100000\n");
        write(sys + "/fs/cgroup" + app + "/cgroup.events", "populated 1\nfrozen 1\n");
        // Records what it was asked, the way systemctl would be.
        const QString log = root.filePath("calls");
        write(root.filePath("systemctl"), "#!/bin/sh\necho \"$*\" >> " + log.toUtf8() + "\n");
        QFile::setPermissions(root.filePath("systemctl"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

        Monitor monitor(proc, sys);
        monitor.setSystemctl(root.filePath("systemctl"));
        const GroupInfo *busy = monitor.findGroup("app:busy");
        QVERIFY(busy);
        QVERIFY(busy->control.isEmpty());
        QVERIFY(busy->frozen);
        QCOMPARE(busy->cpuLimit, 50.0);
        QCOMPARE(busy->controlUnits, QStringList{"app-Hyprland-busy-1a2b.scope"});
        QCOMPARE(monitor.findGroup("service:wayland-wm@hyprland.desktop")->control, QString("session"));
        QCOMPARE(monitor.findGroup("system:upower")->control, QString("system"));

        QVERIFY(monitor.setAppPaused("app:busy", false).isEmpty());
        QVERIFY(!monitor.findGroup("app:busy")->frozen);
        QVERIFY(monitor.setAppCpuLimit("app:busy", 25).isEmpty());
        QCOMPARE(monitor.findGroup("app:busy")->cpuLimit, 25.0);
        // The desktop's own services and the system's are never touched.
        QVERIFY(!monitor.setAppPaused("service:wayland-wm@hyprland.desktop", true).isEmpty());
        QVERIFY(!monitor.setAppCpuLimit("system:upower", 10).isEmpty());
        QVERIFY(monitor.setAppPaused("app:busy", true).isEmpty());
        monitor.undoControls();

        QFile calls(log);
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(calls.readAll()).split('\n', Qt::SkipEmptyParts),
                 QStringList({"--user thaw app-Hyprland-busy-1a2b.scope",
                              "--user set-property --runtime app-Hyprland-busy-1a2b.scope CPUQuota=25%",
                              "--user freeze app-Hyprland-busy-1a2b.scope",
                              "--user thaw app-Hyprland-busy-1a2b.scope",
                              "--user set-property --runtime app-Hyprland-busy-1a2b.scope CPUQuota="}));
    }

    void monitorRewindsToEarlierMoments()
    {
        QTemporaryDir root;
        const QString proc = root.filePath("proc"), sys = root.filePath("sys");
        const QString scope = "/user.slice/user-1000.slice/user@1000.service/app.slice/app-Hyprland-build-1a2b.scope";
        write(proc + "/meminfo", "MemTotal: 1000 kB\nMemAvailable: 600 kB\n");
        write(proc + "/stat", "cpu  0 0 0 0 0 0 0 0\ncpu0 0 0 0 0 0 0 0 0\n");
        write(proc + "/100/stat", pidStat(100, "make", 0));
        write(proc + "/100/cgroup", "0::" + scope.toUtf8() + "\n");
        write(sys + "/fs/cgroup" + scope + "/cpu.stat", "usage_usec 0\n");
        Monitor monitor(proc, sys);

        // A burst of CPU, then nothing.
        QTest::qWait(20);
        write(sys + "/fs/cgroup" + scope + "/cpu.stat", "usage_usec 20000\n");
        monitor.sample();
        const double burst = monitor.sampleTime(0);
        QTest::qWait(20);
        monitor.sample();
        QCOMPARE(monitor.findGroup("app:build")->cpu, 0.0);

        const QVariantList blamed = monitor.blame(burst, "cpu", 3);
        QCOMPARE(blamed.size(), 1);
        QCOMPARE(blamed.first().toMap()["key"].toString(), QString("app:build"));
        QVERIFY(blamed.first().toMap()["value"].toDouble() > 10);

        QSignalSpy rewound(&monitor, &Monitor::rewindChanged);
        monitor.rewindTo(burst);
        QCOMPARE(monitor.rewoundTo(), monitor.momentTime(burst));
        QCOMPARE(rewound.count(), 1);
        ProcessModel *model = monitor.processes();
        QVERIFY(model->data(model->index(model->rowOf("g:app:build")), ProcessModel::CpuRole).toDouble() > 10);
        // New scans don't move the list while it shows the past.
        monitor.sample();
        QVERIFY(model->data(model->index(model->rowOf("g:app:build")), ProcessModel::CpuRole).toDouble() > 10);

        // Rewinding further than the history holds stops at its start.
        monitor.rewindBy(-3600);
        QCOMPARE(monitor.rewoundTo(), monitor.historyStart());
        monitor.rewindTo(0);
        QCOMPARE(monitor.rewoundTo(), 0.0);
        QCOMPARE(model->data(model->index(model->rowOf("g:app:build")), ProcessModel::CpuRole).toDouble(), 0.0);
    }

    void parsesGpuClients()
    {
        gpu::Client before, now;
        QVERIFY(!gpu::parseFdinfo("pos:\t0\nflags:\t02\n", before));
        // i915: engine time in ns, memory per region, video engines in twos.
        QVERIFY(gpu::parseFdinfo("pos:\t0\ndrm-driver:\ti915\ndrm-client-id:\t6\ndrm-pdev:\t0000:00:02.0\n"
                                 "drm-total-system0:\t8192\ndrm-resident-system0:\t4096\n"
                                 "drm-engine-render:\t1000000000 ns\ndrm-engine-video:\t0 ns\ndrm-engine-capacity-video:\t2\n", before));
        QCOMPARE(before.driver, QString("i915"));
        QCOMPARE(before.id, QString("6"));
        QCOMPARE(before.memory, quint64(4096)); // Resident only, not total.
        QCOMPARE(before.capacity.value("video"), quint64(2));
        QVERIFY(gpu::parseFdinfo("drm-driver:\ti915\ndrm-client-id:\t6\ndrm-resident-system0:\t4096\n"
                                 "drm-engine-render:\t1250000000 ns\ndrm-engine-video:\t500000000 ns\ndrm-engine-capacity-video:\t2\n", now));
        const QHash<QString, double> shares = gpu::engineShares(before, now, 1.0);
        QCOMPARE(shares.value("render"), 0.25);
        QCOMPARE(shares.value("video"), 0.25); // Half a second over two engines.

        // amdgpu's older memory keys, in KiB.
        gpu::Client amd;
        QVERIFY(gpu::parseFdinfo("drm-driver:\tamdgpu\ndrm-memory-vram:\t2048 KiB\ndrm-memory-gtt: \t1024 KiB\n"
                                 "drm-engine-gfx:\t5 ns\n", amd));
        QCOMPARE(amd.memory, quint64(3 << 20));

        // xe: busy cycles over the GPU's own, no time at all.
        gpu::Client xeBefore, xeNow;
        QVERIFY(gpu::parseFdinfo("drm-driver:\txe\ndrm-cycles-rcs:\t100\ndrm-total-cycles-rcs:\t1000\n", xeBefore));
        QVERIFY(gpu::parseFdinfo("drm-driver:\txe\ndrm-cycles-rcs:\t400\ndrm-total-cycles-rcs:\t2000\n", xeNow));
        QCOMPARE(gpu::engineShares(xeBefore, xeNow, 0.5).value("rcs"), 0.3);
    }

    void readsGpuDevices()
    {
        QTemporaryDir dir;
        const QString sys = dir.filePath("sys");
        write(dir.filePath("pci.ids"), "# ids\n10de  NVIDIA Corporation\n\t1b80  GP104 [GeForce GTX 1080]\n"
                                       "8086  Intel Corporation\n\t46a8  Alder Lake-UP3 GT2 [Iris Xe Graphics]\n");
        // A display controller only (no render node): not a GPU to list.
        gpuDevice(sys, "simple-framebuffer.0", 0, 0, "DRIVER=simple-framebuffer\n");
        // An AMD card with its own counters and sensors.
        const QString amd = gpuDevice(sys, "pci0000:00/0000:03:00.0", 1, 128, "DRIVER=amdgpu\nPCI_SLOT_NAME=0000:03:00.0\n");
        write(amd + "/product_name", "AMD Radeon RX 7600\n");
        write(amd + "/gpu_busy_percent", "37\n");
        write(amd + "/mem_info_vram_used", "1073741824\n");
        write(amd + "/mem_info_vram_total", "8589934592\n");
        write(amd + "/hwmon/hwmon4/temp1_input", "54000\n");
        write(amd + "/hwmon/hwmon4/power1_average", "23500000\n");
        write(amd + "/hwmon/hwmon4/freq1_input", "2400000000\n");
        // An NVIDIA card asleep, and one awake, both through the fake NVML.
        const QString sleeping = gpuDevice(sys, "pci0000:00/0000:02:00.0", 2, 129, "DRIVER=nvidia\nPCI_SLOT_NAME=0000:02:00.0\n");
        write(sleeping + "/power/runtime_status", "suspended\n");
        const QString nvidia = gpuDevice(sys, "pci0000:00/0000:01:00.0", 3, 130, "DRIVER=nvidia\nPCI_SLOT_NAME=0000:01:00.0\n");
        write(nvidia + "/vendor", "0x10de\n");
        write(nvidia + "/device", "0x1b80\n");
        write(nvidia + "/power/runtime_status", "active\n");
        // Apple's GPU on Asahi Linux, which reports no usage.
        gpuDevice(sys, "platform/soc/206400000.gpu", 4, 131, "DRIVER=asahi\n");

        gpu::Gpus gpus(sys, dir.filePath("pci.ids"));
        auto *fake = new FakeNvml;
        gpus.setNvml(std::unique_ptr<gpu::Nvml>(fake));
        gpus.sample(1000);
        const QList<gpu::Device> &devices = gpus.devices();
        QCOMPARE(devices.size(), 4);

        const gpu::Device &radeon = devices[0];
        QCOMPARE(radeon.name, QString("AMD Radeon RX 7600"));
        QCOMPARE(radeon.busy, 0.37);
        QCOMPARE(radeon.memoryTotal, 8589934592.0);
        QCOMPARE(radeon.temperature, 54.0);
        QCOMPARE(radeon.watts, 23.5);
        QCOMPARE(radeon.mhz, 2400.0);
        QVERIFY(radeon.perApp);

        // Asleep: nothing read, NVML not even asked.
        QVERIFY(devices[1].asleep);
        QCOMPARE(devices[1].busy, -1.0);
        QCOMPARE(devices[1].name, QString("NVIDIA GPU")); // No PCI id to look up.

        const gpu::Device &geforce = devices[2];
        QCOMPARE(fake->calls, 1);
        QCOMPARE(geforce.name, QString("NVIDIA GeForce GTX 1080"));
        QCOMPARE(geforce.busy, 0.42);
        QCOMPARE(geforce.memoryUsed, double(1 << 30));
        QCOMPARE(geforce.fan, 35.0);
        QVERIFY(geforce.perApp);
        const auto usage = gpus.nvidiaProcesses();
        QCOMPARE(usage.value(4398).percent, 12.5);
        QCOMPARE(usage.value(4398).memory, quint64(221 << 20));
        // Busy every sample; the slower facts once a second, kept between.
        gpus.sample(1250);
        gpus.sample(1500);
        QCOMPARE(fake->busyReads, 3);
        QCOMPARE(fake->sensorReads, 1);
        QCOMPARE(gpus.devices()[2].temperature, 49.0);
        gpus.sample(2000);
        QCOMPARE(fake->sensorReads, 2);
        QCOMPARE(fake->calls, 1); // The handle is looked up once.

        const gpu::Device &apple = devices[3];
        QCOMPARE(apple.name, QString("Apple GPU"));
        QCOMPARE(apple.busy, -1.0);
        QVERIFY(!apple.perApp);
        QVERIFY(!apple.note.isEmpty());

        // Without NVML, an NVIDIA card says why it shows nothing.
        gpu::Gpus bare(sys, dir.filePath("pci.ids"));
        bare.setNvml(nullptr);
        bare.sample(1000);
        QVERIFY(!bare.devices()[2].note.isEmpty());
        QVERIFY(!bare.devices()[2].perApp);
        QCOMPARE(bare.devices()[2].name, QString("NVIDIA GeForce GTX 1080")); // From the PCI ids.
    }

    void readsAppleGpuStats()
    {
        QTemporaryDir dir;
        const QString sys = dir.filePath("sys");
        const QString agx = gpuDevice(sys, "platform/soc/206400000.gpu", 1, 128, "DRIVER=asahi\n");
        auto stats = [&](quint64 busyNs) {
            write(agx + "/agx_stats", QString("busy_ns %1\npower_mw 4200\njobs 12\npstate 3\nutil1 9\n").arg(busyNs).toUtf8());
        };
        stats(5000000000ULL);
        write(agx + "/gpu_memory", "user_committed 900000000\nuser_resident 734003200\nkernel_resident 52428800\n"
                                   "limit 17179869184\nclient_limit 0\nrefused 0\nuser_kept 0\n");
        gpu::Gpus gpus(sys, dir.filePath("pci.ids"));
        gpus.sample(1000);
        const gpu::Device &apple = gpus.devices().first();
        QCOMPARE(apple.busy, -1.0); // A baseline only.
        QCOMPARE(apple.watts, 4.2);
        QCOMPARE(apple.appMemory, 734003200.0);
        QCOMPARE(apple.mhz, -1.0); // No frequency key on this kernel.
        QVERIFY(apple.note.isEmpty());
        QVERIFY(!apple.perApp);

        // Too soon after the last reading: nothing new yet.
        stats(5100000000ULL);
        gpus.sample(1250);
        QCOMPARE(gpus.devices().first().busy, -1.0);
        // 0.25 s busy in a second, once the window is long enough.
        stats(5250000000ULL);
        gpus.sample(2000);
        QVERIFY(qAbs(gpus.devices().first().busy - 0.25) < 1e-9);
        // Kept between publications, then eased towards the new share.
        gpus.sample(2250);
        QVERIFY(qAbs(gpus.devices().first().busy - 0.25) < 1e-9);
        stats(6250000000ULL);
        gpus.sample(3000);
        const double busy = gpus.devices().first().busy;
        QVERIFY(busy > 0.25 && busy < 1.0);

        // Newer kernels add the clock; newer firmware may not know the rest.
        write(agx + "/agx_stats", "busy_ns 6250000000\npower_mw unsupported\nrequested_frequency_hz 1398000000\n"
                                  "achieved_frequency_hz unsupported\n");
        gpus.sample(3500);
        QCOMPARE(gpus.devices().first().mhz, 1398.0);
        QCOMPARE(gpus.devices().first().watts, -1.0);
        write(agx + "/agx_stats", "busy_ns 6250000000\nachieved_frequency_hz 1260000000\n");
        gpus.sample(3600);
        QCOMPARE(gpus.devices().first().mhz, 1260.0);

        // The counter going back (driver reloaded) starts over.
        stats(1000);
        gpus.sample(4000);
        QCOMPARE(gpus.devices().first().busy, -1.0);

        // A kernel without agx_stats: as before, no usage and a note.
        QFile::remove(agx + "/agx_stats");
        QFile::remove(agx + "/gpu_memory");
        gpus.sample(5000);
        QCOMPARE(gpus.devices().first().busy, -1.0);
        QCOMPARE(gpus.devices().first().watts, -1.0);
        QCOMPARE(gpus.devices().first().appMemory, -1.0);
        QVERIFY(!gpus.devices().first().note.isEmpty());
    }

    void monitorPutsGpuUsageDownToApps()
    {
        QTemporaryDir root;
        const QString proc = root.filePath("proc"), sys = root.filePath("sys");
        const QString scope = "/user.slice/user-1000.slice/user@1000.service/app.slice/app-Hyprland-game-1a2b.scope";
        write(proc + "/meminfo", "MemTotal: 1000 kB\nMemAvailable: 600 kB\n");
        write(proc + "/stat", "cpu  0 0 0 0 0 0 0 0\ncpu0 0 0 0 0 0 0 0 0\n");
        const QString intel = gpuDevice(sys, "pci0000:00/0000:00:02.0", 1, 128, "DRIVER=i915\nPCI_SLOT_NAME=0000:00:02.0\n");
        write(sys + "/class/drm/card1/gt_act_freq_mhz", "1100\n");
        // Two processes holding the same client (an inherited fd), and one
        // with a client of its own that doesn't use the GPU.
        auto fdinfo = [](int client, quint64 ns) {
            return QString("pos:\t0\ndrm-driver:\ti915\ndrm-client-id:\t%1\ndrm-resident-system0:\t1048576\n"
                           "drm-engine-render:\t%2 ns\n").arg(client).arg(ns).toUtf8();
        };
        for (int pid : {100, 101, 102}) {
            write(proc + QString("/%1/stat").arg(pid), pidStat(pid, pid == 102 ? "idle" : "game", 0));
            write(proc + QString("/%1/cgroup").arg(pid), "0::" + scope.toUtf8() + "\n");
            QDir().mkpath(proc + QString("/%1/fd").arg(pid));
            QFile::link("/dev/dri/renderD128", proc + QString("/%1/fd/5").arg(pid));
            write(proc + QString("/%1/fdinfo/5").arg(pid), fdinfo(pid == 102 ? 9 : 7, 0));
        }
        Monitor monitor(proc, sys);
        QCOMPARE(monitor.gpus().size(), 1);
        QCOMPARE(monitor.gpus().first().toMap()["name"].toString(), QString("Intel GPU")); // No PCI ids here.

        // Ten seconds of engine time in a fraction of one: fully busy.
        write(proc + "/100/fdinfo/5", fdinfo(7, 10000000000ULL));
        write(proc + "/101/fdinfo/5", fdinfo(7, 10000000000ULL));
        QTest::qWait(100);
        monitor.sample();

        const GroupInfo g = monitor.groupList().first();
        QCOMPARE(g.gpu, 100.0); // The shared client counts once, not twice.
        QCOMPARE(g.gpuMemory, quint64(2 << 20));
        const QVariantMap device = monitor.gpus().first().toMap();
        QCOMPARE(device["busy"].toDouble(), 1.0);
        QCOMPARE(device["appMemory"].toDouble(), double(2 << 20));
        QCOMPARE(device["mhz"].toDouble(), 1100.0);
        QVERIFY(monitor.gpuPerApp());
        QCOMPARE(monitor.blame(monitor.sampleTime(0), "gpu", 3).first().toMap()["value"].toDouble(), 100.0);

        const QJsonObject snapshot = snapshotJson(monitor, 5, "gpu");
        QCOMPARE(snapshot["gpus"].toArray().first().toObject()["busy_percent"].toDouble(), 100.0);
        QCOMPARE(snapshot["apps"].toArray().first().toObject()["gpu_percent"].toDouble(), 100.0);
    }

    void namesArmMachines()
    {
        QTemporaryDir root;
        const QString proc = root.filePath("proc"), sys = root.filePath("sys");
        write(proc + "/meminfo", "MemTotal: 1000 kB\nMemAvailable: 600 kB\n");
        write(proc + "/stat", "cpu  0 0 0 0 0 0 0 0\ncpu0 0 0 0 0 0 0 0 0\n");
        write(proc + "/cpuinfo", "processor\t: 0\nBogoMIPS\t: 48.00\nCPU implementer\t: 0x61\n");
        write(sys + "/firmware/devicetree/base/model", QByteArray("Apple MacBook Air (13-inch, M2, 2022)\0", 38));
        write(sys + "/class/thermal/thermal_zone0/type", "battery\n");
        write(sys + "/class/thermal/thermal_zone1/type", "cpu0-thermal\n");
        write(sys + "/class/thermal/thermal_zone1/temp", "47500\n");
        Monitor apple(proc, sys);
        QCOMPARE(apple.cpuModel(), QString("Apple MacBook Air (13-inch, M2, 2022)"));
        QCOMPARE(apple.cpuTemperature(), 47.5);

        QFile::remove(sys + "/firmware/devicetree/base/model");
        write(sys + "/devices/soc0/family", "Snapdragon\n");
        write(sys + "/devices/soc0/machine", "X1E80100\n");
        Monitor snapdragon(proc, sys);
        QCOMPARE(snapdragon.cpuModel(), QString("Snapdragon X1E80100"));
    }

    void linksAndRemovesTheSkill()
    {
        QTemporaryDir dir;
        const QString home = dir.path() + "/home", packaged = dir.path() + "/usr/share/montop/skills/montop";
        const QByteArray text = "---\nname: montop\ndescription: test\n---\n";
        QDir().mkpath(home + "/.claude");
        QDir().mkpath(home + "/.codex/skills/montop");  // The user's own skill by that name.
        write(home + "/.codex/skills/montop/SKILL.md", "---\nname: mine\n---\n");
        QDir().mkpath(packaged);
        write(packaged + "/SKILL.md", text);

        // Packaged: links in the shared folder and each agent that is set up.
        SkillChanges changes = installSkill(home, packaged, text);
        QCOMPARE(QFileInfo(home + "/.agents/skills/montop").symLinkTarget(), packaged);
        QCOMPARE(QFileInfo(home + "/.claude/skills/montop").symLinkTarget(), packaged);
        QVERIFY(!QFileInfo(home + "/.codex/skills/montop").isSymLink());
        QVERIFY(!QFileInfo::exists(home + "/.pi"));
        QCOMPARE(changes.done.size(), 2);
        QCOMPARE(changes.errors.size(), 1);
        QVERIFY(installSkill(home, packaged, text).done.isEmpty());

        // Removing takes out only Montop' links.
        changes = removeSkill(home, packaged);
        QCOMPARE(changes.done.size(), 2);
        QVERIFY(!QFileInfo(home + "/.claude/skills/montop").isSymLink());
        QVERIFY(QFileInfo::exists(home + "/.codex/skills/montop/SKILL.md"));
        QDir(home + "/.codex").removeRecursively();

        // From source: a copy, linked to; and 0.3.0's relative link is taken over.
        QDir().mkpath(home + "/.claude/skills");
        QVERIFY(QFile::link("../../.agents/skills/montop", home + "/.claude/skills/montop"));
        changes = installSkill(home, dir.path() + "/nowhere", text);
        QVERIFY(changes.errors.isEmpty());
        QVERIFY(QFileInfo(home + "/.agents/skills/montop").isDir());
        QVERIFY(!QFileInfo(home + "/.agents/skills/montop").isSymLink());
        QCOMPARE(QFileInfo(home + "/.claude/skills/montop").symLinkTarget(), home + "/.agents/skills/montop");
        changes = removeSkill(home, packaged);
        QCOMPARE(changes.done.size(), 2);
        QVERIFY(!QFileInfo::exists(home + "/.agents/skills/montop"));
        QVERIFY(!QFileInfo(home + "/.claude/skills/montop").isSymLink());

        // A link left dangling by an uninstalled package is cleaned up.
        QVERIFY(QFile::link(dir.path() + "/gone", home + "/.claude/skills/montop"));
        QCOMPARE(removeSkill(home, packaged).done.size(), 1);

        // Links from when Montop was called Vitals go; a "vitals" of anyone
        // else's stays.
        QVERIFY(QFile::link("/usr/share/vitals/skills/vitals", home + "/.claude/skills/vitals"));
        QVERIFY(QFile::link(dir.path() + "/someone-elses", home + "/.agents/skills/vitals"));
        QCOMPARE(removeSkill(home, packaged).done.size(), 1);
        QVERIFY(!QFileInfo(home + "/.claude/skills/vitals").isSymLink());
        QVERIFY(QFileInfo(home + "/.agents/skills/vitals").isSymLink());
    }

    void readsWhatACgroupUsed()
    {
        QTemporaryDir dir;
        const QString cg = dir.path();
        write(cg + "/cpu.stat", "usage_usec 2500000\nuser_usec 2000000\nsystem_usec 500000\n");
        write(cg + "/memory.current", "4096000\n");
        write(cg + "/memory.peak", "8192000\n");
        write(cg + "/memory.stat", "anon 1000\nfile 999999\nshmem 24\nkernel 2000\n");
        write(cg + "/io.stat", "8:0 rbytes=100 wbytes=200 rios=1\n253:0 rbytes=100 wbytes=200\n");
        write(cg + "/cpu.pressure", "some avg10=0.00 avg60=0.00 avg300=0.00 total=1500000\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
        write(cg + "/io.pressure", "some avg10=0.00 avg60=0.00 avg300=0.00 total=250000\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
        write(cg + "/pids.peak", "12\n");

        const CgroupUsage u = readCgroupUsage(cg, {"8:0"});
        QVERIFY(u.ok);
        QVERIFY(u.hasIo);
        QCOMPARE(u.cpuUsec, quint64(2500000));
        QCOMPARE(u.userUsec, quint64(2000000));
        QCOMPARE(u.ownMemory, quint64(3024)); // anon + shmem + kernel
        QCOMPARE(u.memoryPeak, quint64(8192000));
        QCOMPARE(u.readBytes, quint64(100)); // the disk, not the volume on it too
        QCOMPARE(u.writeBytes, quint64(200));
        QCOMPARE(u.cpuWaitUsec, quint64(1500000));
        QCOMPARE(u.ioWaitUsec, quint64(250000));
        QCOMPARE(u.memoryWaitUsec, quint64(0));
        QCOMPARE(u.processesPeak, 12);
        QVERIFY(!readCgroupUsage(cg + "/gone", {}).ok);

        RunReport report;
        report.command = {"make", "-j8"};
        report.exitCode = 2;
        report.seconds = 1.25;
        report.usage = u;
        report.ownMemoryPeak = 3024;
        report.cpuPeak = 350;
        const QJsonObject json = runJson(report);
        QCOMPARE(json["exit_code"].toInt(), 2);
        QCOMPARE(json["cpu"].toObject()["seconds"].toDouble(), 2.5);
        QCOMPARE(json["cpu"].toObject()["average_percent"].toDouble(), 200.0);
        QCOMPARE(json["memory"].toObject()["peak_with_cache_bytes"].toDouble(), 8192000.0);
        QCOMPARE(json["waited_seconds"].toObject()["cpu"].toDouble(), 1.5);
        QVERIFY(runText(report).contains("Exit       2"));
    }

    void monitorSaysWhenProcIsMissing()
    {
        QTemporaryDir root;
        Monitor monitor(root.filePath("nothing"), root.filePath("nothing"));
        QVERIFY(!monitor.error().isEmpty());
    }

    void formatsBytes()
    {
        QLocale::setDefault(QLocale::c());
        QCOMPARE(Monitor::bytes(512), QString("512 B"));
        QCOMPARE(Monitor::bytes(1536), QString("1.50 KiB"));
        QCOMPARE(Monitor::rate(10.0 * 1024 * 1024), QString("10.0 MiB/s"));
    }

    void sparklineScalesInSteadySteps()
    {
        QCOMPARE(Sparkline::autoMaximum({}, {}), 20000.0); // idle floor, rounded up
        QCOMPARE(Sparkline::autoMaximum({3e6}, {1e6}), 5e6);
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
        QVERIFY(theme.dark());
        for (const char *key : {"background", "foreground", "red", "green", "yellow", "orange", "selection", "muted",
                                "dark_foreground", "lighter_background", "bright_foreground"})
            QVERIFY2(QColor::isValidColorName(theme.colors().value(key).toString()), key);
    }

    void themeReadsThePalette()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml",
              "mode = \"light\"\naccent = \"#112233\"\nbackground = \"#ffffff\"\nred = '#ff0000'\n");

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
        QVERIFY(!theme.dark());
        QCOMPARE(theme.colors().value("red").toString(), QString("#ff0000"));
        QCOMPARE(theme.colors().value("background").toString(), QString("#ffffff"));
    }

    void themeFollowsASwitch()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n");
        Theme theme(dir.path());
        QFile::remove(dir.path() + "/theme/colors.toml");
        write(dir.path() + "/theme/colors.toml", "accent = \"#445566\"\n");
        QTRY_COMPARE(theme.accent(), QString("#445566"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        write(response, "{\"int\":6}");
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorFollowsTheLanguageFile()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Quit": "Sair", "Cancel": "Cancelar"})");
        write(dir.path() + "/pt-BR.json", R"({"Quit": "Fechar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Quit"), QString("Fechar"));
        QCOMPARE(translator.translate("Main", "Cancel"), QString("Cancelar"));
        QVERIFY(translator.translate("Main", "Kill").isNull());
    }

    void shippedCatalogsCoverEveryString()
    {
        // Each shipped language must translate every non-plural string the
        // Dutch catalog does, so a half-done catalog shows up here.
        Translator translator(QString(), ":/i18n");
        QFile nl(":/i18n/nl.json");
        QVERIFY(nl.open(QIODevice::ReadOnly));
        const QJsonObject strings = QJsonDocument::fromJson(nl.readAll()).object();
        QVERIFY(strings.size() > 40);
        for (auto it = strings.begin(); it != strings.end(); ++it)
            QVERIFY2(it.value().isObject() || !it.value().toString().isEmpty(), qPrintable(it.key()));
    }

private:
    static ProcessInfo process(int pid, const QString &name, double cpu, bool kernel = false)
    {
        ProcessInfo p;
        p.pid = pid;
        p.name = name;
        p.command = name;
        p.cpu = cpu;
        p.kernel = kernel;
        return p;
    }

    static GroupInfo group(const QString &key, const QString &name, const QList<int> &pids, double cpu,
                           const QString &kind = "app")
    {
        GroupInfo g;
        g.key = key;
        g.name = name;
        g.kind = kind;
        g.pids = pids;
        g.cpu = cpu;
        return g;
    }

    static QByteArray pidStat(int pid, const char *comm, int ticks)
    {
        return QByteArray::number(pid) + " (" + comm + ") R 1 1 1 0 -1 0 0 0 0 0 " + QByteArray::number(ticks)
               + " 0 0 0 20 0 1 0 100 1000 10";
    }

    // A GPU in a stand-in sysfs: its device directory, and the card and
    // render nodes that point at it.
    static QString gpuDevice(const QString &sys, const QString &device, int card, int render, const QByteArray &uevent)
    {
        const QString path = sys + "/devices/" + device;
        write(path + "/uevent", uevent);
        QDir().mkpath(sys + QString("/class/drm/card%1").arg(card));
        QFile::link(path, sys + QString("/class/drm/card%1/device").arg(card));
        if (render > 0) {
            QDir().mkpath(sys + QString("/class/drm/renderD%1").arg(render));
            QFile::link(path, sys + QString("/class/drm/renderD%1/device").arg(render));
        }
        return path;
    }

    static void write(const QString &path, const QByteArray &contents)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(contents);
    }
};

QTEST_MAIN(MontopTests)
#include "montop_tests.moc"
