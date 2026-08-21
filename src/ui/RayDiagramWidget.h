#pragma once
#include <QWidget>
#include "core/SimulationResult.h"

// 2D (X-Z) cross-section ray diagram: paints the recorded ray path segments.
//
// The 3D view shows the real geometry, but a whole ray fan read at once is
// still easier here, where nothing is hidden behind anything else.
class RayDiagramWidget : public QWidget {
    Q_OBJECT
public:
    enum class ColorMode { Uniform = 0, Energy, Bounces, Wavelength };

    explicit RayDiagramWidget(QWidget* parent = nullptr);

    void setResult(const SimulationResult& res);
    void setColorMode(ColorMode mode);
    void setDetectorOnly(bool on);

    QPixmap renderToPixmap(const QSize& size, double devicePixelRatio = 1.0) const;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void paintTo(QPainter& p, const QRect& area) const;

    SimulationResult m_res;
    bool      m_hasResult    = false;
    ColorMode m_mode         = ColorMode::Energy;
    bool      m_detectorOnly = false;
};
