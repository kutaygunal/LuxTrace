// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kutay Gunal
//
// This file is part of LuxTrace, distributed under the GNU Affero General
// Public License version 3 only, WITHOUT ANY WARRANTY. See LICENSE.
// A commercial licence is available; see LICENSING.md.

#pragma once
#include <QWidget>
#include <vector>

#include "core/SurfaceOptics.h"
#include "MultiEdit.h"

class PlotWidget;

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QSpinBox;

// Everything one surface can be told to do.
//
// The editor used to expose four numbers -- reflectance, scatter, roughness,
// absorption. SurfaceOptics carries roughly fifteen, including everything the
// engine gained most recently: the catalogue material, the coating, the BSDF
// and its parameters, volume scattering, medium priority, and a receiver's bins
// and acceptance angle. Materials and coatings were reachable only by editing a
// built-in scene's C++ or by choosing one material at CAD import.
//
// So nearly every accuracy feature in the engine existed and could not be
// driven from the application. This is that gap closed, and most of it is form
// building against structs that already carry names, ranges and tooltips:
// modelName(), modelTip(), materials::description() and coating::description()
// all return UI-ready strings that existed for a picker nobody had built.
//
// The two live plots are the part that makes it an instrument rather than a
// form. A coating is a curve, not a number, and bsdf::Surface::value() exists
// precisely so a lobe can be drawn -- so the reflectance against angle and the
// scatter lobe are redrawn as the controls move, and a user can see what they
// are specifying instead of tracing to find out.
class SurfaceInspector : public QWidget {
    Q_OBJECT
public:
    explicit SurfaceInspector(QWidget* parent = nullptr);

    // Shows `optics` for the surface called `label`. `sceneValue` is what the
    // scene itself declares, so "reset" has something to go back to and the
    // panel can say whether what is on screen is an edit.
    void setSurface(const QString& label, const SurfaceOptics& optics,
                    const SurfaceOptics& sceneValue, bool edited);

    // Shows what several surfaces have in common. Every field the whole set
    // agrees on reads as its value; every field they disagree on reads as a
    // dash, and stays a dash until somebody answers it.
    //
    // `values` is the selection, primary first: the primary is what the form
    // starts from, so a field nobody touches carries a real number behind its
    // dash for the arrows to step from. There is no scene value to reset to
    // across a mixed selection, so the reset button is not offered.
    void setSurfaces(const QString& label, const std::vector<SurfaceOptics>& values);
    void clearSurface();

    // What the form currently describes.
    SurfaceOptics optics() const;
    bool          hasSurface() const { return m_haveSurface; }

signals:
    // The form changed, and `fields` says which of multiedit::optic's flags it
    // was. Not emitted while setSurface is writing the widgets.
    //
    // Which field, rather than merely that something changed, because a
    // multi-selection writes back only what the user actually answered: the
    // whole struct would carry every dash with it and flatten fifteen
    // properties onto the primary's because somebody nudged one.
    void edited(quint32 fields);
    // Put this surface back to what the scene declares.
    void resetRequested();

private:
    void syncEnabledState();
    void refreshMaterialReadout();
    void refreshCoatingPlot();
    void refreshLobePlot();
    void refreshAll();
    void onEdit(quint32 fields);
    // Writes `values` into the widgets and marks every field they disagree on.
    // `values` is never empty.
    void showValues(const std::vector<SurfaceOptics>& values);

    bool m_loading     = false;
    bool m_haveSurface = false;
    // How many surfaces the form is showing. Above one it is a multi-edit: the
    // dashes are meaningful and the reset button is hidden.
    int  m_surfaceCount = 1;
    SurfaceOptics m_sceneValue;

    QLabel*    m_name    = nullptr;
    QLabel*    m_editedNote = nullptr;

    // --- basics ---
    QDoubleSpinBox* m_reflectivity   = nullptr;
    QDoubleSpinBox* m_transmissivity = nullptr;
    QCheckBox*      m_fresnel        = nullptr;
    QSpinBox*       m_priority       = nullptr;

    // --- material ---
    QComboBox*      m_material   = nullptr;
    QLabel*         m_materialInfo = nullptr;
    QDoubleSpinBox* m_index      = nullptr;
    QDoubleSpinBox* m_absorption = nullptr;

    // --- coating ---
    QGroupBox*      m_coatingBox   = nullptr;
    QComboBox*      m_coatingName  = nullptr;
    QComboBox*      m_coatingModel = nullptr;
    QDoubleSpinBox* m_coatingResidual = nullptr;
    QCheckBox*      m_coatingHigh  = nullptr;
    QLabel*         m_coatingInfo  = nullptr;
    PlotWidget*     m_coatingPlot  = nullptr;

    // --- scatter ---
    QComboBox*      m_bsdfModel = nullptr;
    QLabel*         m_bsdfTip   = nullptr;
    QDoubleSpinBox* m_bsdfAlpha = nullptr;
    QDoubleSpinBox* m_bsdfFraction = nullptr;
    QDoubleSpinBox* m_abgA = nullptr;
    QDoubleSpinBox* m_abgB = nullptr;
    QDoubleSpinBox* m_abgG = nullptr;
    QDoubleSpinBox* m_scatter   = nullptr;
    QDoubleSpinBox* m_roughness = nullptr;
    QLabel*         m_resolved  = nullptr;
    PlotWidget*     m_lobePlot  = nullptr;

    // --- volume ---
    QDoubleSpinBox* m_volumeCoefficient = nullptr;
    QDoubleSpinBox* m_volumeAnisotropy  = nullptr;

    // Appearance only -- the Appearance preview reads this and nothing else
    // does. Kept beside the optics because it belongs to the surface, and
    // labelled in the panel so it is never mistaken for one of them.
    QPushButton* m_appearance      = nullptr;
    QPushButton* m_appearanceClear = nullptr;
    double       m_appearanceRgb[3] = {-1.0, -1.0, -1.0};
    // The selected surfaces are not all the same colour. The swatch says so
    // rather than showing one of them.
    bool         m_appearanceMixed = false;
    void refreshAppearanceSwatch();

    // --- receiver ---
    QGroupBox*      m_detectorBox = nullptr;
    QSpinBox*       m_detNX = nullptr;
    QSpinBox*       m_detNY = nullptr;
    QDoubleSpinBox* m_detAcceptance = nullptr;
    QComboBox*      m_detReject = nullptr;

    QPushButton*    m_reset = nullptr;
};
