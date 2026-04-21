#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QQueue>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <memory>
#include <utility>

namespace {

QString sanitizeFileName(QString fileName)
{
    fileName = QFileInfo(fileName).fileName().trimmed();
    if (fileName.isEmpty())
        return {};

    static const QRegularExpression unsafeChars(QStringLiteral(R"([^A-Za-z0-9._ -])"));
    fileName.replace(unsafeChars, QStringLiteral("_"));
    return fileName.left(180);
}

QString defaultClientName()
{
    const QString computerName = qEnvironmentVariable("COMPUTERNAME");
    if (!computerName.isEmpty())
        return computerName;

    const QString hostName = QSysInfo::machineHostName();
    if (!hostName.isEmpty())
        return hostName;

    return QStringLiteral("windows-client");
}

QUrl normalizeServerUrl(QString input)
{
    input = input.trimmed();
    if (!input.contains(QStringLiteral("://")))
        input.prepend(QStringLiteral("http://"));

    QUrl url(input);
    if (url.path().isEmpty() || url.path() == QStringLiteral("/"))
        url.setPath(QStringLiteral("/events"));
    return url;
}

QString defaultZipTarget(const QString &packageName)
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QDir::tempPath() + QStringLiteral("/SilenceInstaller");
    return QDir(base).filePath(QFileInfo(packageName).completeBaseName());
}

QString jobTempDir(const QString &jobId)
{
    return QDir(QDir::tempPath()).filePath(QStringLiteral("SilenceInstaller/%1").arg(jobId));
}

QStringList splitArgumentsOrDefault(const QString &arguments, const QStringList &defaults)
{
    if (arguments.trimmed().isEmpty())
        return defaults;
    return QProcess::splitCommand(arguments);
}

} // namespace

class SseInstallerClient final : public QObject
{
public:
    SseInstallerClient(QUrl eventsUrl, QString clientName, int reconnectMs, QObject *parent = nullptr)
        : QObject(parent),
          m_eventsUrl(std::move(eventsUrl)),
          m_clientName(std::move(clientName)),
          m_reconnectMs(reconnectMs)
    {
        m_serverBase = m_eventsUrl;
        m_serverBase.setPath(QStringLiteral("/"));
        m_serverBase.setQuery(QString());
        m_serverBase.setFragment({});

        QUrlQuery query(m_eventsUrl);
        query.addQueryItem(QStringLiteral("role"), QStringLiteral("client"));
        query.addQueryItem(QStringLiteral("name"), m_clientName);
        m_eventsUrl.setQuery(query);
    }

    void start()
    {
        connectSse();
    }

private:
    void connectSse()
    {
        if (m_sseReply)
            m_sseReply->deleteLater();

        QNetworkRequest request(m_eventsUrl);
        request.setRawHeader("Accept", "text/event-stream");
        request.setRawHeader("Cache-Control", "no-cache");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

        QTextStream(stdout) << "Connecting SSE: " << m_eventsUrl.toString() << Qt::endl;
        m_sseReply = m_network.get(request);
        connect(m_sseReply, &QNetworkReply::readyRead, this, [this] { processSseBytes(m_sseReply->readAll()); });
        connect(m_sseReply, &QNetworkReply::finished, this, [this] {
            const QString error = m_sseReply->error() == QNetworkReply::NoError
                                      ? QStringLiteral("closed")
                                      : m_sseReply->errorString();
            QTextStream(stderr) << "SSE disconnected: " << error << Qt::endl;
            m_sseReply->deleteLater();
            m_sseReply = nullptr;
            scheduleReconnect();
        });
    }

    void scheduleReconnect()
    {
        QTimer::singleShot(m_reconnectMs, this, [this] { connectSse(); });
    }

    void processSseBytes(const QByteArray &bytes)
    {
        m_sseBuffer += bytes;
        while (true) {
            const qsizetype newline = m_sseBuffer.indexOf('\n');
            if (newline < 0)
                break;

            QByteArray line = m_sseBuffer.left(newline);
            m_sseBuffer.remove(0, newline + 1);
            if (line.endsWith('\r'))
                line.chop(1);
            processSseLine(line);
        }
    }

    void processSseLine(const QByteArray &line)
    {
        if (line.isEmpty()) {
            dispatchSseEvent();
            return;
        }
        if (line.startsWith(':'))
            return;

        const qsizetype separator = line.indexOf(':');
        const QByteArray field = separator < 0 ? line : line.left(separator);
        QByteArray value = separator < 0 ? QByteArray{} : line.mid(separator + 1);
        if (value.startsWith(' '))
            value.remove(0, 1);

        if (field == "event") {
            m_currentEvent = value;
        } else if (field == "data") {
            m_currentData += value;
            m_currentData += '\n';
        }
    }

    void dispatchSseEvent()
    {
        if (m_currentData.endsWith('\n'))
            m_currentData.chop(1);
        const QByteArray event = m_currentEvent.isEmpty() ? QByteArrayLiteral("message") : m_currentEvent;
        const QByteArray data = m_currentData;
        m_currentEvent.clear();
        m_currentData.clear();

        if (event != "install" || data.isEmpty())
            return;

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            sendReport({}, QStringLiteral("failed"), QStringLiteral("invalid install job json"), -1);
            return;
        }

        m_jobs.enqueue(document.object());
        runNextJob();
    }

    void runNextJob()
    {
        if (m_installing || m_jobs.isEmpty())
            return;

        m_installing = true;
        const QJsonObject job = m_jobs.dequeue();
        downloadJob(job);
    }

    void downloadJob(const QJsonObject &job)
    {
        const QString jobId = job.value(QStringLiteral("id")).toString();
        const QString packageName = sanitizeFileName(job.value(QStringLiteral("packageName")).toString());
        const QUrl downloadUrl = m_serverBase.resolved(QUrl(job.value(QStringLiteral("downloadUrl")).toString()));
        if (jobId.isEmpty() || packageName.isEmpty() || !downloadUrl.isValid()) {
            finishJob(job, QStringLiteral("failed"), QStringLiteral("invalid job fields"), -1);
            return;
        }

        QTextStream(stdout) << "Downloading job " << jobId << ": " << downloadUrl.toString() << Qt::endl;
        QNetworkRequest request(downloadUrl);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = m_network.get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, job, packageName] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                finishJob(job, QStringLiteral("failed"), reply->errorString(), -1);
                return;
            }

            const QByteArray data = reply->readAll();
            const QString jobDir = jobTempDir(job.value(QStringLiteral("id")).toString());
            if (!QDir().mkpath(jobDir)) {
                finishJob(job, QStringLiteral("failed"), QStringLiteral("cannot create temp directory"), -1);
                return;
            }

            const QString packagePath = QDir(jobDir).filePath(packageName);
            QFile packageFile(packagePath);
            if (!packageFile.open(QIODevice::WriteOnly) || packageFile.write(data) != data.size()) {
                finishJob(job, QStringLiteral("failed"), QStringLiteral("cannot save package"), -1);
                return;
            }
            packageFile.close();

            executeJob(job, packagePath);
        });
    }

    void executeJob(const QJsonObject &job, const QString &packagePath)
    {
#ifndef Q_OS_WIN
        Q_UNUSED(packagePath);
        finishJob(job, QStringLiteral("failed"), QStringLiteral("client installer execution is Windows-only"), -1);
#else
        const QString type = job.value(QStringLiteral("type")).toString();
        const QString packageName = job.value(QStringLiteral("packageName")).toString();
        const QString suffix = QFileInfo(packagePath).suffix().toLower();
        const QString rawArguments = job.value(QStringLiteral("arguments")).toString();

        QString program;
        QStringList arguments;
        if (type == QStringLiteral("zip")) {
            const QString targetDir = job.value(QStringLiteral("targetDir")).toString().trimmed().isEmpty()
                                          ? defaultZipTarget(packageName)
                                          : job.value(QStringLiteral("targetDir")).toString().trimmed();
            QDir().mkpath(targetDir);
            program = QStringLiteral("powershell.exe");
            arguments = {
                QStringLiteral("-NoProfile"),
                QStringLiteral("-ExecutionPolicy"),
                QStringLiteral("Bypass"),
                QStringLiteral("-Command"),
                QStringLiteral("Expand-Archive -LiteralPath $args[0] -DestinationPath $args[1] -Force"),
                packagePath,
                targetDir
            };
        } else if (suffix == QStringLiteral("msi")) {
            program = QStringLiteral("msiexec.exe");
            arguments = {QStringLiteral("/i"), packagePath};
            arguments += splitArgumentsOrDefault(rawArguments, {QStringLiteral("/qn"), QStringLiteral("/norestart")});
        } else if (suffix == QStringLiteral("exe")) {
            program = packagePath;
            arguments = splitArgumentsOrDefault(rawArguments, {QStringLiteral("/S")});
        } else if (suffix == QStringLiteral("bat") || suffix == QStringLiteral("cmd")) {
            program = QStringLiteral("cmd.exe");
            arguments = {QStringLiteral("/c"), packagePath};
            arguments += QProcess::splitCommand(rawArguments);
        } else if (suffix == QStringLiteral("ps1")) {
            program = QStringLiteral("powershell.exe");
            arguments = {
                QStringLiteral("-NoProfile"),
                QStringLiteral("-ExecutionPolicy"),
                QStringLiteral("Bypass"),
                QStringLiteral("-File"),
                packagePath
            };
            arguments += QProcess::splitCommand(rawArguments);
        } else {
            finishJob(job, QStringLiteral("failed"), QStringLiteral("unsupported package extension"), -1);
            return;
        }

        QTextStream(stdout) << "Executing job " << job.value(QStringLiteral("id")).toString()
                            << " with " << program << Qt::endl;
        QProcess *process = new QProcess(this);
        auto output = std::make_shared<QByteArray>();
        auto completed = std::make_shared<bool>(false);
        connect(process, &QProcess::readyReadStandardOutput, this, [process, output] {
            output->append(process->readAllStandardOutput());
            if (output->size() > 8192)
                output->remove(0, output->size() - 8192);
        });
        connect(process, &QProcess::readyReadStandardError, this, [process, output] {
            output->append(process->readAllStandardError());
            if (output->size() > 8192)
                output->remove(0, output->size() - 8192);
        });
        connect(process, &QProcess::errorOccurred, this, [this, process, completed, job](QProcess::ProcessError error) {
            if (*completed || error != QProcess::FailedToStart)
                return;
            *completed = true;
            const QString message = QStringLiteral("failed to start process: %1").arg(process->errorString());
            process->deleteLater();
            finishJob(job, QStringLiteral("failed"), message, -1);
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                this, [this, process, output, completed, job](int exitCode, QProcess::ExitStatus exitStatus) {
            if (*completed)
                return;
            *completed = true;
            const QString status = exitStatus == QProcess::NormalExit && exitCode == 0
                                       ? QStringLiteral("succeeded")
                                       : QStringLiteral("failed");
            QString message = QString::fromLocal8Bit(*output).trimmed();
            if (message.isEmpty())
                message = QStringLiteral("process finished");
            process->deleteLater();
            finishJob(job, status, message, exitCode);
        });
        process->start(program, arguments);
#endif
    }

    void finishJob(const QJsonObject &job, const QString &status, const QString &message, int exitCode)
    {
        sendReport(job, status, message, exitCode);
        cleanupJobFiles(job);
        m_installing = false;
        QTimer::singleShot(0, this, [this] { runNextJob(); });
    }

    void cleanupJobFiles(const QJsonObject &job)
    {
        const QString jobId = job.value(QStringLiteral("id")).toString();
        if (jobId.isEmpty())
            return;

        const QString dirPath = jobTempDir(jobId);
        QDir dir(dirPath);
        if (!dir.exists())
            return;

        if (dir.removeRecursively()) {
            QTextStream(stdout) << "Cleaned job files: " << dirPath << Qt::endl;
        } else {
            QTextStream(stderr) << "Failed to clean job files: " << dirPath << Qt::endl;
        }
    }

    void sendReport(const QJsonObject &job, const QString &status, const QString &message, int exitCode)
    {
        QJsonObject report{
            {QStringLiteral("client"), m_clientName},
            {QStringLiteral("jobId"), job.value(QStringLiteral("id")).toString()},
            {QStringLiteral("packageName"), job.value(QStringLiteral("packageName")).toString()},
            {QStringLiteral("status"), status},
            {QStringLiteral("message"), message.left(4000)},
            {QStringLiteral("exitCode"), exitCode},
            {QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}
        };

        QNetworkRequest request(m_serverBase.resolved(QUrl(QStringLiteral("/api/reports"))));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        QNetworkReply *reply = m_network.post(request, QJsonDocument(report).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);

        QTextStream(stdout) << "Job report: " << QJsonDocument(report).toJson(QJsonDocument::Compact) << Qt::endl;
    }

    QNetworkAccessManager m_network;
    QUrl m_eventsUrl;
    QUrl m_serverBase;
    QString m_clientName;
    int m_reconnectMs;
    QNetworkReply *m_sseReply = nullptr;
    QByteArray m_sseBuffer;
    QByteArray m_currentEvent;
    QByteArray m_currentData;
    QQueue<QJsonObject> m_jobs;
    bool m_installing = false;
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("SilenceInstallerClient"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("SSE silent installer client for Windows."));
    parser.addHelpOption();

    QCommandLineOption serverOption(
        QStringList{QStringLiteral("s"), QStringLiteral("server")},
        QStringLiteral("Server base URL or SSE URL."),
        QStringLiteral("url"),
        QStringLiteral("http://127.0.0.1:8080"));
    QCommandLineOption nameOption(
        QStringList{QStringLiteral("n"), QStringLiteral("name")},
        QStringLiteral("Client name shown in the server UI."),
        QStringLiteral("name"),
        defaultClientName());
    QCommandLineOption reconnectOption(
        QStringLiteral("reconnect-ms"),
        QStringLiteral("Reconnect delay after SSE disconnect."),
        QStringLiteral("milliseconds"),
        QStringLiteral("5000"));

    parser.addOption(serverOption);
    parser.addOption(nameOption);
    parser.addOption(reconnectOption);
    parser.process(app);

    bool ok = false;
    const int reconnectMs = parser.value(reconnectOption).toInt(&ok);
    SseInstallerClient client(normalizeServerUrl(parser.value(serverOption)),
                              parser.value(nameOption),
                              ok && reconnectMs > 0 ? reconnectMs : 5000);
    client.start();
    return app.exec();
}
