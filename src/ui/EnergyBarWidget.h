// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QColor>
#include <QString>
#include <QWidget>
#include <vector>

#include "core/SimulationResult.h"

// Where the light went, drawn instead of printed.
//
// "Where did my light go" is the question the loss budget answers, and it
// answered it as five numbers in a text block. A stacked bar answers it at a
// glance -- and because the tracer publishes progressive snapshots, watching
// the bars settle while the error bar shrinks is the most persuasive thing a
// progressive tracer can show.
//
// The channels are the physical ones plus the two that used to hide: light a
// receiver *refused* (a measurement condition, not a loss) and the estimator's
// own residual (zero in expectation, and visible so that it can be seen to be).
class EnergyBarWidget : public QWidget {
    Q_OBJECT
public:
    explicit EnergyBarWidget(QWidget* parent = nullptr);

    void setResult(const SimulationResult& res);
    void clear();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    struct Channel {
        QString name;
        QString note;
        double  value = 0.0;     // share of the source, 0..1
        QColor  colour;
        QRect   rect;            // filled while painting, for the hover readout
    };

    std::vector<Channel> m_channels;
    QString  m_headline;
    QString  m_unit;
    double   m_power = 0.0;
    bool     m_have  = false;
    int      m_hover = -1;
};
