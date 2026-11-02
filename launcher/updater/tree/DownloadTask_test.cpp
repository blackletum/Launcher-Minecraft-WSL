#include <QTest>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include "TestUtil.h"
#include "TestTrees.h"
#include "FileSystem.h"
#include "updater/tree/DownloadTask.h"

using namespace TreeUpdater;

namespace {

Variant variant(qint64 build)
{
    BuildManifest out;
    QString error;
    parseBuild(TestTrees::read("main", QString("builds/%1.json").arg(build)), build, out, error);
    return out.variants.value("linux-x86_64-qt5");
}

void writeFile(const QString &path, const QByteArray &data)
{
    FS::ensureFilePathExists(path);
    QFile file(path);
    file.open(QFile::WriteOnly);
    file.write(data);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    file.open(QFile::ReadOnly);
    return file.readAll();
}

}

class TreeDownloadTaskTest : public QObject
{
    Q_OBJECT

    shared_qobject_ptr<QNetworkAccessManager> m_network;
    std::unique_ptr<QTemporaryDir> m_dir;

    QString root()
    {
        return m_dir->path() + "/root";
    }

    // as MainWindow makes it
    QString target()
    {
        auto path = m_dir->path() + "/update/XXXXXX";
        FS::ensureFilePathExists(path);
        return path;
    }

    // build 10 installed, an update to 11 found on `mirrors`
    CheckResult update(const QStringList &mirrors)
    {
        for(auto &file: variant(10).files)
        {
            writeFile(FS::PathCombine(root(), file.path), TestTrees::read("main", contentPath(file.sha256)));
        }
        CheckResult out;
        out.kind = CheckResult::Update;
        out.currentBuild = 10;
        out.build = 11;
        out.variant = variant(11);
        out.haveCurrentFiles = true;
        out.currentFiles = variant(10).files;
        out.mirrors = mirrors;
        return out;
    }

    bool run(DownloadTask &task)
    {
        bool done = false, succeeded = false;
        connect(&task, &Task::succeeded, [&]() { done = succeeded = true; });
        connect(&task, &Task::failed, [&](QString) { done = true; });
        task.start();
        QElapsedTimer timer;
        timer.start();
        while(!done && timer.elapsed() < 20000)
        {
            QTest::qWait(10);
        }
        return succeeded;
    }

private
slots:
    void init()
    {
        m_network.reset(new QNetworkAccessManager());
        m_dir.reset(new QTemporaryDir());
    }

    void test_download()
    {
        DownloadTask task(m_network, update(QStringList() << TestTrees::url("main")), root(), target());
        QVERIFY(run(task));
        auto operations = task.operations();
        QCOMPARE(operations.size(), 4);
        QCOMPARE(operations[0], GoUpdate::Operation::DeleteOp("share/old.txt"));
        QStringList destinations;
        for(auto &operation: operations.mid(1))
        {
            QVERIFY(operation.type == GoUpdate::Operation::OP_REPLACE);
            QVERIFY(operation.source.startsWith(task.updateFilesDir()));
            destinations.append(operation.destination);
        }
        QCOMPARE(destinations, QStringList() << "MultiMC" << "bin/MultiMC" << "share/new.txt");
        auto next = variant(11);
        QCOMPARE(readFile(operations[2].source), TestTrees::read("main", contentPath(next.files[1].sha256)));
        QCOMPARE(next.files[1].path, QString("bin/MultiMC"));
    }

    // a mirror serving a wrong file: the rest comes from the next one
    void test_badContentFallsBack()
    {
        auto bad = m_dir->path() + "/bad";
        auto next = variant(11);
        writeFile(bad + "/" + contentPath(next.files[0].sha256), "not what the manifest lists\n");
        QStringList mirrors;
        mirrors << QUrl::fromLocalFile(bad + "/").toString() << TestTrees::url("main");
        DownloadTask task(m_network, update(mirrors), root(), target());
        QVERIFY(run(task));
        QCOMPARE(readFile(task.operations()[1].source), TestTrees::read("main", contentPath(next.files[0].sha256)));
    }

    void test_allMirrorsFail()
    {
        auto bad = m_dir->path() + "/bad";
        // more than the manifest says
        auto next = variant(11);
        writeFile(bad + "/" + contentPath(next.files[0].sha256), TestTrees::read("main", contentPath(next.files[0].sha256)) + "x");
        DownloadTask task(m_network, update(QStringList() << QUrl::fromLocalFile(bad + "/").toString()), root(), target());
        QVERIFY(!run(task));
        QVERIFY2(task.failReason().contains("more than"), qPrintable(task.failReason()));
        QVERIFY(task.operations().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TreeDownloadTaskTest)

#include "DownloadTask_test.moc"
