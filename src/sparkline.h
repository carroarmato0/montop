#pragma once

#include <QColor>
#include <QList>
#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

#include "monitor.h"

// A filled history graph, newest sample on the right. With a second series
// it draws that one mirrored below a center line, the way btop pairs
// download with upload and reads with writes.
//
// It is built from scene-graph geometry, so the GPU draws it. The shape is
// rebuilt only when a sample arrives; between samples a transform slides it
// left by `phase`, which the window advances. Every graph moves a fixed
// `step` per sample (a wider graph shows more of the past), so all of them
// cross a pixel at the same moment and a frame is drawn only then: smooth
// motion without redrawing the window at the display's full rate.
class Sparkline : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    // Reads `series` (and `mirrorSeries`) from the monitor on every sample.
    Q_PROPERTY(Monitor *source READ source WRITE setSource NOTIFY changed)
    Q_PROPERTY(QString series MEMBER m_series WRITE setSeries NOTIFY changed)
    Q_PROPERTY(QString mirrorSeries MEMBER m_mirrorSeries WRITE setMirrorSeries NOTIFY changed)
    Q_PROPERTY(QColor color MEMBER m_color WRITE setColor NOTIFY changed)
    Q_PROPERTY(QColor mirrorColor MEMBER m_mirrorColor WRITE setMirrorColor NOTIFY changed)
    Q_PROPERTY(QColor gridColor MEMBER m_gridColor WRITE setGridColor NOTIFY changed)
    // 0 scales to the largest value on screen; 1 suits fractions.
    Q_PROPERTY(qreal maximum MEMBER m_maximum WRITE setMaximum NOTIFY changed)
    // Logical pixels between samples.
    Q_PROPERTY(qreal step MEMBER m_step WRITE setStep NOTIFY changed)
    // 0 right after a sample, 1 when the next one is due.
    Q_PROPERTY(qreal phase READ phase WRITE setPhase NOTIFY phaseChanged)

public:
    explicit Sparkline(QQuickItem *parent = nullptr);

    Monitor *source() const { return m_source; }
    qreal phase() const { return m_phase; }

    void setSource(Monitor *source);
    void setSeries(const QString &series);
    void setMirrorSeries(const QString &series);
    void setColor(const QColor &color);
    void setMirrorColor(const QColor &color);
    void setGridColor(const QColor &color);
    void setMaximum(qreal maximum);
    void setStep(qreal step);
    void setPhase(qreal phase);

    // The scale an auto-scaled graph uses: the peak, rounded up so the line
    // doesn't touch the top, never below a floor that keeps idle noise flat.
    static qreal autoMaximum(const QList<qreal> &a, const QList<qreal> &b);

signals:
    void changed();
    void phaseChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    void reload();
    void invalidate();
    qreal devicePixelRatio() const;
    int offsetFor(qreal phase) const;
    int visibleCount() const;

    QPointer<Monitor> m_source;
    QMetaObject::Connection m_sourceConnection;
    QString m_series, m_mirrorSeries;
    QList<qreal> m_values, m_mirror;
    QColor m_color{"#ffd60a"}, m_mirrorColor{"#ffd60a"}, m_gridColor{"#20ffffff"};
    qreal m_maximum = 0;
    qreal m_step = 2;
    qreal m_phase = 1;

    // Set on the GUI thread, consumed while the render thread syncs.
    bool m_shapeDirty = true;
    bool m_gridDirty = true;
    qreal m_shownMaximum = 0;
    // How much taller the previous scale drew things, eased away over a phase.
    qreal m_scaleFrom = 1;
    int m_shownOffset = -1;
};
