#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <memory>

// GPUs, through what each driver offers: the kernel's per-client usage in
// /proc/<pid>/fdinfo (Intel, AMD, Adreno, Mali, ...), each driver's own
// counters in sysfs for the whole device, and NVIDIA's NVML library for its
// proprietary driver, which reports nothing to the kernel's files. Apple's
// GPU on Asahi Linux keeps a busy counter and its power draw in agx_stats,
// on kernels that have it; per-app usage it doesn't report.
namespace gpu {

// One DRM client, an open GPU file, as its fdinfo describes it.
struct Client {
    QString driver;
    QString id; // drm-client-id: the same client seen through dup'd fds.
    QHash<QString, quint64> engineNs, cycles, totalCycles, capacity;
    quint64 memory = 0; // Resident bytes, over every memory region.
};

// False when the text isn't a DRM client's fdinfo.
bool parseFdinfo(const QByteArray &text, Client &client);

// Each engine's busy share between two readings of one client, already
// divided by the engine's capacity (identical engines grouped as one), so
// 1 is the engine fully busy.
QHash<QString, double> engineShares(const Client &before, const Client &now, double seconds);

struct Device {
    QString path;   // Real sysfs path of the device: its identity.
    QString card;   // "card1"
    QString driver; // "i915", "amdgpu", "nvidia", "asahi", ...
    QString name;
    QString pci;    // "0000:00:02.0", or empty for a GPU on the SoC.
    bool asleep = false; // Runtime-suspended: nothing is read, so it sleeps on.
    bool perApp = false; // Its usage can be put down to apps.
    double busy = -1;    // 0..1, or -1 unknown.
    double memoryUsed = -1, memoryTotal = -1; // Its own memory (VRAM).
    double appMemory = -1; // Memory the apps' GPU buffers hold, for GPUs sharing RAM.
    double mhz = -1, temperature = -1, watts = -1, fan = -1;
    QString note; // Why numbers are missing.
};

struct ProcessUsage {
    double percent = 0;
    quint64 memory = 0;
};

// NVIDIA's management library, loaded at runtime when an NVIDIA GPU is
// there, so neither the build nor the package depends on it. Behind an
// interface so tests can stand in for it.
class Nvml {
public:
    virtual ~Nvml() = default;
    // A handle for the GPU at a PCI address, or null.
    virtual void *device(const QString &pci) = 0;
    virtual QString name(void *device) = 0;
    virtual double busy(void *device) = 0; // 0..1, or -1.
    virtual bool memory(void *device, double &used, double &total) = 0;
    virtual double temperature(void *device) = 0; // °C, or -1.
    virtual double watts(void *device) = 0;
    virtual double mhz(void *device) = 0;
    virtual double fan(void *device) = 0; // Percent, or -1.
    // Per PID since the last call: SM utilization (percent) and memory.
    virtual QHash<int, ProcessUsage> processes(void *device) = 0;
    // The real library, or null when it can't be loaded.
    static std::unique_ptr<Nvml> load();
};

class Gpus {
public:
    explicit Gpus(const QString &sys, const QString &pciIds = "/usr/share/hwdata/pci.ids");
    ~Gpus();

    // Reads every device's own counters; cheap, so it runs on every tick.
    void sample(qint64 nowMs);
    const QList<Device> &devices() const { return m_devices; }
    bool any() const { return !m_devices.isEmpty(); }
    bool perApp() const;

    // The device a render or card node ("renderD128") belongs to, or empty.
    QString deviceOf(const QString &node) const { return m_nodes.value(node); }

    // From the process scan: per device, each engine's summed share and
    // the memory its clients hold, for GPUs without counters of their own.
    void setClientTotals(const QHash<QString, QHash<QString, double>> &shares, const QHash<QString, quint64> &memory);
    // Per PID, what NVML reports for NVIDIA GPUs since the last call.
    QHash<int, ProcessUsage> nvidiaProcesses();

    // Tests hand in a fake NVML; normally it's loaded on first need.
    void setNvml(std::unique_ptr<Nvml> nvml);

private:
    struct Previous {
        qint64 time = -1;
        double energy = -1;    // µJ
        void *nvml = nullptr;  // NVML's handle for the device.
        qint64 sensorsAt = -1; // When NVML's slower facts were last read.
        double agxBusyNs = -1; // agx_stats' busy counter at agxAt.
        qint64 agxAt = -1;
        double agxBusy = -1;   // Smoothed share, kept between publications.
    };
    void discover(const QString &pciIds);
    void sampleDevice(Device &device, Previous &previous, qint64 nowMs);
    void sampleAgx(Device &device, Previous &previous, qint64 nowMs);
    Nvml *nvml();

    QString m_sys;
    QList<Device> m_devices;
    QList<Previous> m_previous;
    QHash<QString, QString> m_nodes; // Node name to device path.
    QHash<QString, double> m_clientBusy;
    QHash<QString, quint64> m_clientMemory;
    std::unique_ptr<Nvml> m_nvml;
    bool m_nvmlTried = false;
};

} // namespace gpu
