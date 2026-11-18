#include "monitor.h"

#include <QDateTime>
#include <QDir>
#include <QMap>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QSysInfo>
#include <algorithm>

#include <cerrno>
#include <dirent.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

using namespace procfs;

static QString readLine(const QString &path) {
    return QString::fromUtf8(readFile(path, 4096)).trimmed();
}

Monitor::Monitor(const QString &procRoot, const QString &sysRoot, QObject *parent)
    : QObject(parent), m_proc(procRoot), m_sys(sysRoot) {
    // For screenshots and demos: a made-up system instead of this one
    // (tools/demo writes one), so nothing personal ends up shown.
    const QString root = qEnvironmentVariable("MONTOP_ROOT");
    if (!root.isEmpty() && procRoot == "/proc" && sysRoot == "/sys") {
        m_proc = root + "/proc";
        m_sys = root + "/sys";
    }
    m_dev = m_proc == "/proc" ? QString("/dev") : m_proc + "/../dev";
    m_pageSize = sysconf(_SC_PAGESIZE);
    m_gpus = std::make_unique<gpu::Gpus>(m_sys);
    // From /proc, so a stand-in root under test brings its own.
    m_hostname = readLine(m_proc + "/sys/kernel/hostname");
    if (m_hostname.isEmpty())
        m_hostname = QSysInfo::machineHostName();
    m_kernel = readLine(m_proc + "/sys/kernel/osrelease");
    if (m_kernel.isEmpty())
        m_kernel = QSysInfo::kernelVersion();
    // A stand-in root can't own its files as other users, so it says who
    // owns each process in its status file, and names them in its own passwd.
    if (m_proc != "/proc") {
        for (const QByteArray &line : readFile(m_proc + "/../etc/passwd").split('\n')) {
            const QList<QByteArray> f = line.split(':');
            if (f.size() > 2)
                m_users.insert(f[2].toUInt(), QString::fromUtf8(f[0]));
        }
    }
    for (const QByteArray &line : readFile(m_proc + "/cpuinfo").split('\n')) {
        if (line.startsWith("model name")) {
            m_cpuModel = QString::fromUtf8(line.mid(line.indexOf(':') + 1)).simplified();
            // "Intel(R) Core(TM) Ultra 7 155H" reads better without the marks.
            m_cpuModel.remove("(R)").remove("(TM)").replace(" CPU", "");
            m_cpuModel = m_cpuModel.simplified();
            break;
        }
    }
    // Arm has no model name in cpuinfo. The device tree names the machine
    // ("Apple MacBook Air (13-inch, M2, 2022)"); the SoC driver names the
    // chip ("Snapdragon X1E80100").
    if (m_cpuModel.isEmpty())
        m_cpuModel = QString::fromUtf8(readFile(m_sys + "/firmware/devicetree/base/model", 256)).remove(QChar(0)).trimmed();
    if (m_cpuModel.isEmpty()) {
        // Firmware-reported IDs ("jep106:0070:...") are codes, not names.
        auto name = [&](const char *field) {
            const QString value = readLine(m_sys + "/devices/soc0/" + field);
            return value.startsWith("jep106") ? QString() : value;
        };
        m_cpuModel = (name("family") + ' ' + name("machine")).trimmed();
    }
    m_temperatureInput = findTemperatureInput();
    m_uid = getuid();
    // Always the real /proc: it's this process, not the one under test.
    for (const QByteArray &line : readFile("/proc/self/cgroup", 4096).split('\n'))
        if (line.startsWith("0::"))
            m_selfCgroup = QString::fromUtf8(line.mid(3)).trimmed();

    m_clock.start();
    // Graphs scroll between samples, so a late tick shows as a stutter; a
    // precise timer keeps the beat even.
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(250);
    connect(&m_timer, &QTimer::timeout, this, &Monitor::tick);

    sample();
}

void Monitor::start() {
    // Rates need two samples; the constructor took the first, and this takes
    // the second soon rather than showing zeros for a whole interval.
    QTimer::singleShot(250, this, [this] {
        if (!m_paused)
            tick();
    });
    m_timer.start();
}

void Monitor::setInterval(int ms) {
    ms = std::clamp(ms, 125, 4000);
    if (ms == m_timer.interval())
        return;
    m_timer.setInterval(ms);
    emit intervalChanged();
}

void Monitor::faster() {
    setInterval(interval() / 2);
}

void Monitor::slower() {
    setInterval(interval() * 2);
}

void Monitor::setPaused(bool paused) {
    if (paused == m_paused)
        return;
    m_paused = paused;
    if (m_paused) {
        m_timer.stop();
    } else {
        // The first sample after a pause would spread the paused time over
        // one interval; resetting the baseline keeps the rates honest.
        m_lastSample = m_lastProcessSample = -1;
        sample();
        m_timer.start();
    }
    emit pausedChanged();
}

QVariantMap Monitor::signalProcesses(const QVariantList &pids, bool force) {
    int sent = 0, failed = 0, error = 0;
    for (const QVariant &pid : pids) {
        const int result = signalProcess(pid.toInt(), force);
        if (result == 0) {
            ++sent;
        } else if (result != ESRCH) { // Already gone is as good as ended.
            ++failed;
            if (!error)
                error = result;
        }
    }
    return {{"sent", sent}, {"failed", failed}, {"error", error}};
}

int Monitor::signalProcess(int pid, bool force) {
    if (pid <= 1)
        return EPERM;
    return ::kill(pid, force ? SIGKILL : SIGTERM) == 0 ? 0 : errno;
}

QString Monitor::bytes(double value) {
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    int unit = 0;
    while (value >= 1024 && unit < 5) {
        value /= 1024;
        ++unit;
    }
    const int precision = unit == 0 || value >= 100 ? 0 : value >= 10 ? 1 : 2;
    return QLocale().toString(value, 'f', precision) + QChar(0x202F) + QLatin1String(units[unit]);
}

QString Monitor::rate(double bytesPerSecond) {
    return bytes(bytesPerSecond) + "/s";
}

void Monitor::push(const QString &series, qreal value) {
    QList<qreal> &history = m_history[series];
    history.append(value);
    if (history.size() > HistoryLength)
        history.remove(0, history.size() - HistoryLength);
}

int Monitor::processInterval() const {
    // Scanning every process costs far more than the system counters and a
    // table rewritten four times a second can't be read anyway, so the
    // process list keeps a calmer pace of its own.
    return std::max(1000, interval() * 2);
}

void Monitor::sample() {
    sampleSystem();
    sampleProcessTable();
    emit sampled();
}

void Monitor::tick() {
    sampleSystem();
    // Half an interval of slack, so timer jitter doesn't skip a beat.
    if (m_lastProcessSample < 0 || m_clock.elapsed() - m_lastProcessSample >= processInterval() - interval() / 2)
        sampleProcessTable();
    emit sampled();
}

void Monitor::sampleSystem() {
    const qint64 now = m_clock.elapsed();
    const double seconds = m_lastSample < 0 ? 0 : (now - m_lastSample) / 1000.0;
    m_lastSample = now;
    m_sampleTimes.append(QDateTime::currentMSecsSinceEpoch());
    if (m_sampleTimes.size() > HistoryLength)
        m_sampleTimes.remove(0, m_sampleTimes.size() - HistoryLength);

    sampleCpu();
    sampleMemory();
    sampleDisks(seconds);
    sampleNetworks(seconds);
    sampleGpus();
}

void Monitor::sampleGpus() {
    m_gpus->sample(m_clock.elapsed());
    const QList<gpu::Device> &devices = m_gpus->devices();
    for (int i = 0; i < devices.size(); ++i)
        push(QString("gpu/%1").arg(i), std::max(0.0, devices[i].busy));
}

QVariantList Monitor::gpus() const {
    QVariantList list;
    for (const gpu::Device &d : m_gpus->devices())
        list.append(QVariantMap{
            {"name", d.name}, {"driver", d.driver}, {"asleep", d.asleep}, {"perApp", d.perApp}, {"busy", d.busy},
            {"memoryUsed", d.memoryUsed}, {"memoryTotal", d.memoryTotal}, {"appMemory", d.appMemory},
            {"mhz", d.mhz}, {"temperature", d.temperature}, {"watts", d.watts}, {"fan", d.fan}, {"note", d.note}});
    return list;
}

void Monitor::sampleGpuClients(const QString &base, ProcessInfo &p, bool known, double seconds,
                               QHash<QString, QHash<QString, double>> &shares, QHash<QString, quint64> &memory,
                               QHash<QString, gpu::Client> &clients) {
    auto cached = m_gpuFds.find(p.pid);
    if (!known || cached == m_gpuFds.end())
        cached = m_gpuFds.insert(p.pid, {});
    // A process opens the GPU as it starts, so it's listed on each of its
    // first scans; after that every 15th scan if it uses the GPU, else every
    // 30th, spread over the PIDs so no one scan lists them all.
    const int every = cached->fds.isEmpty() ? 30 : 15;
    if (cached->listed < 3 || (p.pid + m_scanCount) % every == 0) {
        QList<QPair<int, QString>> fds;
        const QByteArray fdDir = QFile::encodeName(base + "/fd");
        if (DIR *dir = opendir(fdDir.constData())) {
            char target[256];
            while (dirent *entry = readdir(dir)) {
                if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
                    continue;
                const QByteArray link = fdDir + '/' + entry->d_name;
                const ssize_t n = ::readlink(link.constData(), target, sizeof target - 1);
                if (n > 9 && memcmp(target, "/dev/dri/", 9) == 0)
                    fds.append({atoi(entry->d_name), QString::fromLatin1(target + 9, int(n - 9))});
            }
            closedir(dir);
        }
        cached->fds = fds;
        ++cached->listed;
    }
    for (const auto &[fd, node] : std::as_const(cached->fds)) {
        const QString device = m_gpus->deviceOf(node);
        gpu::Client client;
        if (device.isEmpty() || !gpu::parseFdinfo(readFile(base + "/fdinfo/" + QString::number(fd), 8192), client))
            continue;
        // One client reached through dup'd or inherited fds counts once.
        const QString key = device + '#' + (client.id.isEmpty() ? QString("%1:%2").arg(p.pid).arg(fd) : client.id);
        if (clients.contains(key))
            continue;
        clients.insert(key, client);
        const auto before = m_prevClients.constFind(key);
        if (before != m_prevClients.constEnd()) {
            const QHash<QString, double> engines = gpu::engineShares(*before, client, seconds);
            QHash<QString, double> &deviceShares = shares[device];
            double busiest = 0;
            for (auto it = engines.begin(); it != engines.end(); ++it) {
                busiest = std::max(busiest, it.value());
                deviceShares[it.key()] += it.value();
            }
            p.gpu += 100 * busiest;
        }
        p.gpuMemory += client.memory;
        memory[device] += client.memory;
    }
}

void Monitor::sampleProcessTable() {
    const qint64 now = m_clock.elapsed();
    const double seconds = m_lastProcessSample < 0 ? 0 : (now - m_lastProcessSample) / 1000.0;
    m_lastProcessSample = now;

    // The process scan runs at its own pace, so it measures CPU time against
    // its own previous reading, not the last system sample's.
    const quint64 total = m_prevCpu.isEmpty() ? 0 : m_prevCpu[0].total;
    const quint64 delta = seconds > 0 && total > m_processCpuTotal ? total - m_processCpuTotal : 0;
    m_processCpuTotal = total;

    sampleFilesystems();
    sampleProcesses(seconds, delta);
}

void Monitor::sampleCpu() {
    const QList<CpuTimes> times = parseStat(readFile(m_proc + "/stat"));
    if (times.isEmpty()) {
        m_error = tr("Can't read %1. Montop needs the proc filesystem to see the system.").arg(m_proc + "/stat");
        return;
    }
    m_error.clear();

    auto usage = [](const CpuTimes &now, const CpuTimes &before) {
        const quint64 total = now.total - before.total;
        return total == 0 ? 0.0 : double(now.busy - before.busy) / double(total);
    };

    const bool hasPrevious = m_prevCpu.size() == times.size();
    m_cpuUsage = hasPrevious ? usage(times[0], m_prevCpu[0]) : 0;
    push("cpu", m_cpuUsage);

    QVariantList cores;
    double frequencySum = 0;
    int frequencyCount = 0;
    for (int i = 1; i < times.size(); ++i) {
        const double mhz = readLine(QString("%1/devices/system/cpu/cpu%2/cpufreq/scaling_cur_freq").arg(m_sys).arg(i - 1))
                               .toDouble() / 1000.0;
        if (mhz > 0) {
            frequencySum += mhz;
            ++frequencyCount;
        }
        cores.append(QVariantMap{{"usage", hasPrevious ? usage(times[i], m_prevCpu[i]) : 0.0}, {"mhz", mhz}});
    }
    m_cores = cores;
    m_cpuFrequency = frequencyCount ? frequencySum / frequencyCount : 0;
    m_prevCpu = times;

    m_cpuTemperature = m_temperatureInput.isEmpty() ? -1 : readLine(m_temperatureInput).toDouble() / 1000.0;

    const QStringList load = readLine(m_proc + "/loadavg").split(' ');
    m_loadAverage = {load.value(0).toDouble(), load.value(1).toDouble(), load.value(2).toDouble()};
    m_uptime = qint64(readLine(m_proc + "/uptime").section(' ', 0, 0).toDouble());
}

// The package temperature where the driver reports one, so the number
// matches what the fans react to; otherwise the first CPU sensor found.
QString Monitor::findTemperatureInput() const {
    static const QStringList drivers{"coretemp", "k10temp", "zenpower", "cpu_thermal", "acpitz"};
    static const QStringList labels{"Package id 0", "Tctl", "Tdie"};
    const QDir hwmon(m_sys + "/class/hwmon");
    for (const QString &driver : drivers) {
        for (const QString &entry : hwmon.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System)) {
            const QString base = hwmon.filePath(entry);
            if (readLine(base + "/name") != driver)
                continue;
            for (int i = 1; i < 64; ++i) {
                const QString input = QString("%1/temp%2_input").arg(base).arg(i);
                if (QFileInfo::exists(input) && labels.contains(readLine(QString("%1/temp%2_label").arg(base).arg(i))))
                    return input;
            }
            if (QFileInfo::exists(base + "/temp1_input"))
                return base + "/temp1_input";
        }
    }
    // Arm SoCs report through thermal zones instead: "cpu0-thermal",
    // "cpu-0-0-top-thermal", "cpuss0-thermal". (Apple's chips report no CPU
    // temperature on Linux at all.)
    const QDir thermal(m_sys + "/class/thermal");
    QStringList zones = thermal.entryList({"thermal_zone*"}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::System);
    std::sort(zones.begin(), zones.end(), [](const QString &a, const QString &b) { return a.mid(12).toInt() < b.mid(12).toInt(); });
    for (const QString &prefix : {QString("x86_pkg_temp"), QString("cpu"), QString("soc")})
        for (const QString &zone : zones)
            if (readLine(thermal.filePath(zone) + "/type").startsWith(prefix) && QFileInfo::exists(thermal.filePath(zone) + "/temp"))
                return thermal.filePath(zone) + "/temp";
    return {};
}

void Monitor::sampleMemory() {
    const auto info = parseMeminfo(readFile(m_proc + "/meminfo"));
    m_memTotal = info.value("MemTotal");
    m_memFree = info.value("MemFree");
    // MemAvailable is the kernel's own estimate of what can be handed out
    // without swapping; "used" is everything else, as free(1) reports it.
    m_memAvailable = info.value("MemAvailable", info.value("MemFree"));
    m_memUsed = std::max(0.0, m_memTotal - m_memAvailable);
    m_memCached = info.value("Cached") + info.value("Buffers") + info.value("SReclaimable");
    m_swapTotal = info.value("SwapTotal");
    m_swapUsed = std::max(0.0, m_swapTotal - double(info.value("SwapFree")));
    push("memory", m_memTotal > 0 ? m_memUsed / m_memTotal : 0);
}

void Monitor::sampleDisks(double seconds) {
    // Whole physical devices only: /sys/block lists disks, not partitions.
    // Loop and RAM devices and empty slots are noise, and device-mapper
    // volumes (LUKS, LVM) repeat the traffic of the disk underneath them.
    const QDir block(m_sys + "/block");
    const QStringList devices = block.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System);
    QHash<QString, DiskCounters> counters;
    for (const DiskCounters &d : parseDiskstats(readFile(m_proc + "/diskstats")))
        counters.insert(d.name, d);

    QVariantList disks;
    QHash<QString, DiskCounters> previous;
    QSet<QString> physical;
    for (const QString &name : devices) {
        if (name.startsWith("loop") || name.startsWith("ram") || name.startsWith("zram") || name.startsWith("dm-")
            || !counters.contains(name))
            continue;
        if (readLine(block.filePath(name + "/size")).toULongLong() == 0 || appleInternal(name))
            continue;
        const DiskCounters now = counters.value(name);
        previous.insert(name, now);
        physical.insert(readLine(block.filePath(name + "/dev")));
        double readRate = 0, writeRate = 0, busy = 0;
        if (seconds > 0 && m_prevDisks.contains(name)) {
            const DiskCounters before = m_prevDisks.value(name);
            readRate = (now.readBytes - before.readBytes) / seconds;
            writeRate = (now.writeBytes - before.writeBytes) / seconds;
            busy = std::min(1.0, (now.busyMs - before.busyMs) / (seconds * 1000));
        }
        push("disk/" + name + "/read", readRate);
        push("disk/" + name + "/write", writeRate);
        // The model says more than "nvme0n1" to most people.
        const QString model = readLine(block.filePath(name + "/device/model"));
        disks.append(QVariantMap{
            {"id", diskId(name)},
            {"name", name},
            {"model", model},
            {"readRate", readRate},
            {"writeRate", writeRate},
            {"busy", busy},
        });
    }
    // A name gone may come back as another disk.
    for (auto it = m_diskIds.begin(); it != m_diskIds.end();)
        it = previous.contains(it.key()) ? std::next(it) : m_diskIds.erase(it);
    for (auto it = m_appleController.begin(); it != m_appleController.end();)
        it = counters.contains(it.key()) ? std::next(it) : m_appleController.erase(it);
    m_prevDisks = previous;
    m_physicalDevices = physical;
    m_disks = disks;
}

// A disk's own id: its WWID (NVMe keeps it beside the disk, SCSI and SATA
// on the device), or else the name udev gives it in /dev/disk/by-id, which
// carries the serial number of a USB drive, or else its kernel name.
QString Monitor::diskId(const QString &name) {
    const auto known = m_diskIds.constFind(name);
    if (known != m_diskIds.constEnd())
        return *known;
    QString id = readLine(m_sys + "/block/" + name + "/wwid");
    if (id.isEmpty())
        id = readLine(m_sys + "/block/" + name + "/device/wwid");
    if (id.isEmpty()) {
        const QDir byId(m_dev + "/disk/by-id");
        for (const QString &link : byId.entryList(QDir::System | QDir::Files | QDir::NoDotAndDotDot, QDir::Name)) {
            if (link.contains("-part") || link.startsWith("wwn-") || link.startsWith("nvme-eui."))
                continue;
            if (QFileInfo(QFileInfo(byId.filePath(link)).symLinkTarget()).fileName() == name) {
                id = link;
                break;
            }
        }
    }
    if (id.isEmpty())
        id = name;
    m_diskIds.insert(name, id);
    return id;
}

// Apple's SSD controller (Apple Silicon Macs on Asahi Linux) shows the
// macOS side's internal namespaces as disks of their own: nvme0n2, nvme0n3.
// Only namespace 1 is the disk Linux uses; another is left out unless it
// has partitions or is in use (mounted, or under LUKS, LVM or RAID).
bool Monitor::appleInternal(const QString &name) {
    if (!name.startsWith("nvme"))
        return false;
    auto known = m_appleController.constFind(name);
    if (known == m_appleController.constEnd()) {
        bool apple = false;
        for (const QByteArray &line : readFile(m_sys + "/block/" + name + "/device/device/uevent").split('\n')) {
            if (line == "DRIVER=apple-nvme" || line == "DRIVER=nvme-apple"
                || (line.startsWith("OF_COMPATIBLE_") && line.endsWith("nvme-ans2")))
                apple = true;
        }
        known = m_appleController.insert(name, apple);
    }
    if (!*known)
        return false;
    const QString nsid = readLine(m_sys + "/block/" + name + "/nsid");
    if (nsid.isEmpty() || nsid == "1")
        return false;
    const QDir disk(m_sys + "/block/" + name);
    if (!disk.entryList({name + "p*"}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty())
        return false;
    if (!QDir(disk.filePath("holders")).entryList(QDir::Dirs | QDir::Files | QDir::System | QDir::NoDotAndDotDot).isEmpty())
        return false;
    return !readFile(m_proc + "/self/mounts").contains("/dev/" + name.toLatin1() + ' ');
}

// The block device a mount names: "/dev/mapper/root" is "dm-0".
QString Monitor::blockName(const QString &device) const {
    if (!device.startsWith("/dev/"))
        return QString();
    const QFileInfo node(m_dev + device.mid(4));
    return node.isSymLink() ? QFileInfo(node.symLinkTarget()).fileName() : node.fileName();
}

void Monitor::sampleFilesystems() {
    QVariantList filesystems;
    const QByteArray mounts = readFile(m_proc + "/self/mounts");
    // A filesystem's id is its UUID, so hiding it holds whichever of its
    // mount points (btrfs subvolumes, bind mounts) the row is named after.
    // Matched again only when something is mounted or unmounted.
    if (mounts != m_mountsText) {
        m_mountsText = mounts;
        m_uuidOf.clear();
        const QDir byUuid(m_dev + "/disk/by-uuid");
        for (const QString &uuid : byUuid.entryList(QDir::System | QDir::Files | QDir::NoDotAndDotDot))
            m_uuidOf.insert(QFileInfo(QFileInfo(byUuid.filePath(uuid)).symLinkTarget()).fileName(), uuid);
    }
    for (const Mount &mount : parseMounts(mounts)) {
        struct statvfs fs;
        if (statvfs(QFile::encodeName(mount.path).constData(), &fs) != 0 || fs.f_blocks == 0)
            continue;
        const double total = double(fs.f_blocks) * fs.f_frsize;
        const double available = double(fs.f_bavail) * fs.f_frsize;
        const double used = total - double(fs.f_bfree) * fs.f_frsize;
        filesystems.append(QVariantMap{
            {"id", m_uuidOf.value(blockName(mount.device), mount.device)},
            {"path", mount.path}, {"device", mount.device}, {"type", mount.type},
            {"total", total}, {"used", used}, {"available", available},
            {"share", total > 0 ? used / (used + available) : 0.0},
        });
    }
    m_filesystems = filesystems;
}

void Monitor::sampleNetworks(double seconds) {
    struct Row { QVariantMap map; bool up; double total; };
    QList<Row> rows;
    QHash<QString, NetCounters> previous;
    for (const NetCounters &now : parseNetDev(readFile(m_proc + "/net/dev"))) {
        if (now.name == "lo" || now.name.startsWith("veth"))
            continue;
        previous.insert(now.name, now);
        double rx = 0, tx = 0;
        if (seconds > 0 && m_prevNet.contains(now.name)) {
            const NetCounters before = m_prevNet.value(now.name);
            rx = (now.rxBytes - before.rxBytes) / seconds;
            tx = (now.txBytes - before.txBytes) / seconds;
        }
        push("net/" + now.name + "/rx", rx);
        push("net/" + now.name + "/tx", tx);
        const bool up = readLine(QString("%1/class/net/%2/operstate").arg(m_sys, now.name)) == "up";
        if (!up && now.rxBytes + now.txBytes == 0)
            continue;
        rows.append({QVariantMap{
                         {"name", now.name}, {"up", up},
                         {"rxRate", rx}, {"txRate", tx},
                         {"rxTotal", double(now.rxBytes)}, {"txTotal", double(now.txBytes)},
                     },
                     up, double(now.rxBytes + now.txBytes)});
    }
    // Connected interfaces first, busiest on top.
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        return a.up != b.up ? a.up : a.total > b.total;
    });
    QVariantList networks;
    for (const Row &row : rows)
        networks.append(row.map);
    m_prevNet = previous;
    m_networks = networks;
}

QString Monitor::userName(uint uid) {
    const auto cached = m_users.constFind(uid);
    if (cached != m_users.constEnd())
        return *cached;
    struct passwd pwd, *result = nullptr;
    char buffer[4096];
    QString name = getpwuid_r(uid, &pwd, buffer, sizeof buffer, &result) == 0 && result
                       ? QString::fromLocal8Bit(pwd.pw_name) : QString::number(uid);
    m_users.insert(uid, name);
    return name;
}

void Monitor::sampleProcesses(double seconds, quint64 cpuTotalDelta) {
    DIR *dir = opendir(QFile::encodeName(m_proc).constData());
    if (!dir)
        return;

    // A process's share is measured against one core's worth of ticks, so a
    // process saturating two cores shows 200 %, as in top and htop.
    const int cores = std::max(1, int(m_cores.size()));
    const double ticksPerCore = cpuTotalDelta > 0 ? double(cpuTotalDelta) / cores : 0;

    QList<ProcessInfo> processes;
    QHash<int, PidHistory> seen;
    m_cgroupOf.clear();
    int tasks = 0;
    QHash<QString, QHash<QString, double>> gpuShares;
    QHash<QString, quint64> gpuMemory;
    QHash<QString, gpu::Client> gpuClients;
    while (dirent *entry = readdir(dir)) {
        if (entry->d_name[0] < '1' || entry->d_name[0] > '9')
            continue;
        const QString base = m_proc + '/' + QLatin1String(entry->d_name);
        PidStat stat;
        if (!parsePidStat(readFile(base + "/stat", 4096), stat))
            continue; // Exited between readdir() and here.

        ProcessInfo p;
        p.pid = stat.pid;
        p.ppid = stat.ppid;
        p.state = stat.state;
        p.threads = stat.threads;
        p.kernel = stat.pid == 2 || stat.ppid == 2;
        p.memory = stat.rssPages * quint64(m_pageSize);
        p.memoryShare = m_memTotal > 0 ? p.memory / m_memTotal : 0;
        tasks += std::max(1, stat.threads);

        struct stat info;
        if (m_proc != "/proc") {
            for (const QByteArray &line : readFile(base + "/status", 4096).split('\n'))
                if (line.startsWith("Uid:"))
                    p.user = userName(line.mid(4).simplified().split(' ').value(0).toUInt());
        } else if (::stat(QFile::encodeName(base).constData(), &info) == 0) {
            p.user = userName(info.st_uid);
        }

        PidHistory history;
        const auto before = m_prevPids.constFind(p.pid);
        const bool known = before != m_prevPids.constEnd() && before->comm == stat.comm;
        history.ticks = stat.cpuTicks;
        history.comm = stat.comm;

        // The command line rarely changes once a process has exec'd, and
        // exec changes the comm too, so it's read again only then.
        if (known) {
            history.command = before->command;
            history.name = before->name;
        } else {
            history.command = parseCmdline(readFile(base + "/cmdline", 8192));
            // comm is cut at 15 characters; argv[0] usually has the whole name.
            const QString argv0 = QFileInfo(history.command.section(' ', 0, 0)).fileName();
            history.name = !argv0.isEmpty() && argv0.startsWith(stat.comm) ? argv0 : stat.comm;
        }
        // Which app it belongs to. Processes rarely change cgroup after
        // they start, so this is read once per process too.
        if (known) {
            history.cgroup = before->cgroup;
        } else {
            for (const QByteArray &line : readFile(base + "/cgroup", 4096).split('\n'))
                if (line.startsWith("0::"))
                    history.cgroup = QString::fromUtf8(line.mid(3)).trimmed();
        }
        m_cgroupOf.insert(p.pid, history.cgroup);
        // Without a readable cgroup (or with cgroups v1), a process is its
        // own app; only kernel threads go in the kernel's group.
        const apps::Identity identity = apps::classify(history.cgroup);
        p.group = p.kernel ? QString("kernel")
                  : history.cgroup.isEmpty() || identity.kind == "kernel" ? "process:" + stat.comm
                                                                          : identity.key;

        p.name = history.name;
        p.command = history.command.isEmpty() ? '[' + stat.comm + ']' : history.command;

        if (known && ticksPerCore > 0)
            p.cpu = 100.0 * double(stat.cpuTicks - std::min(stat.cpuTicks, before->ticks)) / ticksPerCore;

        // A process that ran no CPU since the last scan did next to no I/O:
        // its counters move in its own system calls. Most processes are
        // idle, so they're read every fifth scan instead, spread over the
        // PIDs, which makes the scan much cheaper. CPU time comes in 10 ms
        // ticks, so a little I/O can hide in an "idle" process; its rate is
        // taken over the time between reads, so it shows late, not wrong.
        const bool idle = known && stat.cpuTicks == before->ticks && before->ioAt >= 0 && (p.pid + m_scanCount) % 5 != 0;
        if (idle) {
            history.hasIo = before->hasIo;
            history.io = before->io;
            history.ioAt = before->ioAt;
            p.readRate = p.writeRate = before->hasIo ? 0 : -1;
        } else {
            history.hasIo = parsePidIo(readFile(base + "/io", 4096), history.io);
            history.ioAt = m_clock.elapsed();
            const double span = known ? (history.ioAt - before->ioAt) / 1000.0 : 0;
            if (history.hasIo && known && before->hasIo && before->ioAt >= 0 && span > 0 && seconds > 0) {
                p.readRate = (history.io.readBytes - std::min(history.io.readBytes, before->io.readBytes)) / span;
                p.writeRate = (history.io.writeBytes - std::min(history.io.writeBytes, before->io.writeBytes)) / span;
            } else if (history.hasIo) {
                p.readRate = p.writeRate = 0;
            }
        }

        if (m_gpus->any() && !p.kernel)
            sampleGpuClients(base, p, known, seconds, gpuShares, gpuMemory, gpuClients);

        seen.insert(p.pid, history);
        processes.append(p);
    }
    closedir(dir);

    // GPU usage: devices without counters of their own add up their
    // clients', and NVIDIA's driver reports per process through NVML.
    ++m_scanCount;
    m_prevClients = gpuClients;
    for (auto it = m_gpuFds.begin(); it != m_gpuFds.end();)
        it = seen.contains(it.key()) ? std::next(it) : m_gpuFds.erase(it);
    m_gpus->setClientTotals(gpuShares, gpuMemory);
    const QHash<int, gpu::ProcessUsage> nvidia = m_gpus->nvidiaProcesses();
    if (!nvidia.isEmpty()) {
        for (ProcessInfo &p : processes) {
            const auto usage = nvidia.constFind(p.pid);
            if (usage != nvidia.constEnd()) {
                p.gpu += usage->percent;
                p.gpuMemory += usage->memory;
            }
        }
    }
    m_processes.setGpuAvailable(m_gpus->perApp());

    m_prevPids = seen;
    m_taskCount = tasks;
    m_processList = processes;
    m_groupList = groupProcesses(processes, seconds);
    recordMoment();
    // While rewound, the list holds still on the moment being looked at.
    if (m_rewoundTo == 0)
        m_processes.update(m_processList, m_groupList);
}

void Monitor::recordMoment() {
    // What explains a spike: every app or process that was doing something,
    // and the ones holding the most memory. Idle services add nothing to
    // look back at but would cost memory once a second for ten minutes.
    auto io = [](double rate) { return std::max(0.0, rate); };
    Moment moment;
    moment.time = QDateTime::currentMSecsSinceEpoch();

    QList<GroupInfo> groups = m_groupList;
    std::sort(groups.begin(), groups.end(), [](const GroupInfo &a, const GroupInfo &b) { return a.memory > b.memory; });
    for (int i = 0; i < groups.size(); ++i) {
        GroupInfo g = groups[i];
        if (i >= 20 && g.cpu < 0.1 && g.gpu < 0.1 && io(g.readRate) + io(g.writeRate) == 0)
            continue;
        // The same few strings come back every second; keep one copy each.
        g.key = intern(g.key);
        g.name = intern(g.name);
        g.kind = intern(g.kind);
        g.user = intern(g.user);
        g.control = intern(g.control);
        for (QString &unit : g.units)
            unit = intern(unit);
        g.controlUnits.clear(); // The past can't be paused.
        moment.groups.append(g);
    }

    QList<ProcessInfo> byMemory = m_processList;
    std::sort(byMemory.begin(), byMemory.end(), [](const ProcessInfo &a, const ProcessInfo &b) { return a.memory > b.memory; });
    for (int i = 0; i < byMemory.size(); ++i) {
        ProcessInfo p = byMemory[i];
        if (i >= 20 && p.cpu < 0.5 && p.gpu < 0.5 && io(p.readRate) + io(p.writeRate) == 0)
            continue;
        p.group = intern(p.group);
        p.name = intern(p.name);
        p.user = intern(p.user);
        moment.processes.append(p);
    }
    moment.groups.squeeze();
    moment.processes.squeeze();

    m_moments.append(moment);
    if (m_moments.size() > MomentCount)
        m_moments.remove(0, m_moments.size() - MomentCount);
    // Strings no moment uses any more go once the pool outgrows its use.
    if (m_strings.size() > 4096)
        m_strings.clear();
}

QString Monitor::intern(const QString &text) {
    const auto it = m_strings.constFind(text);
    if (it != m_strings.constEnd())
        return *it;
    m_strings.insert(text);
    return text;
}

int Monitor::momentAt(double time) const {
    if (m_moments.isEmpty())
        return -1;
    // A scan reports what happened since the one before it, so the moment
    // that explains a time is the first scan at or after it.
    auto it = std::lower_bound(m_moments.begin(), m_moments.end(), qint64(time),
                               [](const Moment &m, qint64 t) { return m.time < t; });
    return it == m_moments.end() ? int(m_moments.size()) - 1 : int(it - m_moments.begin());
}

void Monitor::rewindTo(double time) {
    const int at = time > 0 ? momentAt(time) : -1;
    const double moment = at < 0 || at == m_moments.size() - 1 ? 0 : double(m_moments[at].time);
    if (moment == m_rewoundTo)
        return;
    m_rewoundTo = moment;
    if (at >= 0 && moment > 0)
        m_processes.update(m_moments[at].processes, m_moments[at].groups);
    else
        m_processes.update(m_processList, m_groupList);
    emit rewindChanged();
}

void Monitor::rewindBy(double seconds) {
    if (m_moments.isEmpty())
        return;
    const double from = m_rewoundTo > 0 ? m_rewoundTo : double(m_moments.last().time);
    // Never past the oldest moment: rewinding further holds there.
    rewindTo(std::max(double(m_moments.first().time), from + seconds * 1000));
}

double Monitor::sampleTime(int samplesBack) const {
    const int index = int(m_sampleTimes.size()) - 1 - samplesBack;
    return index >= 0 && index < m_sampleTimes.size() ? double(m_sampleTimes[index]) : 0;
}

double Monitor::momentTime(double time) const {
    const int at = momentAt(time);
    return at < 0 ? 0 : double(m_moments[at].time);
}

int Monitor::samplesSince(double time) const {
    const auto it = std::upper_bound(m_sampleTimes.begin(), m_sampleTimes.end(), qint64(time));
    return int(m_sampleTimes.end() - it);
}

QVariantList Monitor::blame(double time, const QString &what, int count) const {
    const int at = momentAt(time);
    if (at < 0)
        return {};
    const Moment &moment = m_moments[at];
    auto value = [&](const GroupInfo &g) {
        return what == "memory" ? double(g.memory)
               : what == "read" ? std::max(0.0, g.readRate)
               : what == "write" ? std::max(0.0, g.writeRate)
               : what == "io" ? std::max(0.0, g.readRate) + std::max(0.0, g.writeRate)
               : what == "gpu" ? g.gpu
                              : g.cpu;
    };
    QList<const GroupInfo *> groups;
    for (const GroupInfo &g : moment.groups)
        if (value(g) > 0)
            groups.append(&g);
    std::sort(groups.begin(), groups.end(), [&](const GroupInfo *a, const GroupInfo *b) { return value(*a) > value(*b); });
    QVariantList result;
    for (int i = 0; i < groups.size() && i < count; ++i)
        result.append(QVariantMap{{"name", groups[i]->name}, {"key", groups[i]->key}, {"value", value(*groups[i])},
                                  {"time", double(moment.time)}});
    return result;
}

// The name people know an app by: its desktop entry's, else its own.
QString Monitor::groupName(const apps::Identity &identity, const QStringList &memberNames) {
    if (identity.kind == "kernel")
        return tr("Kernel");
    if (identity.kind == "session")
        return tr("Login session");
    if (!m_desktopNames)
        m_desktopNames.emplace();
    QString name;
    if (identity.kind == "app" || identity.kind == "service") {
        name = m_desktopNames->name(identity.id);
        // A template instance named after a desktop entry, like uwsm's
        // wayland-wm@hyprland.desktop for the compositor itself.
        const QString instance = identity.id.section('@', 1);
        if (name.isEmpty() && instance.endsWith(".desktop"))
            name = m_desktopNames->name(instance.chopped(8));
        // Launchers like xdg-terminal-exec name the scope after themselves;
        // what runs in it is the app: the first process, oldest first, that
        // has a desktop entry of its own (the terminal, not the shell in it).
        for (const QString &member : memberNames) {
            if (!name.isEmpty() || identity.kind != "app")
                break;
            name = m_desktopNames->name(member);
        }
    }
    if (!name.isEmpty())
        return name;
    // A template instance with no entry of its own ("org.gnome.Shell@user"):
    // the template's entry, a desktop component's name, or the template.
    // System services keep their instance: getty@tty1 is not getty@tty2.
    if (identity.kind != "app" && identity.kind != "service")
        return identity.id;
    const QString base = identity.id.section('@', 0, 0);
    if (base != identity.id)
        name = m_desktopNames->name(base);
    if (name.isEmpty())
        name = apps::componentName(base);
    return name.isEmpty() ? base : name;
}

QList<GroupInfo> Monitor::groupProcesses(const QList<ProcessInfo> &processes, double seconds) {
    struct Acc {
        GroupInfo group;
        QStringList cgroups;
        int leaderPid = 0;
        QMap<int, QString> names; // By PID, so oldest first.
        double readSum = 0, writeSum = 0;
        bool readKnown = false, writeKnown = false;
        // Per cgroup, the members' own sums, for cgroups without counters.
        QHash<QString, double> cpuByCgroup;
        QHash<QString, quint64> memoryByCgroup;
    };
    QHash<QString, Acc> groups;
    QHash<QString, apps::Identity> identities;

    for (const ProcessInfo &p : processes) {
        const QString cgroup = p.kernel ? QString("/") : m_cgroupOf.value(p.pid);
        Acc &acc = groups[p.group];
        if (acc.group.key.isEmpty()) {
            acc.group.key = p.group;
            identities.insert(p.group, p.group.startsWith("process:")
                                           ? apps::Identity{p.group, p.name, "process", {}}
                                           : apps::classify(cgroup));
        }
        acc.group.pids.append(p.pid);
        acc.group.threads += p.threads;
        if (!acc.cgroups.contains(cgroup))
            acc.cgroups.append(cgroup);
        acc.cpuByCgroup[cgroup] += p.cpu;
        acc.memoryByCgroup[cgroup] += p.memory;
        acc.group.gpu += p.gpu;
        acc.group.gpuMemory += p.gpuMemory;
        if (p.readRate >= 0) { acc.readSum += p.readRate; acc.readKnown = true; }
        if (p.writeRate >= 0) { acc.writeSum += p.writeRate; acc.writeKnown = true; }
        acc.names.insert(p.pid, p.name);
        if (acc.leaderPid == 0 || p.pid < acc.leaderPid) {
            acc.leaderPid = p.pid;
            acc.group.user = p.user;
        }
        const QString unit = apps::unescape(apps::classify(cgroup).unit);
        if (!unit.isEmpty() && !acc.group.units.contains(unit))
            acc.group.units.append(unit);
    }

    // The cgroup's own counters are exact where the members' sums are not:
    // they include children that already exited (a build's compilers) and
    // count shared memory once. Read them where the kernel offers them.
    const QString root = m_sys + "/fs/cgroup";
    QHash<QString, CgroupCounters> current;
    QList<GroupInfo> result;
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        Acc &acc = it.value();
        GroupInfo &g = acc.group;
        const apps::Identity identity = identities.value(g.key);
        g.kind = identity.kind;
        if (!m_groupNames.contains(g.key))
            m_groupNames.insert(g.key, groupName(identity, acc.names.values()));
        g.name = m_groupNames.value(g.key);

        double cpu = 0, read = 0, write = 0, limit = 0;
        quint64 memory = 0;
        bool ioKnown = true, limited = true;
        // The strongest reason not to touch any of its cgroups wins.
        static const QStringList reasons{"", "none", "system", "session", "self"};
        int reason = 0;
        for (const QString &cgroup : acc.cgroups) {
            // The root cgroup's counters are the whole system's.
            const QByteArray cpuStat = cgroup.isEmpty() || cgroup == "/" ? QByteArray()
                                                                          : readFile(root + cgroup + "/cpu.stat", 4096);
            if (cpuStat.isEmpty()) {
                cpu += acc.cpuByCgroup.value(cgroup);
                memory += acc.memoryByCgroup.value(cgroup);
                ioKnown = false;
                reason = std::max(reason, 1);
                limited = false;
                continue;
            }
            reason = std::max(reason, int(reasons.indexOf(controlOf(cgroup))));
            if (reason == 0)
                g.controlUnits.append(apps::classify(cgroup).unit);
            if (readFile(root + cgroup + "/cgroup.events", 4096).contains("frozen 1"))
                g.frozen = true;
            // "max 100000" or "<quota> <period>"; without the file, the CPU
            // controller is off for it, so nothing limits it.
            const QList<QByteArray> max = readFile(root + cgroup + "/cpu.max", 256).trimmed().split(' ');
            if (max.size() == 2 && max[0] != "max" && max[1].toDouble() > 0)
                limit += 100.0 * max[0].toDouble() / max[1].toDouble();
            else
                limited = false;
            CgroupCounters now;
            for (const QByteArray &line : cpuStat.split('\n'))
                if (line.startsWith("usage_usec "))
                    now.cpuUsec = line.mid(11).toULongLong();

            // memory.stat and io.stat are the costly files here, and an app
            // that ran no CPU since the last scan has barely changed: its
            // memory and disk counters are read every fifth scan instead,
            // spread over the apps. Rates divide by the time between reads,
            // so disk writes flushed meanwhile still show at their rate.
            const auto before = m_prevCgroups.constFind(cgroup);
            const bool idle = before != m_prevCgroups.constEnd() && before->readAt >= 0
                              && before->cpuUsec == now.cpuUsec
                              && (m_scanCount + int(qHash(cgroup) % 5)) % 5 != 0;
            if (idle) {
                now.held = before->held;
                now.hasIo = before->hasIo;
                now.readBytes = before->readBytes;
                now.writeBytes = before->writeBytes;
                now.readAt = before->readAt;
            } else {
                // Memory the app itself holds: anonymous, shared, and kernel
                // memory, but not the page cache, which the kernel hands back.
                for (const QByteArray &line : readFile(root + cgroup + "/memory.stat", 16384).split('\n')) {
                    const QList<QByteArray> field = line.split(' ');
                    if (field.size() == 2 && (field[0] == "anon" || field[0] == "shmem" || field[0] == "kernel"))
                        now.held += field[1].toULongLong();
                }
                // Only whole physical disks: a disk and the encrypted volume
                // on top of it both list the same bytes.
                const QByteArray io = readFile(root + cgroup + "/io.stat", 16384);
                now.hasIo = !io.isNull();
                for (const QByteArray &line : io.split('\n')) {
                    const QList<QByteArray> fields = line.split(' ');
                    if (fields.isEmpty() || !m_physicalDevices.contains(QString::fromLatin1(fields[0])))
                        continue;
                    for (const QByteArray &field : fields) {
                        if (field.startsWith("rbytes="))
                            now.readBytes += field.mid(7).toULongLong();
                        else if (field.startsWith("wbytes="))
                            now.writeBytes += field.mid(7).toULongLong();
                    }
                }
                now.readAt = m_clock.elapsed();
            }
            memory += now.held > 0 ? now.held : acc.memoryByCgroup.value(cgroup);
            current.insert(cgroup, now);

            if (before != m_prevCgroups.constEnd() && seconds > 0) {
                cpu += 100.0 * double(now.cpuUsec - std::min(now.cpuUsec, before->cpuUsec)) / (seconds * 1e6);
                if (now.hasIo && before->hasIo) {
                    const double span = (now.readAt - before->readAt) / 1000.0;
                    if (span > 0) {
                        read += (now.readBytes - std::min(now.readBytes, before->readBytes)) / span;
                        write += (now.writeBytes - std::min(now.writeBytes, before->writeBytes)) / span;
                    }
                } else {
                    ioKnown = false;
                }
            } else {
                // A cgroup seen for the first time has no previous reading.
                cpu += acc.cpuByCgroup.value(cgroup);
                ioKnown = false;
            }
        }
        g.cpu = cpu;
        g.memory = memory;
        g.control = g.kind == "process" || g.kind == "kernel" ? QString("none") : reasons.value(reason);
        if (!g.control.isEmpty())
            g.controlUnits.clear();
        // A limit counts only when every part of the app has one.
        g.cpuLimit = limited ? limit : 0;
        g.memoryShare = m_memTotal > 0 ? memory / m_memTotal : 0;
        if (ioKnown) {
            g.readRate = read;
            g.writeRate = write;
        } else {
            g.readRate = acc.readKnown ? acc.readSum : -1;
            g.writeRate = acc.writeKnown ? acc.writeSum : -1;
        }
        result.append(g);
    }
    m_prevCgroups = current;
    return result;
}

QString Monitor::controlOf(const QString &cgroup) const {
    if (!m_selfCgroup.isEmpty() && cgroup == m_selfCgroup)
        return "self";
    const QString manager = QString("/user@%1.service/").arg(m_uid);
    const int at = cgroup.indexOf(manager);
    if (at < 0)
        return "system";
    // systemd's convention: apps in app.slice, the user's own daemons in
    // background.slice, and what the desktop can't do without (compositor,
    // D-Bus, audio, portals) in session.slice. The manager itself is
    // init.scope. Freezing those would freeze the session.
    const QString slice = cgroup.mid(at + manager.size()).section('/', 0, 0);
    return slice == "app.slice" || slice == "background.slice" ? QString() : QString("session");
}

const GroupInfo *Monitor::findGroup(const QString &key) const {
    for (const GroupInfo &g : m_groupList)
        if (g.key == key)
            return &g;
    return nullptr;
}

QVariantMap Monitor::app(const QString &groupKey) const {
    const GroupInfo *g = findGroup(groupKey);
    if (!g)
        return {};
    return {{"key", g->key}, {"name", g->name}, {"frozen", g->frozen}, {"cpuLimit", g->cpuLimit}, {"control", g->control}};
}

QString Monitor::refusal(const GroupInfo &g) const {
    if (g.control == "self")
        return tr("Montop runs inside %1, so it can't pause or limit it without pausing itself.").arg(g.name);
    if (g.control == "session")
        return tr("%1 keeps your desktop running, so Montop won't pause or limit it.").arg(g.name);
    if (g.control == "system")
        return tr("%1 is a system service. Pausing or limiting it needs root.").arg(g.name);
    if (!g.control.isEmpty() || g.controlUnits.isEmpty())
        return tr("%1 has no cgroup of its own, so it can't be paused or limited.").arg(g.name);
    return {};
}

QString Monitor::systemctl(const QStringList &arguments) {
    QProcess process;
    process.start(m_systemctl, QStringList{"--user"} + arguments);
    if (!process.waitForStarted(2000))
        return tr("Couldn't run systemctl: %1").arg(process.errorString());
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(1000);
        return tr("systemctl didn't answer in time.");
    }
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
        return {};
    const QString error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    return error.isEmpty() ? tr("systemctl failed with code %1.").arg(process.exitCode()) : error;
}

QString Monitor::setAppPaused(const QString &groupKey, bool paused) {
    const GroupInfo *found = findGroup(groupKey);
    if (!found)
        return tr("That app has already exited.");
    GroupInfo &g = m_groupList[found - m_groupList.constData()];
    const QString refused = refusal(g);
    if (!refused.isEmpty())
        return refused;
    const QString error = systemctl(QStringList{paused ? "freeze" : "thaw"} + g.controlUnits);
    if (!error.isEmpty())
        return error;
    for (const QString &unit : g.controlUnits) {
        if (paused)
            m_pausedUnits.insert(unit);
        else
            m_pausedUnits.remove(unit);
    }
    g.frozen = paused;
    m_processes.refresh(m_groupList);
    return {};
}

QString Monitor::setAppCpuLimit(const QString &groupKey, double percent) {
    const GroupInfo *found = findGroup(groupKey);
    if (!found)
        return tr("That app has already exited.");
    GroupInfo &g = m_groupList[found - m_groupList.constData()];
    const QString refused = refusal(g);
    if (!refused.isEmpty())
        return refused;
    // systemd limits each unit, so an app spread over several (a terminal
    // with a scope per window) shares the limit out between them.
    const int units = int(g.controlUnits.size());
    const int share = percent > 0 ? std::max(1, int(std::lround(percent / units))) : 0;
    for (const QString &unit : g.controlUnits) {
        const QString error = systemctl({"set-property", "--runtime", unit,
                                         share > 0 ? QString("CPUQuota=%1%").arg(share) : QString("CPUQuota=")});
        if (!error.isEmpty())
            return error;
        if (share > 0)
            m_limitedUnits.insert(unit);
        else
            m_limitedUnits.remove(unit);
    }
    g.cpuLimit = double(share) * units;
    m_processes.refresh(m_groupList);
    return {};
}

void Monitor::undoControls() {
    if (!m_pausedUnits.isEmpty())
        systemctl(QStringList{"thaw"} + QStringList(m_pausedUnits.begin(), m_pausedUnits.end()));
    for (const QString &unit : std::as_const(m_limitedUnits))
        systemctl({"set-property", "--runtime", unit, "CPUQuota="});
    m_pausedUnits.clear();
    m_limitedUnits.clear();
}
