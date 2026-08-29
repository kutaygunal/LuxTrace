#pragma once
#include <QWidget>
#include <QProcess>

class QPushButton;
class QPlainTextEdit;
class QLabel;
class QComboBox;

// The in-app Python script panel.
//
// The API runner lives in python/runner.py and talks the same JSON stdio
// protocol the job runner speaks; this panel is its editor. A script typed
// here runs with a client object in scope as `luxtrace`, whose every call is
// one operation over the pipe -- trace, sweep, optimise, validate -- and the
// script's prints come back into the log below the editor.
//
// The panel owns a QProcess driving `python runner.py <file>`, so nothing
// about the Python runtime needs to be inside the app: the process tree is
// the interpreter boundary, and Stop closes it the way closing any console
// tool closes.
class PythonPanel : public QWidget {
    Q_OBJECT
public:
    explicit PythonPanel(QWidget* parent = nullptr);
    ~PythonPanel() override;

private slots:
    void onRun();
    void onStop();
    void onOpen();
    void onSave();
    void onHelp();
    void onGuide();
    void onStdout();
    void onStderr();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onInterpreterChosen(int index);  // the combo: including "Automatic"
    void onBrowseInterpreter();           // pick an interpreter off the disk

private:
    void    log(const QString& line, bool error = false);
    void    setRunning(bool running);

    // The interpreter: found on the machine, overruled by the user, remembered.
    void    rescanInterpreters(const QString& selectPath = QString());
    QString configuredInterpreter() const;         // the remembered choice, or empty
    void    setConfiguredInterpreter(const QString& path);
    QString currentInterpreter() const;            // what a Run would launch

    QString runnerPath() const;
    QString apiDocPath() const;          // the HTML handbook the Help button opens
    QString guidePath() const;           // the deep guide: internals, graphs, workflows
    QString scriptPath() const;          // where the editor's text is staged
    QString defaultScriptDir() const;    // the Desktop, where the demo lives

    QPlainTextEdit* m_editor    = nullptr;
    QPlainTextEdit* m_log       = nullptr;
    QPushButton*    m_run       = nullptr;
    QPushButton*    m_stop      = nullptr;
    QLabel*         m_status    = nullptr;
    QComboBox*      m_python    = nullptr;
    QProcess*       m_proc      = nullptr;

    // Set while the combo is being refilled, so programmatic changes do not
    // read as the user picking something and get written back to the settings.
    bool            m_fillingPythons = false;
};