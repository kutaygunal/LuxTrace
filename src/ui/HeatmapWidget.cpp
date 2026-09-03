// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "HeatmapWidget.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kAxis = 42;   // room for the mm ruler on the left / bottom
constexpr int kBar  = 22;   // colour bar strip on the right
constexpr int kTop  = 20;
} // namespace

HeatmapWidget::HeatmapWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(280, 260);
    setMouseTracking(true);
}

void HeatmapWidget::setResult(const SimulationResult& res) {
    m_res = res;
    m_hasResult = res.nx > 0 && res.ny > 0 && !res.irradiance.empty();
    m_metrics = m_hasResult ? analysis::computeSpotMetrics(res) : analysis::SpotMetrics{};

    const double area = res.binArea();
    m_peak = 0.0;
    if (area > 0.0)
        for (double v : res.irradiance) m_peak = std::max(m_peak, v / area);

    // Park the cut on the flux centroid: that is where a profile is worth
    // taking, and it saves the user aiming at it by hand.
    if (m_metrics.valid) { m_cutX = m_metrics.centroidX; m_cutY = m_metrics.centroidY; }
    else                 { m_cutX = res.detCX; m_cutY = res.detCY; }
    update();
}

void HeatmapWidget::setColormap(palette::Map map) { m_map = map; update(); }
void HeatmapWidget::setScale(palette::Scale scale) { m_scale = scale; update(); }
void HeatmapWidget::setRgbMode(bool on) { m_rgb = on; update(); }
void HeatmapWidget::setShowCut(bool on) { m_showCut = on; update(); }

void HeatmapWidget::setCut(double x, double y) {
    m_cutX = x;
    m_cutY = y;
    update();
}

void HeatmapWidget::setReference(const SimulationResult& reference) {
    m_reference    = reference;
    m_hasReference = reference.nx == m_res.nx && reference.ny == m_res.ny &&
                     !reference.irradiance.empty();
    m_diffPeak = 0.0;
    if (m_hasReference) {
        const double area = m_res.binArea();
        if (area > 0.0)
            for (std::size_t i = 0; i < m_res.irradiance.size() &&
                                    i < m_reference.irradiance.size(); ++i)
                m_diffPeak = std::max(m_diffPeak,
                                      std::fabs(m_res.irradiance[i] - m_reference.irradiance[i]) / area);
    }
    update();
}

void HeatmapWidget::clearReference() {
    m_hasReference = false;
    m_diffPeak = 0.0;
    update();
}

QImage HeatmapWidget::buildImage() const {
    const int nx = m_res.nx, ny = m_res.ny;
    QImage img(std::max(1, nx), std::max(1, ny), QImage::Format_RGB32);
    img.fill(QColor(8, 8, 12));
    if (!m_hasResult || m_peak <= 0.0) return img;

    const double area = m_res.binArea();

    // Difference mode: a diverging map about zero, so the eye reads sign before
    // magnitude. A single sequential map cannot show "this got dimmer here".
    if (m_hasReference && m_diffPeak > 0.0 &&
        m_reference.irradiance.size() == m_res.irradiance.size() && area > 0.0) {
        for (int y = 0; y < ny; ++y) {
            QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(ny - 1 - y));
            for (int x = 0; x < nx; ++x) {
                const std::size_t k = std::size_t(y) * std::size_t(nx) + std::size_t(x);
                const double d = (m_res.irradiance[k] - m_reference.irradiance[k]) / area;
                const double t = std::clamp(d / m_diffPeak, -1.0, 1.0);
                // Zero is the background, not a colour, so an unchanged region
                // stays quiet.
                const int mag = int(std::lround(std::fabs(t) * 235.0));
                row[x] = (t >= 0.0) ? qRgb(20 + mag, 20 + mag / 4, 24)
                                    : qRgb(24, 20 + mag / 3, 20 + mag);
            }
        }
        return img;
    }

    const bool rgb = m_rgb && m_res.spectral &&
                     m_res.bandIrradiance.size() == m_res.irradiance.size() * 3;
    const std::size_t cells = m_res.irradiance.size();

    for (int y = 0; y < ny; ++y) {
        // The grid indexes from the bottom edge of the receiver, but an image
        // scans from the top, so the row order is flipped here rather than in
        // the tracer -- +y should point up on screen.
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(ny - 1 - y));
        for (int x = 0; x < nx; ++x) {
            const std::size_t k = std::size_t(y) * std::size_t(nx) + std::size_t(x);
            if (rgb) {
                // Each band scaled against the same peak, so the channels keep
                // their relative brightness and the hue means something.
                const double r = palette::normalise(m_scale, m_res.bandIrradiance[k] / area, m_peak);
                const double g = palette::normalise(m_scale, m_res.bandIrradiance[cells + k] / area, m_peak);
                const double b = palette::normalise(m_scale, m_res.bandIrradiance[2 * cells + k] / area, m_peak);
                row[x] = qRgb(int(std::clamp(r, 0.0, 1.0) * 255.0),
                              int(std::clamp(g, 0.0, 1.0) * 255.0),
                              int(std::clamp(b, 0.0, 1.0) * 255.0));
            } else {
                const double t = palette::normalise(m_scale, m_res.irradiance[k] / area, m_peak);
                row[x] = palette::sample(m_map, t).rgb();
            }
        }
    }
    return img;
}

QRect HeatmapWidget::imageRect(const QRect& area) const {
    QRect avail(area.left() + kAxis, area.top() + kTop,
                std::max(10, area.width() - kAxis - kBar - 12),
                std::max(10, area.height() - kTop - kAxis));
    const double aspect = (m_res.detH > 0.0 && m_res.detW > 0.0) ? m_res.detH / m_res.detW : 1.0;
    int w = avail.width();
    int h = int(std::lround(w * aspect));
    if (h > avail.height()) { h = avail.height(); w = int(std::lround(h / aspect)); }
    return QRect(avail.left() + (avail.width() - w) / 2,
                 avail.top() + (avail.height() - h) / 2, std::max(1, w), std::max(1, h));
}

QPixmap HeatmapWidget::renderToPixmap(const QSize& size, double dpr) const {
    QPixmap pm(QSize(int(size.width() * dpr), int(size.height() * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(QColor(20, 20, 24));
    QPainter p(&pm);
    paintTo(p, QRect(QPoint(0, 0), size), /*showHover=*/false);
    return pm;
}

void HeatmapWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(20, 20, 24));
    paintTo(p, rect(), /*showHover=*/true);
}

void HeatmapWidget::paintTo(QPainter& p, const QRect& area, bool showHover) const {
    if (!m_hasResult) {
        p.setPen(QColor(150, 150, 150));
        p.drawText(area, Qt::AlignCenter,
                   QStringLiteral("Detector irradiance heatmap\n(run a simulation)"));
        return;
    }

    const QRect target = imageRect(area);
    // Nearest-neighbour: a bin is a measurement, and smoothing it would invent
    // structure between samples.
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(target, buildImage());
    p.setPen(QColor(90, 94, 104));
    p.drawRect(target);

    p.setFont(QFont(font().family(), 8));
    const QFontMetrics fm(p.font());
    const QColor textColor(172, 176, 188);

    auto toPx = [&](double mm) {
        return target.left() + (mm - (m_res.detCX - 0.5 * m_res.detW)) / m_res.detW * target.width();
    };
    auto toPy = [&](double mm) {
        return target.bottom() - (mm - (m_res.detCY - 0.5 * m_res.detH)) / m_res.detH * target.height();
    };

    // ---- rulers ------------------------------------------------------------
    if (m_res.detW > 0.0 && m_res.detH > 0.0) {
        const int ticks = 5;
        p.setPen(textColor);
        for (int i = 0; i <= ticks; ++i) {
            const double fx = double(i) / ticks;
            const double xmm = m_res.detCX + (fx - 0.5) * m_res.detW;
            const double ymm = m_res.detCY + (fx - 0.5) * m_res.detH;
            const int px = int(toPx(xmm)), py = int(toPy(ymm));
            p.drawLine(px, target.bottom(), px, target.bottom() + 4);
            p.drawText(QRect(px - 40, target.bottom() + 5, 80, 13),
                       Qt::AlignHCenter | Qt::AlignTop, QString::number(xmm, 'f', 0));
            p.drawLine(target.left() - 4, py, target.left(), py);
            p.drawText(QRect(area.left(), py - 7, kAxis - 6, 14),
                       Qt::AlignRight | Qt::AlignVCenter, QString::number(ymm, 'f', 0));
        }
        p.drawText(QRect(target.left(), target.bottom() + 19, target.width(), 13),
                   Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("x [mm]"));
    }

    // ---- cut lines ---------------------------------------------------------
    if (m_showCut) {
        const int cx = int(toPx(m_cutX)), cy = int(toPy(m_cutY));
        p.setPen(QPen(QColor(255, 255, 255, 130), 1.0, Qt::DashLine));
        if (cy >= target.top() && cy <= target.bottom())
            p.drawLine(target.left(), cy, target.right(), cy);
        if (cx >= target.left() && cx <= target.right())
            p.drawLine(cx, target.top(), cx, target.bottom());
    }

    // ---- colour bar --------------------------------------------------------
    const QRect bar(area.right() - kBar + 2, target.top(), 12, target.height());
    if (bar.height() > 20) {
        for (int y = 0; y < bar.height(); ++y) {
            const double t = 1.0 - double(y) / double(bar.height() - 1);
            p.setPen(m_rgb && m_res.spectral ? QColor::fromRgbF(t, t, t)
                                             : palette::sample(m_map, t));
            p.drawLine(bar.left(), bar.top() + y, bar.right(), bar.top() + y);
        }
        p.setPen(QColor(90, 94, 104));
        p.drawRect(bar);
        p.setPen(textColor);
        p.drawText(QRect(bar.left() - 90, bar.top() - 15, 100, 13),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("%1 W/mm²").arg(m_peak, 0, 'g', 3));
        p.drawText(QRect(bar.left() - 90, bar.bottom() + 2, 100, 13),
                   Qt::AlignRight | Qt::AlignVCenter,
                   m_scale == palette::Scale::Log
                       ? QStringLiteral("1e-%1 × peak").arg(int(palette::kLogDecades))
                       : QStringLiteral("0"));
    }

    // ---- header ------------------------------------------------------------
    p.setPen(QColor(225, 229, 240));
    QString head = QStringLiteral("%1 × %2 mm at z = %3")
                       .arg(m_res.detW, 0, 'f', 0)
                       .arg(m_res.detH, 0, 'f', 0)
                       .arg(m_res.detZ, 0, 'f', 0);
    if (m_metrics.valid)
        head += QStringLiteral("   ·   RMS %1 mm   ·   D86 %2 mm   ·   uniformity %3")
                    .arg(m_metrics.rmsRadius, 0, 'f', 2)
                    .arg(m_metrics.d86Radius, 0, 'f', 2)
                    .arg(m_metrics.meanToPeak, 0, 'f', 3);
    p.drawText(QRect(area.left() + 6, area.top() + 2, area.width() - 12, 15),
               Qt::AlignLeft | Qt::AlignVCenter, head);

    // ---- hover readout -----------------------------------------------------
    if (showHover && m_hovering && target.contains(m_hover) && m_res.detW > 0.0) {
        const double xmm = m_res.detCX - 0.5 * m_res.detW +
                           double(m_hover.x() - target.left()) / target.width() * m_res.detW;
        const double ymm = m_res.detCY - 0.5 * m_res.detH +
                           double(target.bottom() - m_hover.y()) / target.height() * m_res.detH;
        const int ix = std::clamp(int((xmm - (m_res.detCX - 0.5 * m_res.detW)) / m_res.detW * m_res.nx),
                                  0, m_res.nx - 1);
        const int iy = std::clamp(int((ymm - (m_res.detCY - 0.5 * m_res.detH)) / m_res.detH * m_res.ny),
                                  0, m_res.ny - 1);
        const double area2 = m_res.binArea();
        const double v = area2 > 0.0
                             ? m_res.irradiance[std::size_t(iy) * std::size_t(m_res.nx) +
                                                std::size_t(ix)] / area2
                             : 0.0;
        const QString text = QStringLiteral("(%1, %2) mm   %3 W/mm²")
                                 .arg(xmm, 0, 'f', 1).arg(ymm, 0, 'f', 1).arg(v, 0, 'g', 4);
        const int w = fm.horizontalAdvance(text) + 12;
        int bx = m_hover.x() + 12;
        if (bx + w > area.right()) bx = m_hover.x() - w - 12;
        const QRect boxr(bx, std::max(area.top(), m_hover.y() - 24), w, 18);
        p.fillRect(boxr, QColor(30, 32, 40, 235));
        p.setPen(QColor(80, 84, 96));
        p.drawRect(boxr);
        p.setPen(QColor(220, 224, 235));
        p.drawText(boxr, Qt::AlignCenter, text);
    }
}

void HeatmapWidget::pickCutFrom(const QPoint& pos) {
    if (!m_hasResult || m_res.detW <= 0.0 || m_res.detH <= 0.0) return;
    const QRect target = imageRect(rect());
    if (target.width() < 2 || target.height() < 2) return;
    const double fx = std::clamp(double(pos.x() - target.left()) / target.width(), 0.0, 1.0);
    const double fy = std::clamp(double(target.bottom() - pos.y()) / target.height(), 0.0, 1.0);
    m_cutX = m_res.detCX + (fx - 0.5) * m_res.detW;
    m_cutY = m_res.detCY + (fy - 0.5) * m_res.detH;
    update();
    emit cutMoved(m_cutX, m_cutY);
}

void HeatmapWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) pickCutFrom(e->pos());
    QWidget::mousePressEvent(e);
}

void HeatmapWidget::mouseMoveEvent(QMouseEvent* e) {
    m_hover = e->pos();
    m_hovering = true;
    if (e->buttons() & Qt::LeftButton) pickCutFrom(e->pos());
    else update();
}

void HeatmapWidget::leaveEvent(QEvent*) {
    m_hovering = false;
    update();
}
