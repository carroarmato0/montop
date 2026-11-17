#include "gpu.h"

#include "procfs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>

namespace gpu {

namespace {

QString readLine(const QString &path) {
    return QString::fromUtf8(procfs::readFile(path, 4096)).trimmed();
}

double readNumber(const QString &path, double fallback = -1) {
    bool ok = false;
    const double value = readLine(path).toDouble(&ok);
    return ok ? value : fallback;
}

QString tr(const char *text) {
    return QCoreApplication::translate("Monitor", text);
}

// "1234 KiB" and the like, as fdinfo writes memory.
quint64 parseSize(const QByteArray &value) {
    const QList<QByteArray> parts = value.simplified().split(' ');
    const quint64 number = parts.value(0).toULongLong();
    const QByteArray unit = parts.value(1);
    return unit == "KiB" ? number << 10 : unit == "MiB" ? number << 20 : unit == "GiB" ? number << 30 : number;
}

// Drivers that report each client's engine time in fdinfo, so usage can be
// put down to apps even before any app has used the GPU.
const QStringList fdinfoDrivers{"i915", "xe", "amdgpu", "msm", "panthor", "panfrost", "v3d", "etnaviv", "lima"};

// A name from the driver alone: for a GPU on the SoC, which has no PCI ids
// to look up, or a card the PCI ids database doesn't know.
QString driverName(const QString &driver) {
    if (driver == "nvidia" || driver == "nouveau")
        return "NVIDIA GPU";
    if (driver == "amdgpu" || driver == "radeon")
        return "AMD GPU";
    if (driver == "i915" || driver == "xe")
        return "Intel GPU";
    if (driver == "asahi")
        return "Apple GPU";
    if (driver == "msm")
        return "Adreno GPU";
    if (driver == "panthor" || driver == "panfrost" || driver == "lima")
        return "Mali GPU";
    if (driver == "v3d")
        return "VideoCore GPU";
    if (driver == "etnaviv")
        return "Vivante GPU";
    return driver + " GPU";
}

// The marketing name from the PCI ids database: "Alder Lake-UP3 GT2 [Iris
// Xe Graphics]" becomes "Intel Iris Xe Graphics".
QString pciName(const QString &pciIds, const QString &vendor, const QString &device) {
    static const QHash<QString, QString> vendors{{"8086", "Intel"}, {"1002", "AMD"}, {"10de", "NVIDIA"}};
    if (vendor.size() != 4 || device.size() != 4)
        return {};
    const QByteArray ids = procfs::readFile(pciIds, 8 << 20);
    if (ids.isEmpty())
        return {};
    const QByteArray vendorLine = '\n' + vendor.toLatin1() + "  ";
    qsizetype at = ids.indexOf(vendorLine);
    if (at < 0)
        return {};
    at += 1;
    const qsizetype vendorEnd = ids.indexOf('\n', at);
    QString vendorName = vendors.value(vendor, QString::fromUtf8(ids.mid(at + 6, vendorEnd - at - 6)).section(' ', 0, 0));
    for (qsizetype line = vendorEnd + 1; line > 0 && line < ids.size();) {
        const qsizetype end = ids.indexOf('\n', line);
        const QByteArray text = ids.mid(line, end < 0 ? -1 : end - line);
        if (!text.startsWith('\t') && !text.startsWith('#') && !text.isEmpty())
            break; // The next vendor.
        if (text.startsWith('\t') && !text.startsWith("\t\t") && text.mid(1, 4) == device.toLatin1()) {
            QString name = QString::fromUtf8(text.mid(7)).trimmed();
            const QRegularExpressionMatch bracket = QRegularExpression("\\[(.+)\\]").match(name);
            if (bracket.hasMatch())
                name = bracket.captured(1).section('/', 0, 0).trimmed();
            return name.startsWith(vendorName) ? name : vendorName + ' ' + name;
        }
        line = end < 0 ? -1 : end + 1;
    }
    return vendorName.isEmpty() ? QString() : vendorName + " GPU";
}

} // namespace

bool parseFdinfo(const QByteArray &text, Client &client) {
    bool drm = false;
    for (const QByteArray &line : text.split('\n')) {
        if (!line.startsWith("drm-"))
            continue;
        const qsizetype colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const QByteArray key = line.left(colon);
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (key == "drm-driver") {
            drm = true;
            client.driver = QString::fromLatin1(value);
        } else if (key == "drm-client-id") {
            client.id = QString::fromLatin1(value);
        } else if (key.startsWith("drm-engine-capacity-")) {
            client.capacity.insert(QString::fromLatin1(key.mid(20)), std::max<quint64>(1, value.toULongLong()));
        } else if (key.startsWith("drm-engine-")) {
            client.engineNs.insert(QString::fromLatin1(key.mid(11)), value.split(' ').value(0).toULongLong());
        } else if (key.startsWith("drm-total-cycles-")) {
            client.totalCycles.insert(QString::fromLatin1(key.mid(17)), value.toULongLong());
        } else if (key.startsWith("drm-cycles-")) {
            client.cycles.insert(QString::fromLatin1(key.mid(11)), value.toULongLong());
        } else if (key.startsWith("drm-resident-") || key.startsWith("drm-memory-")) {
            // drm-memory-* is amdgpu's older name for resident memory.
            client.memory += parseSize(value);
        }
    }
    return drm;
}

QHash<QString, double> engineShares(const Client &before, const Client &now, double seconds) {
    QHash<QString, double> shares;
    for (auto it = now.engineNs.begin(); it != now.engineNs.end(); ++it) {
        const auto was = before.engineNs.constFind(it.key());
        if (was == before.engineNs.constEnd() || seconds <= 0)
            continue;
        const double capacity = double(now.capacity.value(it.key(), 1));
        const double busy = double(it.value() - std::min(it.value(), *was)) / (seconds * 1e9 * capacity);
        shares.insert(it.key(), std::clamp(busy, 0.0, 1.0));
    }
    // Drivers that count cycles instead (xe): busy cycles over the GPU's.
    for (auto it = now.cycles.begin(); it != now.cycles.end(); ++it) {
        if (shares.contains(it.key()) || now.engineNs.contains(it.key()))
            continue;
        const auto was = before.cycles.constFind(it.key());
        const quint64 total = now.totalCycles.value(it.key()), totalWas = before.totalCycles.value(it.key());
        if (was == before.cycles.constEnd() || total <= totalWas)
            continue;
        const double capacity = double(now.capacity.value(it.key(), 1));
        const double busy = double(it.value() - std::min(it.value(), *was)) / (double(total - totalWas) * capacity);
        shares.insert(it.key(), std::clamp(busy, 0.0, 1.0));
    }
    return shares;
}

// The real NVML, through dlopen. Only the handful of calls Montop needs are
// declared here; NVML keeps these signatures stable across driver versions.
class RealNvml : public Nvml {
    struct Utilization { unsigned gpu, memory; };
    struct Memory { unsigned long long total, free, used; };
    // The newer call, which leaves the driver's reserved memory out of
    // "used", as nvidia-smi does.
    struct Memory2 { unsigned version; unsigned long long total, reserved, free, used; };
    struct ProcessInfo { unsigned pid; unsigned long long usedGpuMemory; unsigned gpuInstanceId, computeInstanceId; };
    struct Sample { unsigned pid; unsigned long long timeStamp; unsigned smUtil, memUtil, encUtil, decUtil; };
    using Handle = void *;
    enum { Success = 0, NotFound = 6, InsufficientSize = 7 };

    void *m_library = nullptr;
    int (*m_shutdown)() = nullptr;
    int (*m_byPci)(const char *, Handle *) = nullptr;
    int (*m_name)(Handle, char *, unsigned) = nullptr;
    int (*m_utilization)(Handle, Utilization *) = nullptr;
    int (*m_memory)(Handle, Memory *) = nullptr;
    int (*m_memory2)(Handle, Memory2 *) = nullptr;
    int (*m_temperature)(Handle, int, unsigned *) = nullptr;
    int (*m_power)(Handle, unsigned *) = nullptr;
    int (*m_clock)(Handle, int, unsigned *) = nullptr;
    int (*m_fan)(Handle, unsigned *) = nullptr;
    int (*m_graphics)(Handle, unsigned *, ProcessInfo *) = nullptr;
    int (*m_compute)(Handle, unsigned *, ProcessInfo *) = nullptr;
    int (*m_samples)(Handle, Sample *, unsigned *, unsigned long long) = nullptr;
    QHash<Handle, unsigned long long> m_lastSeen;

    template <typename F> bool resolve(F &function, const char *symbol) {
        function = reinterpret_cast<F>(dlsym(m_library, symbol));
        return function != nullptr;
    }

    void addProcesses(Handle device, int (*list)(Handle, unsigned *, ProcessInfo *), QHash<int, ProcessUsage> &usage) {
        QList<ProcessInfo> infos(64);
        unsigned count = unsigned(infos.size());
        int result = list(device, &count, infos.data());
        if (result == InsufficientSize) {
            infos.resize(count + 16);
            count = unsigned(infos.size());
            result = list(device, &count, infos.data());
        }
        if (result != Success)
            return;
        for (unsigned i = 0; i < count; ++i) {
            // "Not available" is all ones; under WSL and some setups.
            const unsigned long long bytes = infos[i].usedGpuMemory == ~0ULL ? 0 : infos[i].usedGpuMemory;
            ProcessUsage &u = usage[int(infos[i].pid)];
            u.memory = std::max<quint64>(u.memory, bytes); // Graphics and compute list the same memory.
        }
    }

public:
    bool open() {
        m_library = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (!m_library)
            return false;
        int (*init)() = nullptr;
        if (!resolve(init, "nvmlInit_v2") || !resolve(m_shutdown, "nvmlShutdown")
            || !resolve(m_byPci, "nvmlDeviceGetHandleByPciBusId_v2") || !resolve(m_name, "nvmlDeviceGetName")
            || !resolve(m_utilization, "nvmlDeviceGetUtilizationRates") || !resolve(m_memory, "nvmlDeviceGetMemoryInfo")
            || !resolve(m_temperature, "nvmlDeviceGetTemperature") || !resolve(m_power, "nvmlDeviceGetPowerUsage")
            || !resolve(m_clock, "nvmlDeviceGetClockInfo") || !resolve(m_fan, "nvmlDeviceGetFanSpeed")
            || !resolve(m_graphics, "nvmlDeviceGetGraphicsRunningProcesses_v3")
            || !resolve(m_compute, "nvmlDeviceGetComputeRunningProcesses_v3")
            || !resolve(m_samples, "nvmlDeviceGetProcessUtilization") || init() != Success) {
            dlclose(m_library);
            m_library = nullptr;
            return false;
        }
        resolve(m_memory2, "nvmlDeviceGetMemoryInfo_v2"); // Optional: older drivers lack it.
        return true;
    }
    ~RealNvml() override {
        if (m_library) {
            m_shutdown();
            dlclose(m_library);
        }
    }

    void *device(const QString &pci) override {
        Handle handle = nullptr;
        return m_byPci(pci.toLatin1().constData(), &handle) == Success ? handle : nullptr;
    }
    QString name(void *device) override {
        char buffer[96] = {};
        return m_name(device, buffer, sizeof buffer) == Success ? QString::fromUtf8(buffer) : QString();
    }
    double busy(void *device) override {
        Utilization u{};
        return m_utilization(device, &u) == Success ? u.gpu / 100.0 : -1;
    }
    bool memory(void *device, double &used, double &total) override {
        Memory2 m2{};
        m2.version = unsigned(sizeof(Memory2)) | (2u << 24);
        if (m_memory2 && m_memory2(device, &m2) == Success) {
            used = double(m2.used);
            total = double(m2.total);
            return true;
        }
        Memory m{};
        if (m_memory(device, &m) != Success)
            return false;
        used = double(m.used);
        total = double(m.total);
        return true;
    }
    double temperature(void *device) override {
        unsigned value = 0;
        return m_temperature(device, 0 /* the GPU die */, &value) == Success ? double(value) : -1;
    }
    double watts(void *device) override {
        unsigned milliwatts = 0;
        return m_power(device, &milliwatts) == Success ? milliwatts / 1000.0 : -1;
    }
    double mhz(void *device) override {
        unsigned value = 0;
        return m_clock(device, 0 /* graphics */, &value) == Success ? double(value) : -1;
    }
    double fan(void *device) override {
        unsigned value = 0;
        return m_fan(device, &value) == Success ? double(value) : -1;
    }
    QHash<int, ProcessUsage> processes(void *device) override {
        QHash<int, ProcessUsage> usage;
        addProcesses(device, m_graphics, usage);
        addProcesses(device, m_compute, usage);
        // Utilization comes as samples, each a slice of a second or so,
        // since the newest one already seen; average them per process.
        QList<Sample> samples(256);
        unsigned count = unsigned(samples.size());
        unsigned long long &lastSeen = m_lastSeen[device];
        int result = m_samples(device, samples.data(), &count, lastSeen);
        if (result == InsufficientSize) {
            samples.resize(count + 16);
            count = unsigned(samples.size());
            result = m_samples(device, samples.data(), &count, lastSeen);
        }
        if (result != Success)
            return usage;
        QHash<int, QPair<double, int>> sums;
        for (unsigned i = 0; i < count; ++i) {
            auto &sum = sums[int(samples[i].pid)];
            sum.first += samples[i].smUtil;
            ++sum.second;
            lastSeen = std::max(lastSeen, samples[i].timeStamp);
        }
        for (auto it = sums.begin(); it != sums.end(); ++it)
            usage[it.key()].percent = it.value().first / it.value().second;
        return usage;
    }
};

std::unique_ptr<Nvml> Nvml::load() {
    auto nvml = std::make_unique<RealNvml>();
    if (!nvml->open())
        return nullptr;
    return nvml;
}

Gpus::Gpus(const QString &sys, const QString &pciIds) : m_sys(sys) {
    discover(pciIds);
}

Gpus::~Gpus() = default;

void Gpus::discover(const QString &pciIds) {
    // Every node that renders: a card without a render node is a display
    // controller only (Apple's DCP, simpledrm, a DisplayLink dock).
    const QDir drm(m_sys + "/class/drm");
    QHash<QString, QString> cards; // Device path to card name.
    QSet<QString> renders;
    for (const QString &node : drm.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System)) {
        const bool card = QRegularExpression("^card\\d+$").match(node).hasMatch();
        if (!card && !node.startsWith("renderD"))
            continue;
        const QString device = QFileInfo(drm.filePath(node) + "/device").canonicalFilePath();
        if (device.isEmpty())
            continue;
        m_nodes.insert(node, device);
        if (card)
            cards.insert(device, node);
        else
            renders.insert(device);
    }
    QStringList paths;
    for (auto it = cards.begin(); it != cards.end(); ++it)
        if (renders.contains(it.key()))
            paths.append(it.key());
    std::sort(paths.begin(), paths.end(), [&](const QString &a, const QString &b) {
        return cards.value(a).mid(4).toInt() < cards.value(b).mid(4).toInt();
    });

    for (const QString &path : paths) {
        Device d;
        d.path = path;
        d.card = cards.value(path);
        for (const QByteArray &line : procfs::readFile(path + "/uevent", 4096).split('\n')) {
            if (line.startsWith("DRIVER="))
                d.driver = QString::fromLatin1(line.mid(7));
            else if (line.startsWith("PCI_SLOT_NAME="))
                d.pci = QString::fromLatin1(line.mid(14));
        }
        if (d.driver.isEmpty())
            d.driver = QFileInfo(QFileInfo(path + "/driver").symLinkTarget()).fileName();
        d.name = readLine(path + "/product_name");
        if (d.name.isEmpty() && !d.pci.isEmpty())
            d.name = pciName(pciIds, readLine(path + "/vendor").mid(2), readLine(path + "/device").mid(2));
        if (d.name.isEmpty())
            d.name = driverName(d.driver);
        d.perApp = fdinfoDrivers.contains(d.driver);
        m_devices.append(d);
        m_previous.append(Previous{});
    }
}

bool Gpus::perApp() const {
    return std::any_of(m_devices.begin(), m_devices.end(), [](const Device &d) { return d.perApp; });
}

void Gpus::setNvml(std::unique_ptr<Nvml> nvml) {
    m_nvml = std::move(nvml);
    m_nvmlTried = true;
}

Nvml *Gpus::nvml() {
    if (!m_nvmlTried) {
        m_nvmlTried = true;
        m_nvml = Nvml::load();
    }
    return m_nvml.get();
}

void Gpus::sample(qint64 nowMs) {
    for (int i = 0; i < m_devices.size(); ++i)
        sampleDevice(m_devices[i], m_previous[i], nowMs);
}

void Gpus::sampleDevice(Device &d, Previous &previous, qint64 nowMs) {
    // A discrete GPU on a laptop sleeps when nothing uses it, and reading
    // its counters (or asking NVML) would wake it and drain the battery.
    d.asleep = readLine(d.path + "/power/runtime_status") == "suspended";
    d.note.clear();
    auto clear = [&] {
        d.busy = d.mhz = d.temperature = d.watts = d.fan = -1;
        d.memoryUsed = d.appMemory = -1;
    };
    if (d.asleep) {
        clear();
        previous = {};
        return;
    }
    const double seconds = previous.time < 0 ? 0 : (nowMs - previous.time) / 1000.0;
    previous.time = nowMs;

    if (d.driver == "nvidia") {
        Nvml *api = nvml();
        if (!previous.nvml && api && !d.pci.isEmpty())
            previous.nvml = api->device(d.pci);
        void *handle = previous.nvml;
        if (!handle) {
            clear();
            d.note = tr("Its driver reports usage only through NVIDIA's NVML library, which Montop couldn't load.");
            d.perApp = false;
            return;
        }
        d.perApp = true;
        d.busy = api->busy(handle);
        // Each NVML call costs about a tenth of a millisecond, and these
        // change slowly: once a second is plenty, against four for busy.
        if (previous.sensorsAt >= 0 && nowMs - previous.sensorsAt < 1000)
            return;
        if (previous.sensorsAt < 0) {
            const QString name = api->name(handle);
            if (!name.isEmpty())
                d.name = name.startsWith("NVIDIA") ? name : "NVIDIA " + name;
        }
        previous.sensorsAt = nowMs;
        double used = 0, total = 0;
        if (api->memory(handle, used, total)) {
            d.memoryUsed = used;
            d.memoryTotal = total;
        }
        d.temperature = api->temperature(handle);
        d.watts = api->watts(handle);
        d.mhz = api->mhz(handle);
        d.fan = api->fan(handle);
        return;
    }
    clear();

    auto glob = [](const QString &dir, const QString &pattern, const QString &tail) {
        QStringList found;
        for (const QString &entry : QDir(dir).entryList({pattern}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::System))
            found.append(dir + '/' + entry + tail);
        return found;
    };
    // How busy: AMD keeps a busy percentage of its own. Intel's idle
    // counters (RC6) only say the GPU is awake, which reads 100 % while its
    // engines are half idle, so Intel, like the rest, is measured by its
    // busiest engine over every app's usage, the same as the apps' GPU%.
    if (d.driver == "amdgpu") {
        const double percent = readNumber(d.path + "/gpu_busy_percent");
        d.busy = percent >= 0 ? percent / 100.0 : -1;
        d.memoryUsed = readNumber(d.path + "/mem_info_vram_used");
        d.memoryTotal = readNumber(d.path + "/mem_info_vram_total");
    } else if (d.driver == "i915") {
        d.mhz = readNumber(m_sys + "/class/drm/" + d.card + "/gt_act_freq_mhz");
    } else if (d.driver == "xe") {
        d.mhz = readNumber(d.path + "/tile0/gt0/freq0/act_freq");
    } else if (d.driver == "asahi") {
        sampleAgx(d, previous, nowMs);
    }
    if (d.busy < 0 && d.perApp)
        d.busy = m_clientBusy.value(d.path, 0);
    if (d.memoryTotal <= 0 && d.perApp)
        d.appMemory = double(m_clientMemory.value(d.path));

    // Sensors, where the GPU has its own (discrete cards, mostly).
    const QStringList hwmons = glob(d.path + "/hwmon", "hwmon*", "");
    if (!hwmons.isEmpty()) {
        const QString hwmon = hwmons.first();
        const double temp = readNumber(hwmon + "/temp1_input");
        d.temperature = temp >= 0 ? temp / 1000.0 : -1;
        double microwatts = readNumber(hwmon + "/power1_average");
        if (microwatts < 0)
            microwatts = readNumber(hwmon + "/power1_input");
        if (microwatts >= 0) {
            d.watts = microwatts / 1e6;
        } else {
            const double energy = readNumber(hwmon + "/energy1_input");
            if (energy >= 0 && previous.energy >= 0 && seconds > 0 && energy >= previous.energy)
                d.watts = (energy - previous.energy) / 1e6 / seconds;
            previous.energy = energy;
        }
        const double hz = readNumber(hwmon + "/freq1_input");
        if (hz > 0 && d.mhz < 0)
            d.mhz = hz / 1e6;
        const double pwm = readNumber(hwmon + "/pwm1");
        d.fan = pwm >= 0 ? pwm / 255.0 * 100 : -1;
    }
    // A GPU on the SoC clocks through devfreq.
    if (d.mhz < 0) {
        const QString devfreq = m_sys + "/class/devfreq";
        for (const QString &entry : QDir(devfreq).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System)) {
            if (QFileInfo(devfreq + '/' + entry + "/device").canonicalFilePath() == d.path) {
                const double hz = readNumber(devfreq + '/' + entry + "/cur_freq");
                d.mhz = hz > 0 ? hz / 1e6 : -1;
                break;
            }
        }
    }
    // agx_stats needs a second reading before it says anything.
    if (d.busy < 0 && previous.agxBusyNs < 0)
        d.note = tr("Its driver doesn't report how busy it is.");
}

// Apple's GPU (Asahi) has no busy percentage, but newer kernels keep a
// cumulative busy time and the power draw in agx_stats, as "key value"
// lines. The counter is published about once a second, so the share is
// taken over at least 0.9 s (a shorter window reads 0 or a spike) and
// smoothed over about 2 s, as YASSM does on the same hardware.
void Gpus::sampleAgx(Device &d, Previous &previous, qint64 nowMs) {
    const QByteArray text = procfs::readFile(d.path + "/agx_stats", 4096);
    if (text.isEmpty()) {
        previous.agxBusyNs = previous.agxBusy = -1;
        return;
    }
    double busyNs = -1, milliwatts = -1;
    for (const QByteArray &line : text.split('\n')) {
        const QList<QByteArray> fields = line.simplified().split(' ');
        if (fields.size() < 2)
            continue;
        bool ok = false;
        const double value = fields[1].toDouble(&ok);
        if (!ok)
            continue;
        if (fields[0] == "busy_ns")
            busyNs = value;
        else if (fields[0] == "power_mw")
            milliwatts = value;
    }
    if (milliwatts >= 0)
        d.watts = milliwatts / 1000.0;
    if (busyNs < 0) {
        previous.agxBusyNs = previous.agxBusy = -1;
        return;
    }
    // First reading, or the counter went back (driver reloaded): a new baseline.
    if (previous.agxBusyNs < 0 || busyNs < previous.agxBusyNs) {
        previous.agxBusyNs = busyNs;
        previous.agxAt = nowMs;
        previous.agxBusy = -1;
        return;
    }
    const qint64 elapsed = nowMs - previous.agxAt;
    if (elapsed >= 900) {
        const double instant = std::clamp((busyNs - previous.agxBusyNs) / (elapsed * 1e6), 0.0, 1.0);
        const double alpha = 1 - std::exp(-elapsed / 2000.0);
        previous.agxBusy = previous.agxBusy < 0 ? instant : previous.agxBusy + alpha * (instant - previous.agxBusy);
        previous.agxBusyNs = busyNs;
        previous.agxAt = nowMs;
    }
    d.busy = previous.agxBusy;
}

void Gpus::setClientTotals(const QHash<QString, QHash<QString, double>> &shares, const QHash<QString, quint64> &memory) {
    m_clientBusy.clear();
    for (auto it = shares.begin(); it != shares.end(); ++it) {
        double busiest = 0;
        for (double share : it.value())
            busiest = std::max(busiest, share);
        m_clientBusy.insert(it.key(), std::min(1.0, busiest));
    }
    m_clientMemory = memory;
    // Straight away, so a device's number matches its apps' at once rather
    // than a tick later.
    for (Device &d : m_devices) {
        if (d.asleep || !d.perApp || d.driver == "amdgpu" || d.driver == "nvidia")
            continue;
        d.busy = m_clientBusy.value(d.path, 0);
        if (d.memoryTotal <= 0)
            d.appMemory = double(m_clientMemory.value(d.path));
    }
}

QHash<int, ProcessUsage> Gpus::nvidiaProcesses() {
    QHash<int, ProcessUsage> usage;
    for (int i = 0; i < m_devices.size(); ++i) {
        const Device &d = m_devices[i];
        void *handle = m_previous[i].nvml;
        if (d.driver != "nvidia" || d.asleep || !d.perApp || !handle || !nvml())
            continue;
        const auto found = nvml()->processes(handle);
        for (auto it = found.begin(); it != found.end(); ++it) {
            ProcessUsage &u = usage[it.key()];
            u.percent += it.value().percent;
            u.memory += it.value().memory;
        }
    }
    return usage;
}

} // namespace gpu
