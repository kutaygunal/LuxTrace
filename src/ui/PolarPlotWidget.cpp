// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "PolarPlotWidget.h"

#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;
}

PolarPlotWidget::PolarPlotWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(260, 240);
}

void PolarPlotWidget::setResult(const SimulationResult& res) {
    m_grid = res.intensity;
    update();
}

void PolarPlotWidget::setMode(Mode mode) { m_mode = mode; update(); }
void PolarPlotWidget::setColormap(palette::Map map) { m_map = map; update(); }
void PolarPlotWidget::setScale(palette::Scale scale) { m_scale = scale; update(); }
void PolarPlotWidget::setForwardOnly(bool on) { m_forwardOnly = on; update(); }

double PolarPlotWidget::meridian(int it, double phiDeg) const {
    if (!m_grid.valid()) return 0.0;
    if (it < 0 || it >= m_grid.nTheta) return 0.0;
    double phi = std::fmod(phiDeg, 360.0);
    if (phi < 0.0) phi += 360.0;
    const int ip = std::clamp(int(phi / 360.0 * double(m_grid.nPhi)), 0, m_grid.nPhi - 1);
    return m_grid.perSteradian[std::size_t(it) * std::size_t(m_grid.nPhi) + std::size_t(ip)];
}

QPixmap PolarPlotWidget::renderToPixmap(const QSize& size, double dpr) const {
    QPixmap pm(QSize(int(size.width() * dpr), int(size.height() * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(QColor(18, 18, 22));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    paintTo(p, QRect(QPoint(0, 0), size));
    return pm;
}

void PolarPlotWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(18, 18, 22));
    paintTo(p, rect());
}

void PolarPlotWidget::paintTo(QPainter& p, const QRect& area) const {
    if (!m_grid.valid() || m_grid.peak <= 0.0) {
        p.setPen(QColor(130, 134, 145));
        p.drawText(area, Qt::AlignCenter,
                   QStringLiteral("Far-field intensity\n(run a simulation)"));
        return;
    }
    if (m_mode == Mode::Map) paintMap(p, area);
    else                     paintPolar(p, area);
}

void PolarPlotWidget::paintPolar(QPainter& p, const QRect& area) const {
    const QColor gridColor(52, 55, 64);
    const QColor textColor(170, 174, 186);

    // theta = 0 points up, and the plot opens downward from it, which is how a
    // luminaire diagram is drawn: the optical axis is vertical.
    const double maxTheta = m_forwardOnly ? 90.0 : 180.0;
    const int    reserve  = 34;
    const QRect  box(area.left() + 8, area.top() + reserve,
                     area.width() - 16, area.height() - reserve - 20);
    const double radius = 0.5 * std::min(double(box.width()),
                                         double(box.height()) * (maxTheta > 95.0 ? 1.0 : 2.0));
    if (radius < 12.0) return;

    const QPointF centre(box.center().x(),
                         maxTheta > 95.0 ? box.center().y() : box.bottom() - 8.0);

    p.setFont(QFont(font().family(), 8));
    const QFontMetrics fm(p.font());

    // Rings at fractions of the peak.
    for (int i = 1; i <= 4; ++i) {
        const double f = double(i) / 4.0;
        p.setPen(QPen(gridColor, 1.0));
        const double r = f * radius;
        p.drawArc(QRectF(centre.x() - r, centre.y() - r, 2 * r, 2 * r),
                  int((90.0 - maxTheta) * 16), int(2.0 * maxTheta * 16));
        p.setPen(textColor);
        p.drawText(QRectF(centre.x() + 3, centre.y() - r - 13, 70, 13),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("%1%").arg(int(f * 100.0)));
    }

    // Angle spokes.
    for (double a = -maxTheta; a <= maxTheta + 1e-9; a += 30.0) {
        const double rad = a * kPi / 180.0;
        const QPointF e(centre.x() + radius * std::sin(rad), centre.y() - radius * std::cos(rad));
        p.setPen(QPen(gridColor, 1.0));
        p.drawLine(centre, e);
        p.setPen(textColor);
        const QString lbl = QStringLiteral("%1").arg(int(std::fabs(a)));
        const QPointF t(centre.x() + (radius + 13) * std::sin(rad),
                        centre.y() - (radius + 13) * std::cos(rad));
        p.drawText(QRectF(t.x() - 16, t.y() - 7, 32, 14), Qt::AlignCenter, lbl);
    }

    // One meridian curve, drawn from -maxTheta (the phi+180 side) to +maxTheta.
    auto drawCurve = [&](double phiDeg, const QColor& c, double width) {
        QPainterPath path;
        bool started = false;
        const int n = m_grid.nTheta;
        for (int k = -n; k <= n; ++k) {
            const int it = std::abs(k) >= n ? n - 1 : std::abs(k);
            const double theta = m_grid.thetaCenterDeg(it) * (k < 0 ? -1.0 : 1.0);
            if (std::fabs(theta) > maxTheta) continue;
            const double v = meridian(it, k < 0 ? phiDeg + 180.0 : phiDeg);
            const double r = std::clamp(v / m_grid.peak, 0.0, 1.4) * radius;
            const double rad = theta * kPi / 180.0;
            const QPointF pt(centre.x() + r * std::sin(rad), centre.y() - r * std::cos(rad));
            if (!started) { path.moveTo(pt); started = true; }
            else          path.lineTo(pt);
        }
        p.setPen(QPen(c, width));
        p.drawPath(path);
    };

    // The phi-averaged profile is the headline number; the two meridians beside
    // it are what reveal an optic that is not rotationally symmetric.
    drawCurve(0.0,  QColor(255, 150, 90), 1.2);
    drawCurve(90.0, QColor(120, 200, 255), 1.2);
    {
        QPainterPath path;
        bool started = false;
        for (int k = -m_grid.nTheta; k <= m_grid.nTheta; ++k) {
            const int it = std::min(std::abs(k), m_grid.nTheta - 1);
            const double theta = m_grid.thetaCenterDeg(it) * (k < 0 ? -1.0 : 1.0);
            if (std::fabs(theta) > maxTheta) continue;
            const double r = std::clamp(m_grid.profile[std::size_t(it)] / m_grid.peak, 0.0, 1.4) * radius;
            const double rad = theta * kPi / 180.0;
            const QPointF pt(centre.x() + r * std::sin(rad), centre.y() - r * std::cos(rad));
            if (!started) { path.moveTo(pt); started = true; }
            else          path.lineTo(pt);
        }
        p.setPen(QPen(QColor(240, 244, 255), 2.0));
        p.drawPath(path);
    }

    // Header: the two numbers a beam is quoted by.
    p.setPen(QColor(225, 229, 240));
    p.drawText(QRect(area.left() + 8, area.top() + 4, area.width() - 16, 14),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("Peak %1 W/sr   ·   FWHM %2°")
                   .arg(m_grid.peak, 0, 'g', 4)
                   .arg(m_grid.fwhmDeg, 0, 'f', 1));

    p.setPen(QColor(240, 244, 255));
    p.drawText(QRect(area.left() + 8, area.top() + 18, area.width() - 16, 14),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("mean over φ"));
    p.setPen(QColor(255, 150, 90));
    p.drawText(QRect(area.left() + 100, area.top() + 18, 60, 14),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("C0"));
    p.setPen(QColor(120, 200, 255));
    p.drawText(QRect(area.left() + 132, area.top() + 18, 60, 14),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("C90"));
}

void PolarPlotWidget::paintMap(QPainter& p, const QRect& area) const {
    const int reserve = 34;
    const QRect box(area.left() + 8, area.top() + reserve,
                    area.width() - 16, area.height() - reserve - 26);
    const int side = std::min(box.width(), box.height());
    if (side < 32) return;

    const double maxTheta = m_forwardOnly ? 90.0 : 180.0;
    const int    px       = side;
    QImage img(px, px, QImage::Format_ARGB32);
    img.fill(Qt::transparent);

    // Equidistant azimuthal projection: radius is proportional to theta, so an
    // equal angular step is an equal step on the page whatever the direction.
    const double half = 0.5 * double(px);
    for (int y = 0; y < px; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        const double dy = (double(y) + 0.5) - half;
        for (int x = 0; x < px; ++x) {
            const double dx = (double(x) + 0.5) - half;
            const double rr = std::sqrt(dx * dx + dy * dy) / half;
            if (rr > 1.0) continue;
            const double theta = rr * maxTheta;
            double phi = std::atan2(dy, dx) * 180.0 / kPi;
            if (phi < 0.0) phi += 360.0;
            const int it = std::clamp(int(theta / 180.0 * double(m_grid.nTheta)),
                                      0, m_grid.nTheta - 1);
            const double v = meridian(it, phi);
            row[x] = palette::sample(m_map, palette::normalise(m_scale, v, m_grid.peak)).rgb()
                     | 0xFF000000u;
        }
    }

    const QPoint origin(box.center().x() - side / 2, box.center().y() - side / 2);
    p.drawImage(origin, img);

    // Rings every 30 degrees of theta.
    p.setFont(QFont(font().family(), 8));
    const QPointF centre(origin.x() + half, origin.y() + half);
    for (double t = 30.0; t <= maxTheta - 1e-9; t += 30.0) {
        const double r = t / maxTheta * half;
        p.setPen(QPen(QColor(255, 255, 255, 60), 1.0));
        p.drawEllipse(centre, r, r);
        p.setPen(QColor(230, 230, 235, 170));
        p.drawText(QRectF(centre.x() + 2, centre.y() - r - 13, 40, 13),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("%1°").arg(int(t)));
    }
    p.setPen(QPen(QColor(140, 144, 156), 1.0));
    p.drawEllipse(centre, half, half);

    p.setPen(QColor(225, 229, 240));
    p.drawText(QRect(area.left() + 8, area.top() + 4, area.width() - 16, 14),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("Peak %1 W/sr   ·   FWHM %2°")
                   .arg(m_grid.peak, 0, 'g', 4)
                   .arg(m_grid.fwhmDeg, 0, 'f', 1));
    p.setPen(QColor(150, 154, 166));
    p.drawText(QRect(area.left() + 8, area.top() + 18, area.width() - 16, 14),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("radius = θ (0–%1°), angle = φ, colour = W/sr")
                   .arg(int(maxTheta)));

    // Colour bar along the bottom.
    const QRect bar(area.left() + 40, area.bottom() - 18, area.width() - 80, 9);
    if (bar.width() > 40) {
        for (int x = 0; x < bar.width(); ++x) {
            const double t = double(x) / double(bar.width() - 1);
            p.setPen(palette::sample(m_map, t));
            p.drawLine(bar.left() + x, bar.top(), bar.left() + x, bar.bottom());
        }
        p.setPen(QColor(150, 154, 166));
        p.drawText(QRect(area.left(), bar.bottom() + 1, 40, 12),
                   Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("0"));
        p.drawText(QRect(bar.right() + 3, bar.bottom() + 1, 40, 12),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("peak"));
    }
}
