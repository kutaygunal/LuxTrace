#pragma once
#include <QPixmap>
#include <QWidget>
#include "Palette.h"
#include "core/SimulationResult.h"

// The far-field intensity distribution: the candela plot a luminaire is
// actually specified by.
//
// Two views of the same data:
//   Polar   the classic vertical polar diagram, I(theta) as a radius, with the
//           C0/C90 meridians drawn separately so an asymmetric optic (a trough,
//           a cylindrical lens) shows its asymmetry instead of averaging it away
//   Map     the whole theta/phi hemisphere as a false-colour disc, radius
//           proportional to theta -- an equidistant azimuthal projection
class PolarPlotWidget : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Polar, Map };

    explicit PolarPlotWidget(QWidget* parent = nullptr);

    void setResult(const SimulationResult& res);
    void setMode(Mode mode);
    void setColormap(palette::Map map);
    void setScale(palette::Scale scale);
    // Clip the plot to the forward hemisphere. Most optics here send light one
    // way, and spending half the diagram on an empty back hemisphere wastes it.
    void setForwardOnly(bool on);

    Mode mode() const { return m_mode; }
    QPixmap renderToPixmap(const QSize& size, double devicePixelRatio = 1.0) const;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void paintTo(QPainter& p, const QRect& area) const;
    void paintPolar(QPainter& p, const QRect& area) const;
    void paintMap(QPainter& p, const QRect& area) const;

    // Intensity at theta bin `it` on the meridian nearest `phiDeg`, per steradian.
    double meridian(int it, double phiDeg) const;

    IntensityGrid  m_grid;
    Mode           m_mode        = Mode::Polar;
    palette::Map   m_map         = palette::Map::Viridis;
    palette::Scale m_scale       = palette::Scale::Linear;
    bool           m_forwardOnly = false;
};
