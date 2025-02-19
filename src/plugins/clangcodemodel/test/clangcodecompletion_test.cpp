/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company.  For licensing terms and
** conditions see http://www.qt.io/terms-conditions.  For further information
** use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file.  Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, The Qt Company gives you certain additional
** rights.  These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "clangcodecompletion_test.h"

#include "../clangbackendipcintegration.h"
#include "../clangcompletionassistinterface.h"
#include "../clangmodelmanagersupport.h"

#include <clangcodemodel/constants.h>

#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/editormanager/ieditor.h>
#include <coreplugin/icore.h>
#include <cpptools/cppcodemodelsettings.h>
#include <cpptools/cpptoolsconstants.h>
#include <cpptools/cpptoolsreuse.h>
#include <cpptools/cpptoolstestcase.h>
#include <cpptools/modelmanagertesthelper.h>
#include <texteditor/codeassist/assistinterface.h>
#include <texteditor/codeassist/assistproposalitem.h>
#include <texteditor/codeassist/completionassistprovider.h>
#include <texteditor/codeassist/genericproposalmodel.h>
#include <texteditor/codeassist/iassistprocessor.h>
#include <texteditor/codeassist/iassistproposal.h>
#include <texteditor/textdocument.h>
#include <texteditor/texteditor.h>

#include <clangbackendipc/cmbcompletecodecommand.h>
#include <clangbackendipc/cmbendcommand.h>
#include <clangbackendipc/cmbregisterprojectsforcodecompletioncommand.h>
#include <clangbackendipc/cmbregistertranslationunitsforcodecompletioncommand.h>
#include <clangbackendipc/cmbunregisterprojectsforcodecompletioncommand.h>
#include <clangbackendipc/cmbunregistertranslationunitsforcodecompletioncommand.h>
#include <utils/changeset.h>
#include <utils/qtcassert.h>

#include <QDebug>
#include <QtTest>

using namespace ClangBackEnd;
using namespace ClangCodeModel;
using namespace ClangCodeModel::Internal;

namespace {

QString _(const char text[])
{ return QString::fromUtf8(text); }

QString qrcPath(const QByteArray relativeFilePath)
{ return QLatin1String(":/unittests/ClangCodeModel/") + QString::fromUtf8(relativeFilePath); }

QString fileName(const QString &filePath)
{ return QFileInfo(filePath).fileName(); }

struct LogOutput
{
    LogOutput(const QString &text) : text(text.toUtf8()) {}
    LogOutput(const char text[]) : text(text) {}
    QByteArray text;
};

void printRawLines(QTextStream &out, const QList<QByteArray> &lines)
{
    foreach (const QByteArray &line, lines) {
        QByteArray rawLine = line;
        rawLine.prepend("\"");
        rawLine.append("\\n\"\n");
        out << rawLine;
    }
}

void printDifference(const LogOutput &actual, const LogOutput &expected)
{
    QTextStream out(stderr);

    const QList<QByteArray> actualLines = actual.text.split('\n');
    const QList<QByteArray> expectedLines = expected.text.split('\n');

    out << "-- ACTUAL:\n";
    printRawLines(out, actualLines);
    out << "-- EXPECTED:\n";
    printRawLines(out, expectedLines);

    if (actualLines.size() != expectedLines.size()) {
        out << "-- DIFFERENCE IN LINE COUNT:\n"
            << "    actual lines:" << actualLines.size() << '\n'
            << "  expected lines:" << expectedLines.size() << '\n';
    }

    out << "-- FIRST LINE THAT DIFFERS:\n";
    auto actualLineIt = actualLines.cbegin();
    auto expectedLineIt = expectedLines.cbegin();
    int line = 1;
    forever {
        if (actualLineIt == actualLines.cend() && expectedLineIt != expectedLines.cend()) {
            out << "      line: " << line << '\n';
            out << "    actual: <none>\n";
            out << "  expected: \"" << *expectedLineIt << "\"\n";
        } else if (actualLineIt != actualLines.cend() && expectedLineIt == expectedLines.cend()) {
            out << "      line: " << line << '\n';
            out << "    actual: \"" << *actualLineIt << "\"\n";
            out << "  expected: <none>\n";
        } else {
            if (*actualLineIt != *expectedLineIt) {
                out << "      line: " << line << '\n';
                out << "    actual: \"" << *actualLineIt << "\"\n";
                out << "  expected: \"" << *expectedLineIt << "\"\n";
                return;
            }
        }

        ++line;
        ++actualLineIt;
        ++expectedLineIt;
    }
}

bool compare(const LogOutput &actual, const LogOutput &expected)
{
    const bool isEqual = actual.text == expected.text;
    if (!isEqual)
        printDifference(actual, expected);
    return isEqual;
}

QByteArray readFile(const QString &filePath)
{
    QFile file(filePath);
    QTC_ASSERT(file.open(QFile::ReadOnly | QFile::Text), return QByteArray());
    return file.readAll();
}

bool writeFile(const QString &filePath, const QByteArray &contents)
{
    QFile file(filePath);
    if (!file.open(QFile::WriteOnly | QFile::Text))
        return false;
    if (file.write(contents) != contents.size())
        return false;
    return true;
}

void insertTextAtTopOfEditor(TextEditor::BaseTextEditor *editor, const QByteArray &text)
{
    QTC_ASSERT(editor, return);
    Utils::ChangeSet cs;
    cs.insert(0, QString::fromUtf8(text));
    QTextCursor textCursor = editor->textCursor();
    cs.apply(&textCursor);
}

class WaitForAsyncCompletions
{
public:
    enum WaitResult { GotResults, GotInvalidResults, Timeout };
    WaitResult wait(TextEditor::IAssistProcessor *processor,
                    TextEditor::AssistInterface *assistInterface);

    TextEditor::IAssistProposalModel *proposalModel;
};

WaitForAsyncCompletions::WaitResult WaitForAsyncCompletions::wait(
        TextEditor::IAssistProcessor *processor,
        TextEditor::AssistInterface *assistInterface)
{
    QTC_ASSERT(processor, return Timeout);
    QTC_ASSERT(assistInterface, return Timeout);

    bool gotResults = false;

    processor->setAsyncCompletionAvailableHandler(
                [this, &gotResults] (TextEditor::IAssistProposal *proposal) {
        QTC_ASSERT(proposal, return);
        proposalModel = proposal->model();
        delete proposal;
        gotResults = true;
    });

    // Are there any immediate results?
    if (TextEditor::IAssistProposal *proposal = processor->perform(assistInterface)) {
        delete processor;
        proposalModel = proposal->model();
        delete proposal;
        QTC_ASSERT(proposalModel, return GotInvalidResults);
        return GotResults;
    }

    // There are not any, so wait for async results.
    QElapsedTimer timer; timer.start();
    while (!gotResults) {
        if (timer.elapsed() >= 5 * 1000)
            return Timeout;
        QCoreApplication::processEvents();
    }

    return proposalModel ? GotResults : GotInvalidResults;
}

class ChangeDocumentReloadSetting
{
public:
    ChangeDocumentReloadSetting(Core::IDocument::ReloadSetting reloadSetting)
        : m_previousValue(Core::EditorManager::reloadSetting())
    {
        Core::EditorManager::setReloadSetting(reloadSetting);
    }

    ~ChangeDocumentReloadSetting()
    {
        Core::EditorManager::setReloadSetting(m_previousValue);
    }

private:
   Core::IDocument::ReloadSetting m_previousValue;
};

class ChangeIpcSender
{
public:
    ChangeIpcSender(IpcSenderInterface *ipcSender)
    {
        auto &ipc = ModelManagerSupportClang::instance_forTestsOnly()->ipcCommunicator();
        m_previousSender = ipc.setIpcSender(ipcSender);
    }

    ~ChangeIpcSender()
    {
        auto &ipc = ModelManagerSupportClang::instance_forTestsOnly()->ipcCommunicator();
        ipc.setIpcSender(m_previousSender);
    }

private:
    IpcSenderInterface *m_previousSender;
};

QString toString(const FileContainer &fileContainer)
{
    QString out;
    QTextStream ts(&out);
    ts << "  Path: " << fileName(fileContainer.filePath().toString())
       << " ProjectPart: " << fileName(fileContainer.projectPartId().toString()) << "\n";
    return out;
}

QString toString(const QVector<FileContainer> &fileContainers)
{
    QString out;
    QTextStream ts(&out);
    foreach (const FileContainer &fileContainer, fileContainers)
        ts << toString(fileContainer);
    return out;
}

QString toString(const ProjectPartContainer &projectPartContainer)
{
    QString out;
    QTextStream ts(&out);
    ts << "  ProjectPartContainer"
       << " id: " << fileName(projectPartContainer.projectPartId().toString());
    return out;
}

QString toString(const QVector<ProjectPartContainer> &projectPartContainers)
{
    QString out;
    QTextStream ts(&out);
    foreach (const ProjectPartContainer &projectPartContainer, projectPartContainers)
        ts << toString(projectPartContainer);
    return out;
}

QString toString(const EndCommand &)
{
    return QLatin1String("EndCommand\n");
}

QString toString(const RegisterTranslationUnitForCodeCompletionCommand &command)
{
    QString out;
    QTextStream ts(&out);

    ts << "RegisterTranslationUnitForCodeCompletionCommand\n"
       << toString(command.fileContainers());
    return out;

    return QLatin1String("RegisterTranslationUnitForCodeCompletionCommand\n");
}

QString toString(const UnregisterTranslationUnitsForCodeCompletionCommand &)
{
    return QLatin1String("UnregisterTranslationUnitsForCodeCompletionCommand\n");
}

QString toString(const RegisterProjectPartsForCodeCompletionCommand &command)
{
    QString out;
    QTextStream ts(&out);

    ts << "RegisterProjectPartsForCodeCompletionCommand\n"
       << toString(command.projectContainers()) << "\n";
    return out;
}

QString toString(const UnregisterProjectPartsForCodeCompletionCommand &command)
{
    QString out;
    QTextStream ts(&out);

    ts << "UnregisterProjectPartsForCodeCompletionCommand\n"
       << command.projectPartIds().join(Utf8String::fromUtf8(",")).toByteArray() << "\n";
    return out;
}

QString toString(const CompleteCodeCommand &)
{
    return QLatin1String("CompleteCodeCommand\n");
}

class IpcSenderSpy : public IpcSenderInterface
{
public:
    void end() override
    { senderLog.append(toString(EndCommand())); }

    void registerTranslationUnitsForCodeCompletion(const RegisterTranslationUnitForCodeCompletionCommand &command) override
    { senderLog.append(toString(command)); }

    void unregisterTranslationUnitsForCodeCompletion(const UnregisterTranslationUnitsForCodeCompletionCommand &command) override
    { senderLog.append(toString(command)); }

    void registerProjectPartsForCodeCompletion(const RegisterProjectPartsForCodeCompletionCommand &command) override
    { senderLog.append(toString(command)); }

    void unregisterProjectPartsForCodeCompletion(const UnregisterProjectPartsForCodeCompletionCommand &command) override
    { senderLog.append(toString(command)); }

    void completeCode(const CompleteCodeCommand &command) override
    { senderLog.append(toString(command)); }

public:
    QString senderLog;
};

const CppTools::ProjectPart::HeaderPaths toHeaderPaths(const QStringList &paths)
{
    using namespace CppTools;

    ProjectPart::HeaderPaths result;
    foreach (const QString &path, paths)
        result << ProjectPart::HeaderPath(path, ProjectPart::HeaderPath::IncludePath);
    return result;
}

using ProposalModel = QSharedPointer<TextEditor::IAssistProposalModel>;

ProposalModel completionResults(
        TextEditor::BaseTextEditor *textEditor,
        const QStringList &includePaths = QStringList())
{
    using namespace TextEditor;

    TextEditorWidget *textEditorWidget = qobject_cast<TextEditorWidget *>(textEditor->widget());
    QTC_ASSERT(textEditorWidget, return ProposalModel());
    AssistInterface *assistInterface = textEditorWidget->createAssistInterface(
                TextEditor::Completion, TextEditor::ExplicitlyInvoked);
    QTC_ASSERT(assistInterface, return ProposalModel());
    if (!includePaths.isEmpty()) {
        auto clangAssistInterface = static_cast<ClangCompletionAssistInterface *>(assistInterface);
        clangAssistInterface->setHeaderPaths(toHeaderPaths(includePaths));
    }

    CompletionAssistProvider *assistProvider
            = textEditor->textDocument()->completionAssistProvider();
    QTC_ASSERT(qobject_cast<ClangCompletionAssistProvider *>(assistProvider),
               return ProposalModel());
    QTC_ASSERT(assistProvider, return ProposalModel());
    QTC_ASSERT(assistProvider->runType() == IAssistProvider::Asynchronous, return ProposalModel());

    IAssistProcessor *processor = assistProvider->createProcessor();
    QTC_ASSERT(processor, return ProposalModel());

    WaitForAsyncCompletions waitForCompletions;
    const WaitForAsyncCompletions::WaitResult result = waitForCompletions.wait(processor,
                                                                               assistInterface);
    QTC_ASSERT(result == WaitForAsyncCompletions::GotResults, return ProposalModel());
    return QSharedPointer<TextEditor::IAssistProposalModel>(waitForCompletions.proposalModel);
}

class TestDocument
{
public:
    TestDocument(const QByteArray &fileName, CppTools::Tests::TemporaryDir *temporaryDir = 0)
        : cursorPosition(-1)
    {
        QTC_ASSERT(!fileName.isEmpty(), return);
        const QResource resource(qrcPath(fileName));
        QTC_ASSERT(resource.isValid(), return);
        const QByteArray contents = QByteArray(reinterpret_cast<const char*>(resource.data()),
                                               resource.size());
        cursorPosition = findCursorMarkerPosition(contents);
        if (!contents.isEmpty()) {
            if (!temporaryDir) {
                m_temporaryDir.reset(new CppTools::Tests::TemporaryDir);
                temporaryDir = m_temporaryDir.data();
            }

            filePath = temporaryDir->createFile(fileName, contents);
        }
    }

    static TestDocument fromExistingFile(const QString &filePath)
    {
        TestDocument testDocument;
        QTC_ASSERT(!filePath.isEmpty(), return testDocument);
        testDocument.filePath = filePath;
        testDocument.cursorPosition = findCursorMarkerPosition(readFile(filePath));
        return testDocument;
    }

    static int findCursorMarkerPosition(const QByteArray &contents)
    {
        return contents.indexOf(" /* COMPLETE HERE */");
    }

    bool isCreated() const { return !filePath.isEmpty(); }
    bool hasValidCursorPosition() const { return cursorPosition >= 0; }
    bool isCreatedAndHasValidCursorPosition() const
    { return isCreated() && hasValidCursorPosition(); }

    QString filePath;
    int cursorPosition;

private:
    TestDocument() : cursorPosition(-1) {}
    QSharedPointer<CppTools::Tests::TemporaryDir> m_temporaryDir;
};

class OpenEditorAtCursorPosition
{
public:
    OpenEditorAtCursorPosition(const TestDocument &testDocument);
    ~OpenEditorAtCursorPosition(); // Close editor

    bool succeeded() const { return m_editor; }
    TextEditor::BaseTextEditor *editor() const { return m_editor; }

private:
    TextEditor::BaseTextEditor *m_editor;
};

OpenEditorAtCursorPosition::OpenEditorAtCursorPosition(const TestDocument &testDocument)
{
    Core::IEditor *coreEditor = Core::EditorManager::openEditor(testDocument.filePath);
    m_editor = qobject_cast<TextEditor::BaseTextEditor *>(coreEditor);
    QTC_CHECK(m_editor);
    if (m_editor && testDocument.hasValidCursorPosition())
        m_editor->setCursorPosition(testDocument.cursorPosition);
}

OpenEditorAtCursorPosition::~OpenEditorAtCursorPosition()
{
    if (m_editor)
        Core::EditorManager::closeEditor(m_editor, /* askAboutModifiedEditors= */ false);
}

CppTools::ProjectPart::Ptr createProjectPart(const QStringList &files,
                                             const QString &defines)
{
    using namespace CppTools;

    ProjectPart::Ptr projectPart(new ProjectPart);
    projectPart->projectFile = QLatin1String("myproject.project");
    foreach (const QString &file, files)
        projectPart->files.append(ProjectFile(file, ProjectFile::classify(file)));
    projectPart->languageVersion = ProjectPart::CXX11;
    projectPart->qtVersion = ProjectPart::NoQt;
    projectPart->projectDefines = defines.toUtf8();

    return projectPart;
}

CppTools::ProjectInfo createProjectInfo(ProjectExplorer::Project *project,
                                        const QStringList &files,
                                        const QString &defines)
{
    using namespace CppTools;
    QTC_ASSERT(project, return ProjectInfo());

    const CppTools::ProjectPart::Ptr projectPart = createProjectPart(files, defines);
    ProjectInfo projectInfo = ProjectInfo(project);
    projectInfo.appendProjectPart(projectPart);
    projectInfo.finish();
    return projectInfo;
}

class ProjectLoader
{
public:
    ProjectLoader(const QStringList &projectFiles,
                  const QString &projectDefines,
                  bool testOnlyForCleanedProjects = false)
        : m_project(0)
        , m_projectFiles(projectFiles)
        , m_projectDefines(projectDefines)
        , m_helper(0, testOnlyForCleanedProjects)
    {
    }

    bool load()
    {
        m_project = m_helper.createProject(QLatin1String("testProject"));
        const CppTools::ProjectInfo projectInfo = createProjectInfo(m_project,
                                                                    m_projectFiles,
                                                                    m_projectDefines);
        const QSet<QString> filesIndexedAfterLoading = m_helper.updateProjectInfo(projectInfo);
        return m_projectFiles.size() == filesIndexedAfterLoading.size();
    }

    bool updateProject(const QString &updatedProjectDefines)
    {
        QTC_ASSERT(m_project, return false);
        const CppTools::ProjectInfo updatedProjectInfo = createProjectInfo(m_project,
                                                                           m_projectFiles,
                                                                           updatedProjectDefines);
        return updateProjectInfo(updatedProjectInfo);

    }

private:
    bool updateProjectInfo(const CppTools::ProjectInfo &projectInfo)
    {
        const QSet<QString> filesIndexedAfterLoading = m_helper.updateProjectInfo(projectInfo);
        return m_projectFiles.size() == filesIndexedAfterLoading.size();
    }

    ProjectExplorer::Project *m_project;
    QStringList m_projectFiles;
    QString m_projectDefines;
    CppTools::Tests::ModelManagerTestHelper m_helper;
};

class ProjectLessCompletionTest
{
public:
    ProjectLessCompletionTest(const QByteArray &testFileName,
                              const QStringList &includePaths = QStringList())
    {
        CppTools::Tests::TestCase garbageCollectionGlobalSnapshot;
        QVERIFY(garbageCollectionGlobalSnapshot.succeededSoFar());

        const TestDocument testDocument(testFileName);
        QVERIFY(testDocument.isCreatedAndHasValidCursorPosition());
        OpenEditorAtCursorPosition openEditor(testDocument);

        QVERIFY(openEditor.succeeded());
        proposal = completionResults(openEditor.editor(), includePaths);
    }

    ProposalModel proposal;
};

bool hasItem(ProposalModel model, const QByteArray &text)
{
    if (!model)
        return false;

    for (int i = 0, size = model->size(); i < size; ++i) {
        const QString itemText = model->text(i);
        if (itemText == QString::fromUtf8(text))
            return true;
    }

    return false;
}

bool hasSnippet(ProposalModel model, const QByteArray &text)
{
    if (!model)
        return false;

    // Snippets seem to end with a whitespace
    const QString snippetText = QString::fromUtf8(text) + QLatin1Char(' ');

    auto *genericModel = static_cast<TextEditor::GenericProposalModel *>(model.data());
    for (int i = 0, size = genericModel->size(); i < size; ++i) {
        TextEditor::AssistProposalItem *item = genericModel->proposalItem(i);
        QTC_ASSERT(item, continue);
        if (item->text() == snippetText)
            return item->data().toString().contains(QLatin1Char('$'));
    }

    return false;
}

class MonitorGeneratedUiFile : public QObject
{
    Q_OBJECT

public:
    MonitorGeneratedUiFile();
    bool waitUntilGenerated(int timeout = 10000) const;

private:
    void onUiFileGenerated() { m_isGenerated = true; }

    bool m_isGenerated = false;
};

MonitorGeneratedUiFile::MonitorGeneratedUiFile()
{
    connect(CppTools::CppModelManager::instance(),
            &CppTools::CppModelManager::abstractEditorSupportContentsUpdated,
            this, &MonitorGeneratedUiFile::onUiFileGenerated);
}

bool MonitorGeneratedUiFile::waitUntilGenerated(int timeout) const
{
    if (m_isGenerated)
        return true;

    QTime time;
    time.start();

    forever {
        if (m_isGenerated)
            return true;

        if (time.elapsed() > timeout)
            return false;

        QCoreApplication::processEvents();
        QThread::msleep(20);
    }

    return false;
}

class WriteFileAndWaitForReloadedDocument : public QObject
{
public:
    WriteFileAndWaitForReloadedDocument(const QString &filePath,
                                        const QByteArray &fileContents,
                                        Core::IDocument *document)
        : m_filePath(filePath)
        , m_fileContents(fileContents)
    {
        QTC_CHECK(document);
        connect(document, &Core::IDocument::reloadFinished,
                this, &WriteFileAndWaitForReloadedDocument::onReloadFinished);
    }

    void onReloadFinished()
    {
        m_onReloadFinished = true;
    }

    bool wait() const
    {
        QTC_ASSERT(writeFile(m_filePath, m_fileContents), return false);

        QTime totalTime;
        totalTime.start();

        QTime writeFileAgainTime;
        writeFileAgainTime.start();

        forever {
            if (m_onReloadFinished)
                return true;

            if (totalTime.elapsed() > 10000)
                return false;

            if (writeFileAgainTime.elapsed() > 1000) {
                // The timestamp did not change, try again now.
                QTC_ASSERT(writeFile(m_filePath, m_fileContents), return false);
                writeFileAgainTime.restart();
            }

            QCoreApplication::processEvents();
            QThread::msleep(20);
        }
    }

private:
    bool m_onReloadFinished = false;
    QString m_filePath;
    QByteArray m_fileContents;
};

} // anonymous namespace

namespace ClangCodeModel {
namespace Internal {
namespace Tests {

typedef QSharedPointer<CppTools::CppCodeModelSettings> CppCodeModelSettingsPtr;

class ActivateClangModelManagerSupport
{
public:
    ActivateClangModelManagerSupport(CppCodeModelSettingsPtr codeModelSettings);
    ~ActivateClangModelManagerSupport();

private:
    ActivateClangModelManagerSupport();

    CppCodeModelSettingsPtr m_codeModelSettings;
    QHash<QString, QString> m_previousValues;
};

ActivateClangModelManagerSupport::ActivateClangModelManagerSupport(
        CppCodeModelSettingsPtr codeModelSettings)
    : m_codeModelSettings(codeModelSettings)
{
    QTC_CHECK(m_codeModelSettings);
    const QString clangModelManagerSupportId
            = QLatin1String(Constants::CLANG_MODELMANAGERSUPPORT_ID);
    foreach (const QString &mimeType, CppTools::CppCodeModelSettings::supportedMimeTypes()) {
        m_previousValues.insert(mimeType,
                                m_codeModelSettings->modelManagerSupportIdForMimeType(mimeType));
        m_codeModelSettings->setModelManagerSupportIdForMimeType(mimeType,
                                                                 clangModelManagerSupportId);
    }
    m_codeModelSettings->emitChanged();
}

ActivateClangModelManagerSupport::~ActivateClangModelManagerSupport()
{
    QHash<QString, QString>::const_iterator i = m_previousValues.constBegin();
    for (; i != m_previousValues.end(); ++i)
        m_codeModelSettings->setModelManagerSupportIdForMimeType(i.key(), i.value());
    m_codeModelSettings->emitChanged();
}

ClangCodeCompletionTest::ClangCodeCompletionTest()
{
}

ClangCodeCompletionTest::~ClangCodeCompletionTest()
{
}

void ClangCodeCompletionTest::initTestCase()
{
    m_activater.reset(new ActivateClangModelManagerSupport(CppTools::codeModelSettings()));
}

void ClangCodeCompletionTest::testCompleteDoxygenKeywords()
{
    ProjectLessCompletionTest t("doxygenKeywordsCompletion.cpp");

    QVERIFY(hasItem(t.proposal, "brief"));
    QVERIFY(hasItem(t.proposal, "param"));
    QVERIFY(hasItem(t.proposal, "return"));
    QVERIFY(!hasSnippet(t.proposal, "class"));
}

void ClangCodeCompletionTest::testCompletePreprocessorKeywords()
{
    ProjectLessCompletionTest t("preprocessorKeywordsCompletion.cpp");

    QVERIFY(hasItem(t.proposal, "ifdef"));
    QVERIFY(hasItem(t.proposal, "endif"));
    QVERIFY(!hasSnippet(t.proposal, "class"));
}

void ClangCodeCompletionTest::testCompleteIncludeDirective()
{
    CppTools::Tests::TemporaryCopiedDir testDir(qrcPath("exampleIncludeDir"));
    ProjectLessCompletionTest t("includeDirectiveCompletion.cpp", QStringList(testDir.path()));

    QVERIFY(hasItem(t.proposal, "file.h"));
    QVERIFY(hasItem(t.proposal, "otherFile.h"));
    QVERIFY(hasItem(t.proposal, "mylib/"));
    QVERIFY(!hasSnippet(t.proposal, "class"));
}

void ClangCodeCompletionTest::testCompleteGlobals()
{
    ProjectLessCompletionTest t("globalCompletion.cpp");

    QVERIFY(hasItem(t.proposal, "globalVariable"));
    QVERIFY(hasItem(t.proposal, "globalFunction"));
    QVERIFY(hasItem(t.proposal, "GlobalClass"));
    QVERIFY(hasItem(t.proposal, "class"));    // Keyword
    QVERIFY(hasSnippet(t.proposal, "class")); // Snippet
}

void ClangCodeCompletionTest::testCompleteMembers()
{
    ProjectLessCompletionTest t("memberCompletion.cpp");

    QVERIFY(hasItem(t.proposal, "member"));
    QVERIFY(!hasItem(t.proposal, "globalVariable"));
    QVERIFY(!hasItem(t.proposal, "class"));    // Keyword
    QVERIFY(!hasSnippet(t.proposal, "class")); // Snippet
}

void ClangCodeCompletionTest::testCompleteFunctions()
{
    ProjectLessCompletionTest t("functionCompletion.cpp");

    QVERIFY(hasItem(t.proposal, "void f()"));
    QVERIFY(hasItem(t.proposal, "void f(int a)"));
    QVERIFY(hasItem(t.proposal, "void f(const QString &s)"));
    QVERIFY(hasItem(t.proposal, "void f(char c<i>, int optional</i>)")); // TODO: No default argument?
    QVERIFY(hasItem(t.proposal, "void f(char c<i>, int optional1, int optional2</i>)")); // TODO: No default argument?
    QVERIFY(hasItem(t.proposal, "void f(const TType<QString> *t)"));
    QVERIFY(hasItem(t.proposal, "TType<QString> f(bool)"));
}

void ClangCodeCompletionTest::testProjectDependentCompletion()
{
    const TestDocument testDocument("completionWithProject.cpp");
    QVERIFY(testDocument.isCreatedAndHasValidCursorPosition());

    ProjectLoader projectLoader(QStringList(testDocument.filePath),
                                _("#define PROJECT_CONFIGURATION_1\n"));
    QVERIFY(projectLoader.load());

    OpenEditorAtCursorPosition openEditor(testDocument);
    QVERIFY(openEditor.succeeded());

    ProposalModel proposal = completionResults(openEditor.editor());
    QVERIFY(hasItem(proposal, "projectConfiguration1"));
}

void ClangCodeCompletionTest::testChangingProjectDependentCompletion()
{
    const TestDocument testDocument("completionWithProject.cpp");
    QVERIFY(testDocument.isCreatedAndHasValidCursorPosition());

    OpenEditorAtCursorPosition openEditor(testDocument);
    QVERIFY(openEditor.succeeded());

    // Check completion without project
    ProposalModel proposal = completionResults(openEditor.editor());
    QVERIFY(hasItem(proposal, "noProjectConfigurationDetected"));

    {
        // Check completion with project configuration 1
        ProjectLoader projectLoader(QStringList(testDocument.filePath),
                                    _("#define PROJECT_CONFIGURATION_1\n"),
                                    /* testOnlyForCleanedProjects= */ true);
        QVERIFY(projectLoader.load());
        proposal = completionResults(openEditor.editor());

        QVERIFY(hasItem(proposal, "projectConfiguration1"));
        QVERIFY(!hasItem(proposal, "projectConfiguration2"));

        // Check completion with project configuration 2
        QVERIFY(projectLoader.updateProject(_("#define PROJECT_CONFIGURATION_2\n")));
        proposal = completionResults(openEditor.editor());

        QVERIFY(!hasItem(proposal, "projectConfiguration1"));
        QVERIFY(hasItem(proposal, "projectConfiguration2"));
    } // Project closed

    // Check again completion without project
    proposal = completionResults(openEditor.editor());
    QVERIFY(hasItem(proposal, "noProjectConfigurationDetected"));
}

void ClangCodeCompletionTest::testUnsavedFilesTrackingByModifyingIncludedFileInCurrentEditor()
{
    CppTools::Tests::TemporaryDir temporaryDir;
    const TestDocument sourceDocument("mysource.cpp", &temporaryDir);
    QVERIFY(sourceDocument.isCreatedAndHasValidCursorPosition());
    const TestDocument headerDocument("myheader.h", &temporaryDir);
    QVERIFY(headerDocument.isCreated());

    // Test that declarations from header file are visible in source file
    OpenEditorAtCursorPosition openSource(sourceDocument);
    QVERIFY(openSource.succeeded());
    ProposalModel proposal = completionResults(openSource.editor());
    QVERIFY(hasItem(proposal, "globalFromHeader"));

    // Open header and insert a new declaration
    OpenEditorAtCursorPosition openHeader(headerDocument);
    QVERIFY(openHeader.succeeded());
    insertTextAtTopOfEditor(openHeader.editor(), "int globalFromHeaderUnsaved;\n");

    // Switch back to source file and check if modified header is reflected in completions.
    Core::EditorManager::activateEditor(openSource.editor());
    proposal = completionResults(openSource.editor());
    QVERIFY(hasItem(proposal, "globalFromHeader"));
    QVERIFY(hasItem(proposal, "globalFromHeaderUnsaved"));
}

void ClangCodeCompletionTest::testUnsavedFilesTrackingByModifyingIncludedFileInNotCurrentEditor()
{
    CppTools::Tests::TemporaryDir temporaryDir;
    const TestDocument sourceDocument("mysource.cpp", &temporaryDir);
    QVERIFY(sourceDocument.isCreatedAndHasValidCursorPosition());
    const TestDocument headerDocument("myheader.h", &temporaryDir);
    QVERIFY(headerDocument.isCreated());

    // Open header
    OpenEditorAtCursorPosition openHeader(headerDocument);
    QVERIFY(openHeader.succeeded());

    // Open source and test that declaration from header file is visible in source file
    OpenEditorAtCursorPosition openSource(sourceDocument);
    QVERIFY(openSource.succeeded());
    ProposalModel proposal = completionResults(openSource.editor());
    QVERIFY(hasItem(proposal, "globalFromHeader"));

    // Modify header document without switching to its editor.
    // This simulates e.g. changes from refactoring actions.
    Utils::ChangeSet cs;
    cs.insert(0, QLatin1String("int globalFromHeaderUnsaved;\n"));
    QTextCursor textCursor = openHeader.editor()->textCursor();
    cs.apply(&textCursor);

    // Check whether modified header is reflected in the completions.
    proposal = completionResults(openSource.editor());
    QVERIFY(hasItem(proposal, "globalFromHeader"));
    QVERIFY(hasItem(proposal, "globalFromHeaderUnsaved"));
}

void ClangCodeCompletionTest::testUnsavedFilesTrackingByModifyingIncludedFileExternally()
{
    ChangeDocumentReloadSetting reloadSettingsChanger(Core::IDocument::ReloadUnmodified);

    CppTools::Tests::TemporaryDir temporaryDir;
    const TestDocument sourceDocument("mysource.cpp", &temporaryDir);
    QVERIFY(sourceDocument.isCreatedAndHasValidCursorPosition());
    const TestDocument headerDocument("myheader.h", &temporaryDir);
    QVERIFY(headerDocument.isCreated());

    // Open header
    OpenEditorAtCursorPosition openHeader(headerDocument);
    QVERIFY(openHeader.succeeded());

    // Open source and test completions
    OpenEditorAtCursorPosition openSource(sourceDocument);
    QVERIFY(openSource.succeeded());
    ProposalModel proposal = completionResults(openSource.editor());
    QVERIFY(hasItem(proposal, "globalFromHeader"));

    // Simulate external modification and wait for reload
    WriteFileAndWaitForReloadedDocument waitForReloadedDocument(
                headerDocument.filePath,
                "int globalFromHeaderReloaded;\n",
                openHeader.editor()->document());
    QVERIFY(waitForReloadedDocument.wait());

    // Retrigger completion and check if its updated
    proposal = completionResults(openSource.editor());
    QVERIFY(hasItem(proposal, "globalFromHeaderReloaded"));
}

void ClangCodeCompletionTest::testUnsavedFilesTrackingByCompletingUiObject()
{
    CppTools::Tests::TemporaryCopiedDir testDir(qrcPath("qt-widgets-app"));
    QVERIFY(testDir.isValid());

    MonitorGeneratedUiFile monitorGeneratedUiFile;

    // Open project
    const QString projectFilePath = testDir.absolutePath("qt-widgets-app.pro");
    CppTools::Tests::ProjectOpenerAndCloser projectManager;
    const CppTools::ProjectInfo projectInfo = projectManager.open(projectFilePath, true);
    QVERIFY(projectInfo.isValid());

    // Open file with ui object
    const QString completionFile = testDir.absolutePath("mainwindow.cpp");
    const TestDocument testDocument = TestDocument::fromExistingFile(completionFile);
    QVERIFY(testDocument.isCreatedAndHasValidCursorPosition());
    OpenEditorAtCursorPosition openSource(testDocument);
    QVERIFY(openSource.succeeded());

    // ...and check comletions
    QVERIFY(monitorGeneratedUiFile.waitUntilGenerated());
    ProposalModel proposal = completionResults(openSource.editor());
    QVERIFY(hasItem(proposal, "menuBar"));
    QVERIFY(hasItem(proposal, "statusBar"));
    QVERIFY(hasItem(proposal, "centralWidget"));
    QVERIFY(hasItem(proposal, "setupUi"));
}

void ClangCodeCompletionTest::testUpdateBackendAfterRestart()
{
    IpcSenderSpy spy;
    ChangeIpcSender changeIpcSender(&spy);

    CppTools::Tests::TemporaryCopiedDir testDir(qrcPath("qt-widgets-app"));
    QVERIFY(testDir.isValid());

    // Open file not part of any project...
    const TestDocument headerDocument("myheader.h", &testDir);
    QVERIFY(headerDocument.isCreated());
    OpenEditorAtCursorPosition openHeader(headerDocument);
    QVERIFY(openHeader.succeeded());
    // ... and modify it, so we have an unsaved file.
    insertTextAtTopOfEditor(openHeader.editor(), "int someGlobal;\n");
    // Open project ...
    MonitorGeneratedUiFile monitorGeneratedUiFile;
    const QString projectFilePath = testDir.absolutePath("qt-widgets-app.pro");
    CppTools::Tests::ProjectOpenerAndCloser projectManager;
    const CppTools::ProjectInfo projectInfo = projectManager.open(projectFilePath, true);
    QVERIFY(projectInfo.isValid());
    // ...and a file of the project
    const QString completionFile = testDir.absolutePath("mainwindow.cpp");
    const TestDocument testDocument = TestDocument::fromExistingFile(completionFile);
    QVERIFY(testDocument.isCreatedAndHasValidCursorPosition());
    OpenEditorAtCursorPosition openSource(testDocument);
    QVERIFY(openSource.succeeded());
    QVERIFY(monitorGeneratedUiFile.waitUntilGenerated());

    // Check commands that would have been sent
    QVERIFY(compare(LogOutput(spy.senderLog),
                    LogOutput(
                        "RegisterProjectPartsForCodeCompletionCommand\n"
                        "  ProjectPartContainer id: qt-widgets-app.pro\n"
                        "RegisterTranslationUnitForCodeCompletionCommand\n"
                        "  Path: myheader.h ProjectPart: \n"
                        "RegisterTranslationUnitForCodeCompletionCommand\n"
                        "  Path: ui_mainwindow.h ProjectPart: \n"
                    )));
    spy.senderLog.clear();

    // Kill backend process...
    auto &ipcCommunicator = ModelManagerSupportClang::instance_forTestsOnly()->ipcCommunicator();
    ipcCommunicator.killBackendProcess();
    QSignalSpy waitForReinitializedBackend(&ipcCommunicator,
                                           SIGNAL(backendReinitialized()));
    QVERIFY(waitForReinitializedBackend.wait());

    // ...and check if code model backend would have been provided with current data
    QVERIFY(compare(LogOutput(spy.senderLog),
                    LogOutput(
                        "RegisterProjectPartsForCodeCompletionCommand\n"
                        "  ProjectPartContainer id: \n"
                        "RegisterProjectPartsForCodeCompletionCommand\n"
                        "  ProjectPartContainer id: qt-widgets-app.pro\n"
                        "RegisterTranslationUnitForCodeCompletionCommand\n"
                        "  Path: myheader.h ProjectPart: \n"
                        "RegisterTranslationUnitForCodeCompletionCommand\n"
                        "  Path: ui_mainwindow.h ProjectPart: \n"
                    )));
}

} // namespace Tests
} // namespace Internal
} // namespace ClangCodeModel

#include "clangcodecompletion_test.moc"
