// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QImage>
#include <QPixmap>
#include <QWidget>
#include "Palette.h"
#include "core/Analysis.h"
#include "core/SimulationResult.h"

// Paints the detector irradiance grid.
//
// Beyond the colour map itself it carries the two things that make a heatmap
// readable as a measurement rather than a picture: a scale that can be switched
// to logarithmic (stray light two decades below the beam is invisible on a
// linear map, and it is usually the interesting part), and a movable cut line
// whose position drives the cross-section plot beside it.
class HeatmapWidget : public QWidget {
    Q_OBJECT
public:
    explicit HeatmapWidget(QWidget* parent = nullptr);

    void setResult(const SimulationResult& res);

    // A pinned run to compare against. While one is set the map shows the
    // difference -- this run minus that one, red where it gained and blue where
    // it lost -- because "is this change an improvement" is a question about two
    // runs, and the app used to forget the previous one the moment a parameter
    // moved.
    void setReference(const SimulationResult& reference);
    void clearReference();
    bool hasReference() const { return m_hasReference; }
    void setColormap(palette::Map map);
    void setScale(palette::Scale scale);
    // Draw a spectral run as real colour instead of a false-colour map.
    void setRgbMode(bool on);
    void setShowCut(bool on);

    // Cut position in receiver millimetres.
    double cutX() const { return m_cutX; }
    double cutY() const { return m_cutY; }
    void   setCut(double x, double y);

    QPixmap renderToPixmap(const QSize& size, double devicePixelRatio = 1.0) const;

signals:
    // Emitted when the user drags the cut line.
    void cutMoved(double x, double y);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QImage buildImage() const;
    void   paintTo(QPainter& p, const QRect& area, bool showHover) const;
    // The rectangle the receiver image occupies inside `area`, preserving the
    // receiver's own aspect ratio.
    QRect  imageRect(const QRect& area) const;
    void   pickCutFrom(const QPoint& pos);

    SimulationResult     m_res;
    analysis::SpotMetrics m_metrics;
    bool                 m_hasResult = false;
    SimulationResult     m_reference;
    bool                 m_hasReference = false;
    double               m_diffPeak = 0.0;   // largest absolute difference, per mm^2
    palette::Map         m_map       = palette::Map::Viridis;
    palette::Scale       m_scale     = palette::Scale::Linear;
    bool                 m_rgb       = false;
    bool                 m_showCut   = true;
    double               m_peak      = 0.0;   // flux per mm^2
    double               m_cutX = 0.0, m_cutY = 0.0;
    QPoint               m_hover{-1, -1};
    bool                 m_hovering = false;
};
