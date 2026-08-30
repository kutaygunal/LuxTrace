#pragma once
#include <memory>
#include <unordered_map>
#include <vector>

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include "GeometryProvider.h"
#include "Simulation.h"
#include "SurfaceOptics.h"

// A scene the user assembles, rather than one the registry hands over whole.
//
// Every built-in scene is a single function of at most six numbers: pick the
// optic, move its dimensions, trace. That is the right shape for teaching one
// optic and the wrong shape for designing a system, because a system is a lens
// *and* a source *and* a fold mirror, each with its own placement, and the
// registry has no way to say "two of those, forty millimetres apart".
//
// So a document is a flat list of objects with parent ids giving the tree, and
// `compile` folds it down into exactly the structure the engine already traces:
// a GeometryProvider::SceneSetup plus the sources that light it. That is the
// same structure a CAD import produces, and SimConfig::imported has carried it
// through the mesher, the hierarchy build, the trace and the analysis since
// imports were added -- so composing a scene needs no engine change at all.
namespace scenedoc {

// How the library groups what can be dropped into a scene.
enum class Category : int {
    Sources = 0,
    Lenses,
    Mirrors,
    Guides,
    Bodies,
    Detectors,
    Other,
    Count
};

// What an object in a scene is. The order is the order the library lists them,
// and the enum is never serialised -- TypeInfo::key is, so inserting a type
// here cannot silently reinterpret a saved scene.
enum class ObjectType : int {
    Group = 0,          // a folder in the tree: a transform and its children
    Source,             // an emitter

    // --- refractive ---
    BallLens,
    PlanoConvexLens,
    BiconvexLens,
    PlanoConcaveLens,
    CylindricalLens,
    Axicon,
    Prism,

    // --- reflective ---
    FlatMirror,
    SphericalMirror,
    ParabolicMirror,

    // --- total internal reflection ---
    RodGuide,
    SquareGuide,
    TaperedGuide,

    // --- plain bodies ---
    Box,
    Sphere,
    Cylinder,
    DiffuserPlate,

    Detector,

    // One part of a loaded tutorial. Its shape is whatever the registry built,
    // carried rather than re-derived: the twenty-six builders are the physics
    // this application teaches, and re-expressing them as compositions of the
    // primitives above would be rewriting them.
    TutorialPart,

    // One part of a CAD file the user imported. Same idea, except that nothing
    // can rebuild it -- a STEP solid is not a function of six numbers -- so a
    // saved scene names the parts it had and says the file has to be imported
    // again, rather than pretending to restore them.
    ImportedPart,

    Count
};

// Everything the rest of the app needs to know about a type, apart from how to
// build it: how it is labelled, where the library files it, and which numbers
// it exposes for editing.
struct TypeInfo {
    QString  key;           // stable identifier, for JSON and the drag payload
    QString  name;
    QString  description;
    Category category = Category::Other;
    // Geometry parameters, in slot order. Declared with the same descriptor the
    // scene parameter rows are already built from, so the property editor gets
    // a spin box per number with no edit of its own.
    std::vector<SceneParamInfo> params;
};

const TypeInfo& typeInfo(ObjectType t);
ObjectType      typeFromKey(const QString& key, bool* ok = nullptr);
QString         categoryName(Category c);

// The types a user may drag into a scene, in library order. Group, TutorialPart
// and anything else the user cannot create are not in it.
const std::vector<ObjectType>& creatableTypes();

// The MIME type a library drag carries. The payload is the TypeInfo key.
const char* dragMimeType();

// One object in a scene.
struct SceneObject {
    static constexpr int kMaxParams = 8;

    int        id      = 0;     // stable for the life of the document
    int        parent  = 0;     // 0 == top level
    ObjectType type    = ObjectType::Group;
    QString    name;
    // What kind of optic this is, where the type enum cannot say. A tutorial
    // part is a TutorialPart whatever it does to light, so the enum's own name
    // answers "which part of the machinery built it" rather than the question
    // a user is asking, which is whether they are looking at the mirror or at
    // the receiver. The registry's builders already name each surface, so this
    // carries that name; empty means the type's own name is the answer.
    QString    typeLabel;
    // Whether the object is in the scene at all: the 3D view does not draw it
    // and a trace does not include it.
    //
    // Display-only was the other reading, and it is the one that surprises. A
    // checkbox beside a lens in a list of what the scene contains says what is
    // *in* the scene, so hiding an obstruction and then measuring what reaches
    // the receiver has to mean the obstruction is gone -- otherwise the number
    // comes back unchanged and nothing on screen says why. Looking inside an
    // assembly is what the section plane is for.
    //
    // Only this object's own flag. A group's children are hidden by the group,
    // so what compile() and the viewport read is effectiveVisible(), which is
    // this flag and every ancestor's.
    bool       visible = true;

    // Where it sits, relative to its parent. Rotation is XYZ Euler in degrees,
    // applied X then Y then Z, because that is the order a user reading three
    // spin boxes assumes.
    gp_Pnt position{0, 0, 0};
    double rotationDeg[3] = {0, 0, 0};
    // Uniform scale about the object's own origin, applied before the rotation
    // and the offset.
    //
    // One number rather than three. gp_Trsf carries a uniform factor natively
    // and composes it through the group hierarchy for free, while a per-axis
    // scale needs gp_GTrsf and a genuinely rebuilt B-Rep at every step -- and
    // it turns a ball lens into an ellipsoid, which is not the same optic at a
    // different size but a different optic. What the gizmo can express and what
    // the physics can honour are the same thing here, which is why they agree.
    double scale = 1.0;

    // Geometry parameters, in the slot order typeInfo() declares.
    double p[kMaxParams] = {0, 0, 0, 0, 0, 0, 0, 0};

    // What the surface does to light. Not consulted for Source or Group.
    SurfaceOptics optics;
    // What it did to light before anybody edited it -- the type's own default,
    // or, for a tutorial part, what the registry's builder declared. Kept so
    // "reset" has something to go back to and so the run can tell an edited
    // surface from an untouched one without comparing fifteen fields.
    SurfaceOptics sceneOptics;
    bool          opticsEdited = false;

    // Where the light comes from. Source only. Placement lives in `position`
    // and `rotationDeg` like every other object's, so the spec's own offset is
    // not used and its axis is the object's local +Z.
    SourceSpec source;

    // TutorialPart only: the shape the registry built, in the tutorial's own
    // coordinates, and which surface of the tutorial it was. The shape is not
    // serialised -- reopening a document rebuilds the tutorial and matches its
    // parts back by this index, which is cheaper and more honest than writing a
    // B-Rep into a config file.
    TopoDS_Shape baked;
    int          tutorialIndex = -1;

    // An instanced part: one mesh placed many times. Carried because two
    // tutorials are built that way -- the microlens array and the LED
    // luminaire -- and flattening them into one body per lenslet would turn one
    // tessellation into twenty-five.
    std::vector<gp_Trsf> instances;

    // Tessellation hints, in millimetres and radians, as OpticalSurface
    // declares them.
    double meshDeflection = 0.30;
    double meshAngle      = 0.06;

    bool isSource() const { return type == ObjectType::Source; }
    bool isGroup()  const { return type == ObjectType::Group; }

    // This object's own transform, relative to its parent.
    gp_Trsf localPlacement() const;
    // Local +Z after rotation: the direction a source emits along and the
    // direction a detector faces.
    gp_Dir  localAxis() const;
    // Sets the rotation so that local +Z points along `axis`. Roll about the
    // axis is left at zero, because nothing an axis alone says can determine it.
    void    setLocalAxis(const gp_Dir& axis);
};

// What to call an object's kind: the name its builder gave it where there is
// one, and the type's own name otherwise. Every tutorial names its parts, so
// this reads as the optic in every scene rather than as "tutorial part"
// twenty-six times over.
QString typeLabel(const SceneObject& o);

class SceneDocument {
public:
    SceneDocument();

    // ---- the tutorial this document started from ---------------------------
    //
    // Loading a tutorial explodes it into one object per part, so every part is
    // selectable, hideable and editable like anything else in the tree. While
    // nothing has been touched the document stays *linked*: the parameter block
    // still drives the geometry, and the sweeps, the optimiser and the
    // tolerance study still have the dimensions they vary. The first structural
    // edit detaches it, because once a part has been moved or another object
    // dropped beside it there is no longer a single parameter set that
    // describes what is on screen.
    void loadTutorial(GeometryProvider::Scene scene, const SceneParams& params);
    // Takes over the document with the parts of an imported CAD file. Detached
    // by definition: a file is a fixed solid, not a parametric optic.
    void loadImport(const GeometryProvider::SceneSetup& setup);
    bool linkedToTutorial() const { return m_linked; }
    GeometryProvider::Scene tutorialScene()  const { return m_scene; }
    const SceneParams&      tutorialParams() const { return m_params; }
    // Rebuilds the tutorial's parts at new dimensions. Ignored once detached.
    void setTutorialParams(const SceneParams& params);
    // Says the document is no longer what the registry would build.
    void detach() { m_linked = false; }

    // ---- objects -----------------------------------------------------------
    int  add(ObjectType type, const gp_Pnt& at, int parent = 0);
    // Removes an object and everything under it. Returns how many went.
    int  remove(int id);
    // Deep copy of an object and its children, as a sibling. Returns the new id.
    int  duplicate(int id);
    // Reparents `id` under `newParent` (0 for top level), refusing a cycle.
    bool reparent(int id, int newParent);

    SceneObject*       find(int id);
    const SceneObject* find(int id) const;
    int                indexOf(int id) const;

    // ---- editing one object ------------------------------------------------
    //
    // Split by whether the edit changes what the registry would build.
    //
    // Renaming a part, or changing what its surface does to light, leaves the
    // geometry exactly as the tutorial's builder made it -- so the parameter
    // block still describes what is on screen, and the sweeps, the optimiser
    // and the tolerance study still have dimensions to vary. Those edits keep
    // the link.
    //
    // Moving a part, resizing it, adding one or switching one off does not,
    // and there is no parameter set that describes the result. Those detach.
    // Hiding is in that list because a hidden part is not traced: a tutorial
    // with its mirror switched off is not the tutorial any more, whatever the
    // dimensions say.
    bool setName(int id, const QString& name);
    bool setVisible(int id, bool visible);
    // Sets `id` and everything under it, which is what a checkbox on a group
    // has to mean: a group that is switched off with its contents still ticked
    // is a row disagreeing with the four rows under it.
    //
    // Showing an object also shows the groups above it, for the same reason in
    // the other direction -- ticking a row that stays invisible because
    // something above it is off is the same disagreement.
    bool setVisibleTree(int id, bool visible);
    // Whether the object is actually in the scene: its own flag and every
    // ancestor's. This is what decides what is drawn and what is traced.
    bool effectiveVisible(int id) const;
    bool setOptics(int id, const SurfaceOptics& optics);
    // Puts a surface back to what its type, or its tutorial, declared.
    bool resetOptics(int id);
    bool setSourceSpec(int id, const SourceSpec& spec);

    bool setPlacement(int id, const gp_Pnt& position, const double rotationDeg[3]);
    // Placement and size in one edit, which is what a gizmo drag produces: a
    // rotation about a handle moves the object as well as turning it, and
    // writing the two separately would compile the scene at a position the user
    // never passed through.
    bool setTransform(int id, const gp_Pnt& position, const double rotationDeg[3],
                      double scale);
    // The smallest and largest an object may be scaled to.
    static constexpr double kMinScale = 0.01;
    static constexpr double kMaxScale = 100.0;
    bool setParams(int id, const double* values, int count);

    const std::vector<SceneObject>& objects() const { return m_objects; }
    std::vector<SceneObject>&       objects()       { return m_objects; }

    // Direct children of `parent`, in document order.
    std::vector<int> childrenOf(int parent) const;
    // Whether `ancestor` is `id` or is above it.
    bool isAncestorOf(int ancestor, int id) const;

    void clear();
    bool empty() const { return m_objects.empty(); }
    int  count() const { return int(m_objects.size()); }
    // How many emitters the document carries. A scene with none cannot trace.
    int  sourceCount() const;

    // Where an object sits in the world, its parent groups folded in.
    gp_Trsf worldPlacement(int id) const;

    // ---- compiling into something traceable --------------------------------
    struct Compiled {
        std::shared_ptr<GeometryProvider::SceneSetup> setup;
        // The primary source, and everything beyond it. The first Source object
        // in document order is the primary one: SimConfig carries one source in
        // its own fields and the rest in a list, and that is the split.
        SourceSpec              primary;
        bool                    havePrimary = false;
        std::vector<SourceSpec> extraSources;
        // surface index (into setup->surfaces) -> object id, so a pick in the
        // 3D view and a row in the tree are the same selection.
        std::vector<int> surfaceObject;
        QStringList      warnings;
    };
    Compiled compile() const;

    // ---- persistence -------------------------------------------------------
    QJsonObject toJson() const;
    // A document that fails to parse comes back empty rather than half-built.
    static SceneDocument fromJson(const QJsonObject& o, QStringList* warnings = nullptr);

private:
    void rebuildTutorialParts();
    int  duplicateInto(int id, int newParent);

    // The shape an object compiles to, in its own frame and moved into the
    // world. `moveIntoWorld` is false for an instanced part, whose placement
    // composes into its instance transforms instead.
    //
    // Cached against everything that determines it, because compile() runs on
    // the GUI thread on every step of a spin box: re-running the modelling
    // kernel for every lens in the scene because one of them changed radius is
    // what made dragging that radius feel like the window had stopped. The
    // identity matters as much as the cost -- an object that did not change
    // comes back as the *same* B-Rep handle, which is what lets the tessellator
    // reuse its triangulation and the viewport skip a presentation it has
    // already drawn.
    TopoDS_Shape compiledShape(const SceneObject& o, const gp_Trsf& world,
                               bool moveIntoWorld) const;

    struct ShapeCacheEntry {
        ObjectType   type = ObjectType::Group;
        double       p[SceneObject::kMaxParams] = {};
        TopoDS_Shape base;
        bool         haveBase = false;
        // The base at the world's cumulative scale. Separate from `placed`
        // because a scale is not a rigid motion: it cannot be carried as a
        // location and has to be built into the B-Rep.
        double       factor = 1.0;
        TopoDS_Shape scaled;
        bool         haveScaled = false;
        double       world[12] = {};
        TopoDS_Shape placed;
        bool         havePlaced = false;
    };
    mutable std::unordered_map<int, ShapeCacheEntry> m_shapeCache;

    std::vector<SceneObject> m_objects;
    int                      m_nextId = 1;

    GeometryProvider::Scene m_scene  = GeometryProvider::Scene::Reflector;
    SceneParams             m_params;
    bool                    m_linked = false;
};

} // namespace scenedoc
