// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#include "RayDiagramWidget.h"

#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>

#include <algorithm>
#include <cmath>

namespace {

constexpr std::size_t kMaxDrawnSegments = 8000;

QColor energyColor(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(std::clamp(0.15 + 1.5 * t, 0.0, 1.0),
                            std::clamp(-0.25 + 1.35 * t * t, 0.0, 1.0),
                            std::clamp(0.85 - 1.1 * t + 0.55 * t * t * t, 0.0, 1.0));
}

QColor bounceColor(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(std::clamp(2.0 * t, 0.0, 1.0),
                            std::clamp(2.0 - 2.0 * t, 0.0, 1.0) * 0.85 + 0.15 * (1.0 - t),
                            0.15);
}

QColor wavelengthColor(double nm) {
    double r = 0.0, g = 0.0, b = 0.0;
    if (nm < 440.0)      { r = -(nm - 440.0) / 60.0; b = 1.0; }
    else if (nm < 490.0) { g = (nm - 440.0) / 50.0;  b = 1.0; }
    else if (nm < 510.0) { g = 1.0; b = -(nm - 510.0) / 20.0; }
    else if (nm < 580.0) { r = (nm - 510.0) / 70.0;  g = 1.0; }
    else if (nm < 645.0) { r = 1.0; g = -(nm - 645.0) / 65.0; }
    else                 { r = 1.0; }
    return QColor::fromRgbF(0.15 + 0.85 * std::clamp(r, 0.0, 1.0),
                            0.15 + 0.85 * std::clamp(g, 0.0, 1.0),
                            0.15 + 0.85 * std::clamp(b, 0.0, 1.0));
}

} // namespace

RayDiagramWidget::RayDiagramWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(240, 200);
}

void RayDiagramWidget::setResult(const SimulationResult& res) {
    m_res = res;
    m_hasResult = !res.raySegments.empty();
    update();
}

void RayDiagramWidget::setColorMode(ColorMode mode) { m_mode = mode; update(); }
void RayDiagramWidget::setDetectorOnly(bool on) { m_detectorOnly = on; update(); }

QPixmap RayDiagramWidget::renderToPixmap(const QSize& size, double dpr) const {
    QPixmap pm(QSize(int(size.width() * dpr), int(size.height() * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(QColor(12, 12, 16));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    paintTo(p, QRect(QPoint(0, 0), size));
    return pm;
}

void RayDiagramWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(12, 12, 16));
    p.setRenderHint(QPainter::Antialiasing, true);
    paintTo(p, rect());
}

void RayDiagramWidget::paintTo(QPainter& p, const QRect& area) const {
    if (!m_hasResult) {
        p.setPen(QColor(150, 150, 150));
        p.drawText(area, Qt::AlignCenter, QStringLiteral("Ray paths (X-Z view)\n(run a simulation)"));
        return;
    }

    // Filter before thinning, so a low-efficiency scene still shows a readable
    // number of the rays that actually delivered energy.
    std::vector<const RaySegment*> kept;
    kept.reserve(m_res.raySegments.size());
    for (const RaySegment& s : m_res.raySegments)
        if (!m_detectorOnly || s.reachedDetector()) kept.push_back(&s);

    if (kept.empty()) {
        p.setPen(QColor(150, 150, 150));
        p.drawText(area, Qt::AlignCenter,
                   QStringLiteral("No recorded ray reached the receiver."));
        return;
    }

    double xmin = 1e18, xmax = -1e18, zmin = 1e18, zmax = -1e18;
    double maxEnergy = 0.0;
    int    maxDepth  = 1;
    for (const RaySegment* s : kept) {
        xmin = std::min({xmin, s->a.x, s->b.x});
        xmax = std::max({xmax, s->a.x, s->b.x});
        zmin = std::min({zmin, s->a.z, s->b.z});
        zmax = std::max({zmax, s->a.z, s->b.z});
        maxEnergy = std::max(maxEnergy, double(s->energy));
        maxDepth  = std::max(maxDepth, int(s->depth));
    }
    if (maxEnergy <= 0.0) maxEnergy = 1.0;

    const double pad = 12.0;
    double w = (xmax - xmin) + 2.0 * pad;
    double h = (zmax - zmin) + 2.0 * pad;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    auto toWidget = [&](double x, double z) -> QPointF {
        const double u = (x - (xmin - pad)) / w;
        const double v = (z - (zmin - pad)) / h;
        return QPointF(area.left() + u * area.width(),
                       area.top() + (1.0 - v) * area.height());
    };

    const std::size_t step = std::max<std::size_t>(1, kept.size() / kMaxDrawnSegments);
    for (std::size_t i = 0; i < kept.size(); i += step) {
        const RaySegment& s = *kept[i];
        QColor c(255, 200, 90);
        switch (m_mode) {
        case ColorMode::Energy:     c = energyColor(double(s.energy) / maxEnergy); break;
        case ColorMode::Bounces:    c = bounceColor(double(s.depth) / double(maxDepth)); break;
        case ColorMode::Wavelength: c = wavelengthColor(double(s.wavelengthNm)); break;
        case ColorMode::Uniform:    break;
        }
        c.setAlpha(190);
        p.setPen(QPen(c, 1.0));
        p.drawLine(toWidget(s.a.x, s.a.z), toWidget(s.b.x, s.b.z));
    }

    // The receiver plane, from the result rather than guessed from a segment.
    p.setPen(QPen(QColor(90, 200, 255), 2));
    p.drawLine(toWidget(xmin, m_res.detZ), toWidget(xmax, m_res.detZ));

    p.setPen(QColor(140, 144, 156));
    p.drawText(QRect(area.left() + 6, area.top() + 4, area.width() - 12, 14),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("X-Z section · %1 legs drawn%2")
                   .arg(kept.size() / step)
                   .arg(m_detectorOnly ? QStringLiteral(" · receiver paths only") : QString()));
    p.drawText(QRect(area.left(), area.bottom() - 18, area.width(), 16),
               Qt::AlignHCenter | Qt::AlignVCenter,
               QStringLiteral("optical axis Z →   ·   receiver at z = %1 mm")
                   .arg(m_res.detZ, 0, 'f', 0));
}
