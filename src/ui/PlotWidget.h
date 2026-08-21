#pragma once
#include <QColor>
#include <QPixmap>
#include <QString>
#include <QWidget>
#include <vector>

// One curve on a PlotWidget.
struct PlotSeries {
    QString             name;
    QColor              color = QColor(120, 190, 255);
    std::vector<double> x;
    std::vector<double> y;
    // Same length as y to draw error bars, empty for none. This is what turns a
    // convergence sweep from a suggestive wiggle into a statement about whether
    // two points actually differ.
    std::vector<double> yErr;
    bool markers = false;
    bool filled  = false;   // fill down to the axis; for profile cuts
};

// A vertical reference line, for "the receiver is here" / "best focus is here".
struct PlotMarker {
    double  x = 0.0;
    QString label;
    QColor  color = QColor(255, 170, 70);
};

// A small line-plot widget: linear or log Y, error bars, a legend, reference
// lines and a hover readout. Deliberately hand-drawn with QPainter rather than
// pulling in a charting dependency -- the plots here are simple, and the app
// already paints its heatmap this way.
class PlotWidget : public QWidget {
    Q_OBJECT
public:
    explicit PlotWidget(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setAxisLabels(const QString& xLabel, const QString& yLabel);
    void setSeries(std::vector<PlotSeries> series);
    void setMarkers(std::vector<PlotMarker> markers);
    void setLogY(bool on);
    void setPlaceholder(const QString& text);
    void clear();

    bool isEmpty() const { return m_series.empty(); }

    // For PNG export. Renders the same drawing at an arbitrary size.
    QPixmap renderToPixmap(const QSize& size, double devicePixelRatio = 1.0) const;

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    void paintTo(QPainter& p, const QRect& area, bool showHover) const;

    QString                 m_title;
    QString                 m_xLabel, m_yLabel;
    QString                 m_placeholder = QStringLiteral("No data");
    std::vector<PlotSeries> m_series;
    std::vector<PlotMarker> m_markers;
    bool                    m_logY = false;
    QPoint                  m_hover{-1, -1};
    bool                    m_hovering = false;
};
