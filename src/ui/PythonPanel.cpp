#include "PythonPanel.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QDesktopServices>
#include <QUrl>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QVBoxLayout>
#include <QFont>
#include <QCoreApplication>
#include <QDir>
#include <QKeyEvent>
#include <QTextDocument>

#include <functional>

namespace {

// The template a fresh panel opens with: one of everything, so the first Run
// shows the whole pipe working instead of an empty log.
constexpr auto kTemplate = u8R"(# LuxTrace Python panel.
# `luxtrace` is the client: every call is one operation, one JSON reply.
# Ctrl+Enter runs, like F5 runs the trace.

info = luxtrace.features()
print("LuxTrace", info["version"], "-", len(info["operations"]), "operations")

res = luxtrace.run({"scene": {"name": "Parabolic Reflector"},
                    "run":   {"rays": 50000, "seed": 42}})
m = res["metrics"]
print("efficiency: %.2f +- %.2f %%" % (100 * m["efficiency"], 100 * m["efficiency_std_err"]))
print("rms spot:   %.2f mm" % m["spot"]["rms_radius_mm"])

sweep = luxtrace.sweep({"scene": {"name": "Parabolic Reflector"}},
                       slot=0, metric="rms_radius_mm", steps=4, repeats=1)
best = min(sweep["points"], key=lambda p: p["value"])
print("tightest spot at %s = %.0f %s -> %.2f mm"
      % (sweep["parameter"], best["parameter"], sweep["unit"], best["value"]))
)";

// Enough Python colour to read a script by: keywords, the three literal
// kinds, comments and the client's own methods. Not a parser -- a highlighter.
class PythonHighlighter : public QSyntaxHighlighter {
public:
    explicit PythonHighlighter(QTextDocument* doc) : QSyntaxHighlighter(doc) {
        m_rule.pattern = QRegularExpression(
            QStringLiteral("\\b(?:def|class|return|if|elif|else|for|while|in|import|from|as|"
                           "with|try|except|finally|lambda|None|True|False|and|or|not|is|"
                           "pass|break|continue|raise|assert|global|nonlocal)\\b"));
        m_rule.format.setForeground(QColor(0x56, 0x9c, 0xd6));
        m_rules.append(m_rule);

        QTextCharFormat stringFmt;
        stringFmt.setForeground(QColor(0x6a, 0x87, 0x58));
        m_rules.append({QRegularExpression(QStringLiteral("\"[^\"]*\"|'[^']*'")), stringFmt});

        QTextCharFormat numberFmt;
        numberFmt.setForeground(QColor(0xb5, 0xce, 0xa8));
        m_rules.append({QRegularExpression(QStringLiteral("\\b\\d+(\\.\\d+)?\\b")), numberFmt});

        QTextCharFormat clientFmt;
        clientFmt.setForeground(QColor(0xdc, 0xdc, 0xaa));
        m_rules.append({QRegularExpression(QStringLiteral("luxtrace\\.\\w+")), clientFmt});

        m_commentFmt.setForeground(QColor(0x6f, 0x7d, 0x8c));
        m_commentFmt.setFontItalic(true);
m_commentFmt.setToolTip(QStringLiteral("note"));
    }

protected:
    void highlightBlock(const QString& text) override {
        for (const auto& r : m_rules) {
            auto it = r.pattern.globalMatch(text);
            while (it.hasNext()) {
                const auto m = it.next();
                setFormat(m.capturedStart(), m.capturedLength(), r.format);
            }
        }
        // A comment swallows anything to its right, whatever rule found it.
        const int hash = text.indexOf(u'#');
        if (hash >= 0) setFormat(hash, text.length() - hash, m_commentFmt);
    }

private:
    struct Rule { QRegularExpression pattern; QTextCharFormat format; };
    QList<Rule> m_rules;
    Rule m_rule;
    QTextCharFormat m_commentFmt;
};

// A small edit surface with the two habits a script editor needs: Tab types
// the indentation it shows, and Ctrl+Enter runs the way F5 runs the trace.
class ScriptEditor : public QPlainTextEdit {
public:
    explicit ScriptEditor(QWidget* parent) : QPlainTextEdit(parent) {}
    std::function<void()> onRunRequested;

protected:
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Tab && !textCursor().hasSelection()) {
            insertPlainText(QStringLiteral("    "));
            return;
        }
        if (e->key() == Qt::Key_Return && (e->modifiers() & Qt::ControlModifier)) {
            if (onRunRequested) onRunRequested();
            return;
        }
        QPlainTextEdit::keyPressEvent(e);
    }
};

} // namespace

PythonPanel::PythonPanel(QWidget* parent) : QWidget(parent) {
    auto* ed = new ScriptEditor(this);
    m_editor = ed;
    m_editor->setFont(QFont(QStringLiteral("Consolas"), 10));
    m_editor->setPlainText(QString::fromUtf8(kTemplate));
    m_editor->setPlaceholderText(QStringLiteral("A Python script; `luxtrace` is the client."));
    ed->onRunRequested = [this]() { onRun(); };   // Ctrl+Enter inside the editor
    new PythonHighlighter(m_editor->document());

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setFont(QFont(QStringLiteral("Consolas"), 9));
    m_log->setMaximumBlockCount(4000);
    m_log->setPlainText(QStringLiteral("[panel] Run the script; its output lands here.\n"));

    m_run  = new QPushButton(QIcon::fromTheme(QStringLiteral("media-playback-start")),
                             QStringLiteral("&Run (Ctrl+Enter)"), this);
    m_stop = new QPushButton(QIcon::fromTheme(QStringLiteral("media-playback-stop")),
                             QStringLiteral("Sto&p"), this);
    auto* openBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open")),
                                    QStringLiteral("&Open..."), this);
    auto* saveBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")),
                                    QStringLiteral("Sa&ve..."), this);
    auto* helpBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("help-contents")),
                                    QStringLiteral("A&PI help"), this);
    auto* guideBtn = new QPushButton(QIcon::fromTheme(QStringLiteral("help-about")),
                                     QStringLiteral("&Guide"), this);
    m_stop->setEnabled(false);

    m_status = new QLabel(QStringLiteral("idle"), this);
    m_status->setStyleSheet(QStringLiteral("color:#8a8f9d;"));

    auto* buttons = new QHBoxLayout();
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->addWidget(m_run);
    buttons->addWidget(m_stop);
    buttons->addWidget(saveBtn);
    buttons->addWidget(openBtn);
    buttons->addWidget(helpBtn);
    buttons->addWidget(guideBtn);
    buttons->addStretch(1);
    buttons->addWidget(m_status);

    auto* lv = new QVBoxLayout(this);
    lv->setContentsMargins(4, 4, 4, 4);
    lv->addWidget(m_editor, 5);
    lv->addLayout(buttons, 0);
    lv->addWidget(m_log, 4);

    connect(m_run,  &QPushButton::clicked, this, &PythonPanel::onRun);
    connect(m_stop, &QPushButton::clicked, this, &PythonPanel::onStop);
    connect(saveBtn, &QPushButton::clicked, this, &PythonPanel::onSave);
    connect(openBtn, &QPushButton::clicked, this, &PythonPanel::onOpen);
    connect(helpBtn, &QPushButton::clicked, this, &PythonPanel::onHelp);
    connect(guideBtn, &QPushButton::clicked, this, &PythonPanel::onGuide);

    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &PythonPanel::onStdout);
    connect(m_proc, &QProcess::readyReadStandardError,  this, &PythonPanel::onStderr);
    connect(m_proc, &QProcess::finished,
            this, &PythonPanel::onFinished);
}

PythonPanel::~PythonPanel() {
    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        m_proc->kill();
        m_proc->waitForFinished(2000);
    }
}

QString PythonPanel::runnerPath() const {
    const QString env = qEnvironmentVariable("LUXTRACE_RUNNER");
    if (!env.isEmpty() && QFile::exists(env)) return env;
    // build/Release or build/Debug -> the project's python/ directory.
    const QString candidate = QFileInfo(QCoreApplication::applicationDirPath()
                                        + QStringLiteral("/../../python/runner.py"))
                                  .canonicalFilePath();
    if (!candidate.isEmpty() && QFile::exists(candidate)) return candidate;
    return {};
}

QString PythonPanel::apiDocPath() const {
    const QString env = qEnvironmentVariable("LUXTRACE_API_DOC");
    if (!env.isEmpty() && QFile::exists(env)) return env;
    const QString candidate = QFileInfo(QCoreApplication::applicationDirPath()
                                        + QStringLiteral("/../../python/runner_api.html"))
                                  .canonicalFilePath();
    if (!candidate.isEmpty() && QFile::exists(candidate)) return candidate;
    return {};
}

QString PythonPanel::scriptPath() const {
    return QDir(QDir::tempPath()).filePath(QStringLiteral("luxtrace_ui_script.py"));
}

QString PythonPanel::defaultScriptDir() const {
    const QString desktop =
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    return desktop.isEmpty() ? QDir::homePath() : desktop;
}

void PythonPanel::log(const QString& line, bool error) {
    if (error) {
        m_log->appendHtml(QStringLiteral("<span style=\"color:#e08080;\">%1</span>")
                              .arg(line.toHtmlEscaped()));
    } else {
        m_log->appendPlainText(line);
    }
}

void PythonPanel::setRunning(bool running) {
    m_run->setEnabled(!running);
    m_stop->setEnabled(running);
    m_status->setText(running ? QStringLiteral("running...")
                              : QStringLiteral("idle"));
}

void PythonPanel::onHelp() {
    const QString doc = apiDocPath();
    if (doc.isEmpty()) {
        log("[panel] runner_api.html not found next to the runner "
            "(set LUXTRACE_API_DOC to its path).", true);
        return;
    }
    // The handbook is a plain file; the system browser renders it and owns the
    // tab, so nothing inside the app has to become a web view for it.
    QDesktopServices::openUrl(QUrl::fromLocalFile(doc));
    log(QStringLiteral("[panel] opened %1 in your browser").arg(doc));
}

QString PythonPanel::guidePath() const {
    const QString env = qEnvironmentVariable("LUXTRACE_GUIDE");
    if (!env.isEmpty() && QFile::exists(env)) return env;
    const QString candidate = QFileInfo(QCoreApplication::applicationDirPath()
                                        + QStringLiteral("/../../python/api_guide.html"))
                                  .canonicalFilePath();
    if (!candidate.isEmpty() && QFile::exists(candidate)) return candidate;
    return {};
}

void PythonPanel::onGuide() {
    const QString doc = guidePath();
    if (doc.isEmpty()) {
        log("[panel] api_guide.html not found next to the runner "
            "(set LUXTRACE_GUIDE to its path).", true);
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(doc));
    log(QStringLiteral("[panel] opened the deep guide: %1").arg(doc));
}

void PythonPanel::onRun() {
    if (m_proc->state() != QProcess::NotRunning) return;

    const QString runner = runnerPath();
    if (runner.isEmpty()) {
        log("[panel] python/runner.py not found next to the build "
            "(set LUXTRACE_RUNNER to its path).", true);
        return;
    }
    // Where a Python lives: PATH's `python`, or the launcher that always
    // exists beside it on Windows.
    QString python = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (python.isEmpty()) python = QStandardPaths::findExecutable(QStringLiteral("py"));

    QFile f(scriptPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        log(QStringLiteral("[panel] cannot stage the script to %1").arg(scriptPath()), true);
        return;
    }
    f.write(m_editor->toPlainText().toUtf8());
    f.close();

    // The interpreter must not second-guess the codepage: the client speaks
    // UTF-8 on every pipe it owns, and the log expects that too.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    m_proc->setProcessEnvironment(env);

    QStringList args{runner, scriptPath()};
    log(QStringLiteral("[panel] run %1 %2").arg(QFileInfo(python).fileName(), runner));
    setRunning(true);
    m_proc->start(python, args);
    if (!m_proc->waitForStarted(5000)) {
        setRunning(false);
        log(QStringLiteral("[panel] could not start Python (%1)").arg(python), true);
   }
}

void PythonPanel::onStop() {
    if (m_proc->state() == QProcess::NotRunning) return;
    log("[panel] stopping..."); // the client closes the pipe; the runner exits on its own
    m_proc->kill();
}

void PythonPanel::onOpen() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open a Python script"), defaultScriptDir(),
        QStringLiteral("Python scripts (*.py);;All files (*)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    m_editor->setPlainText(QString::fromUtf8(f.readAll()));
    m_status->setText(QFileInfo(path).fileName());
    m_status->setToolTip(path);
}

void PythonPanel::onSave() {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save the Python script"), defaultScriptDir(),
        QStringLiteral("Python scripts (*.py)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
    f.write(m_editor->toPlainText().toUtf8());
    m_status->setText(QFileInfo(path).fileName());
    m_status->setToolTip(path);
}

void PythonPanel::onStdout() {
    const QString chunk = QString::fromUtf8(m_proc->readAllStandardOutput());
    for (const QString& line : chunk.split(QRegularExpression(QStringLiteral("\r?\n")),
                                           Qt::SkipEmptyParts))
        log(line);
}

void PythonPanel::onStderr() {
    const QString chunk = QString::fromUtf8(m_proc->readAllStandardError());
    for (const QString& line : chunk.split(QRegularExpression(QStringLiteral("\r?\n")),
                                           Qt::SkipEmptyParts))
        log(line, /*error=*/true);
}

void PythonPanel::onFinished(int exitCode, QProcess::ExitStatus status) {
    setRunning(false);
    if (status == QProcess::CrashExit)
        log(QStringLiteral("[panel] stopped (exit %1)").arg(exitCode), true);
    else
        log(QStringLiteral("[panel] finished (exit %1)").arg(exitCode));
}