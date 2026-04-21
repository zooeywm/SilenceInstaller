#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

namespace {

struct HttpRequest
{
    QByteArray method;
    QUrl url;
    QHash<QByteArray, QByteArray> headers;
    QByteArray body;
};

struct SseConnection
{
    QPointer<QTcpSocket> socket;
    QString name;
    QString role;
    QDateTime connectedAt;
};

struct PackageUpload
{
    QString fileName;
    QByteArray bytes;
    QString arguments;
    QString targetDir;
};

QString sanitizeFileName(QString fileName)
{
    fileName = QFileInfo(fileName).fileName().trimmed();
    if (fileName.isEmpty())
        return {};

    static const QRegularExpression unsafeChars(QStringLiteral(R"([^A-Za-z0-9._ -])"));
    fileName.replace(unsafeChars, QStringLiteral("_"));
    return fileName.left(180);
}

QString packageTypeForFile(const QString &fileName)
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (suffix == QStringLiteral("zip"))
        return QStringLiteral("zip");
    if (suffix == QStringLiteral("msi") || suffix == QStringLiteral("exe") ||
        suffix == QStringLiteral("bat") || suffix == QStringLiteral("cmd") ||
        suffix == QStringLiteral("ps1"))
        return QStringLiteral("installer");
    return {};
}

QString multipartParameter(const QByteArray &headerValue, const QString &name)
{
    const QRegularExpression expression(
        QStringLiteral("%1=\"([^\"]*)\"").arg(QRegularExpression::escape(name)));
    const QRegularExpressionMatch match = expression.match(QString::fromUtf8(headerValue));
    return match.hasMatch() ? match.captured(1) : QString();
}

int indexedField(const QString &fieldName, const QString &prefix)
{
    if (fieldName == prefix)
        return 0;
    if (!fieldName.startsWith(prefix + QStringLiteral("_")))
        return -1;

    bool ok = false;
    const int index = fieldName.mid(prefix.size() + 1).toInt(&ok);
    return ok && index >= 0 ? index : -1;
}

QByteArray statusLine(int statusCode)
{
    switch (statusCode) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 204: return "204 No Content";
    case 400: return "400 Bad Request";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 413: return "413 Payload Too Large";
    case 500: return "500 Internal Server Error";
    default: return QByteArray::number(statusCode) + " Unknown";
    }
}

QByteArray jsonResponse(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

} // namespace

class InstallerServer;

class HttpConnection final : public QObject
{
public:
    HttpConnection(InstallerServer *server, QTcpSocket *socket);

private:
    friend class InstallerServer;

    void onReadyRead();
    bool tryParseRequest(HttpRequest *request);
    void sendResponse(int statusCode, const QByteArray &contentType, const QByteArray &body,
                      const QList<QPair<QByteArray, QByteArray>> &extraHeaders = {});

    InstallerServer *m_server;
    QTcpSocket *m_socket;
    QByteArray m_buffer;
};

class InstallerServer final : public QObject
{
public:
    InstallerServer(QString packageRoot, quint16 port, QObject *parent = nullptr)
        : QObject(parent),
          m_packageRoot(std::move(packageRoot))
    {
        QDir().mkpath(m_packageRoot);

        connect(&m_tcpServer, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_tcpServer.nextPendingConnection()) {
                socket->setParent(nullptr);
                new HttpConnection(this, socket);
            }
        });

        connect(&m_heartbeatTimer, &QTimer::timeout, this, [this] {
            broadcastRaw(": heartbeat\n\n", true);
        });
        m_heartbeatTimer.start(std::chrono::seconds(25));

        if (!m_tcpServer.listen(QHostAddress::Any, port)) {
            QTextStream(stderr) << "Failed to listen: " << m_tcpServer.errorString() << Qt::endl;
            QCoreApplication::exit(1);
            return;
        }

        QTextStream(stdout) << "SilenceInstallerServer listening on http://0.0.0.0:"
                            << m_tcpServer.serverPort() << Qt::endl;
        QTextStream(stdout) << "Package storage: " << QDir(m_packageRoot).absolutePath() << Qt::endl;
    }

    void handleRequest(const HttpRequest &request, HttpConnection *connection)
    {
        const QString path = request.url.path();

        if (request.method == "OPTIONS") {
            connection->sendResponse(204, "text/plain", {});
            return;
        }

        if (request.method == "GET" && (path == "/" || path == "/index.html")) {
            connection->sendResponse(200, "text/html; charset=utf-8", adminHtml());
            return;
        }

        if (request.method == "GET" && path == "/events") {
            registerSse(request, connection);
            return;
        }

        if (request.method == "GET" && path == "/api/status") {
            connection->sendResponse(200, "application/json", jsonResponse(statusJson()));
            return;
        }

        if (request.method == "POST" && path == "/api/jobs") {
            handleCreateJob(request, connection);
            return;
        }

        if (request.method == "POST" && path == "/api/reports") {
            handleReport(request, connection);
            return;
        }

        if (request.method == "GET" && path.startsWith("/packages/")) {
            servePackage(path, connection);
            return;
        }

        connection->sendResponse(404, "application/json",
                                 jsonResponse({{QStringLiteral("error"), QStringLiteral("not found")}}));
    }

    void registerSse(const HttpRequest &request, HttpConnection *connection)
    {
        QTcpSocket *socket = connection->m_socket;
        connection->m_socket = nullptr;

        const QUrlQuery query(request.url);
        SseConnection client;
        client.socket = socket;
        client.name = query.queryItemValue(QStringLiteral("name"));
        client.role = query.queryItemValue(QStringLiteral("role"));
        client.connectedAt = QDateTime::currentDateTimeUtc();
        if (client.name.isEmpty())
            client.name = socket->peerAddress().toString();
        if (client.role.isEmpty())
            client.role = QStringLiteral("client");

        const QByteArray headers =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream; charset=utf-8\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "X-Accel-Buffering: no\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n";
        socket->write(headers);
        writeSse(socket, "hello", QJsonDocument(QJsonObject{
            {QStringLiteral("name"), client.name},
            {QStringLiteral("role"), client.role},
            {QStringLiteral("connectedAt"), client.connectedAt.toString(Qt::ISODate)}
        }).toJson(QJsonDocument::Compact));

        m_sseConnections.push_back(client);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            removeSse(socket);
            socket->deleteLater();
        });

        connection->deleteLater();
    }

private:
    friend class HttpConnection;

    QJsonObject statusJson() const
    {
        int clientCount = 0;
        int adminCount = 0;
        for (const SseConnection &connection : m_sseConnections) {
            if (!connection.socket)
                continue;
            if (connection.role == QStringLiteral("admin"))
                ++adminCount;
            else
                ++clientCount;
        }

        return {
            {QStringLiteral("clients"), clientCount},
            {QStringLiteral("admins"), adminCount},
            {QStringLiteral("reports"), m_reports},
            {QStringLiteral("serverTime"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}
        };
    }

    void handleCreateJob(const HttpRequest &request, HttpConnection *connection)
    {
        QVector<PackageUpload> packages;
        QString parseError;
        if (!parseCreateJobBody(request, &packages, &parseError)) {
            connection->sendResponse(400, "application/json",
                                     jsonResponse({{QStringLiteral("error"), parseError}}));
            return;
        }

        if (packages.isEmpty()) {
            connection->sendResponse(400, "application/json", jsonResponse({
                {QStringLiteral("error"), QStringLiteral("missing package")}
            }));
            return;
        }

        for (const PackageUpload &package : std::as_const(packages)) {
            if (package.fileName.isEmpty() || packageTypeForFile(package.fileName).isEmpty() || package.bytes.isEmpty()) {
                connection->sendResponse(400, "application/json", jsonResponse({
                    {QStringLiteral("error"), QStringLiteral("missing or unsupported package")}
                }));
                return;
            }
        }

        QJsonArray jobs;
        for (const PackageUpload &package : std::as_const(packages)) {
            const QString id = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmsszzz")) +
                               QStringLiteral("-") +
                               QString::number(QRandomGenerator::global()->bounded(100000, 999999));
            const QString packageDir = QDir(m_packageRoot).filePath(id);
            if (!QDir().mkpath(packageDir)) {
                connection->sendResponse(500, "application/json",
                                         jsonResponse({{QStringLiteral("error"), QStringLiteral("cannot create package directory")}}));
                return;
            }

            const QString packagePath = QDir(packageDir).filePath(package.fileName);
            QFile packageFile(packagePath);
            if (!packageFile.open(QIODevice::WriteOnly) || packageFile.write(package.bytes) != package.bytes.size()) {
                connection->sendResponse(500, "application/json",
                                         jsonResponse({{QStringLiteral("error"), QStringLiteral("cannot store package")}}));
                return;
            }
            packageFile.close();

            const QString encodedFileName = QString::fromUtf8(QUrl::toPercentEncoding(package.fileName));
            jobs.append(QJsonObject{
                {QStringLiteral("id"), id},
                {QStringLiteral("type"), packageTypeForFile(package.fileName)},
                {QStringLiteral("packageName"), package.fileName},
                {QStringLiteral("downloadUrl"), QStringLiteral("/packages/%1/%2").arg(id, encodedFileName)},
                {QStringLiteral("arguments"), package.arguments},
                {QStringLiteral("targetDir"), package.targetDir},
                {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}
            });
        }

        for (const QJsonValue &job : std::as_const(jobs))
            broadcastEvent("install", QJsonDocument(job.toObject()).toJson(QJsonDocument::Compact), false);

        connection->sendResponse(201, "application/json", jsonResponse({
            {QStringLiteral("ok"), true},
            {QStringLiteral("jobs"), jobs},
            {QStringLiteral("jobCount"), jobs.size()},
            {QStringLiteral("clientCount"), statusJson().value(QStringLiteral("clients")).toInt()}
        }));
    }

    bool parseCreateJobBody(const HttpRequest &request, QVector<PackageUpload> *packages,
                            QString *errorMessage) const
    {
        const QByteArray contentType = request.headers.value("content-type").toLower();
        if (contentType.startsWith("application/json")) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(request.body, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                *errorMessage = QStringLiteral("invalid json");
                return false;
            }

            const QJsonObject input = document.object();
            const QJsonArray inputJobs = input.value(QStringLiteral("jobs")).toArray();
            if (!inputJobs.isEmpty()) {
                for (const QJsonValue &value : inputJobs) {
                    const QJsonObject job = value.toObject();
                    packages->append(PackageUpload{
                        sanitizeFileName(job.value(QStringLiteral("fileName")).toString()),
                        QByteArray::fromBase64(job.value(QStringLiteral("fileDataBase64")).toString().toUtf8()),
                        job.value(QStringLiteral("arguments")).toString(),
                        job.value(QStringLiteral("targetDir")).toString()
                    });
                }
                return true;
            }

            packages->append(PackageUpload{
                sanitizeFileName(input.value(QStringLiteral("fileName")).toString()),
                QByteArray::fromBase64(input.value(QStringLiteral("fileDataBase64")).toString().toUtf8()),
                input.value(QStringLiteral("arguments")).toString(),
                input.value(QStringLiteral("targetDir")).toString()
            });
            return true;
        }

        if (!contentType.startsWith("multipart/form-data")) {
            *errorMessage = QStringLiteral("unsupported content type");
            return false;
        }

        const QRegularExpression boundaryExpression(QStringLiteral("boundary=(?:\"([^\"]+)\"|([^;]+))"));
        const QRegularExpressionMatch boundaryMatch = boundaryExpression.match(QString::fromUtf8(request.headers.value("content-type")));
        const QString boundary = boundaryMatch.hasMatch()
                                     ? (!boundaryMatch.captured(1).isEmpty() ? boundaryMatch.captured(1)
                                                                             : boundaryMatch.captured(2).trimmed())
                                     : QString();
        if (boundary.isEmpty()) {
            *errorMessage = QStringLiteral("missing multipart boundary");
            return false;
        }

        const QByteArray delimiter = "--" + boundary.toUtf8();
        qsizetype position = 0;
        QMap<int, PackageUpload> indexedPackages;
        QString globalArguments;
        QString globalTargetDir;
        int nextUploadIndex = 0;
        while (true) {
            qsizetype partStart = request.body.indexOf(delimiter, position);
            if (partStart < 0)
                break;
            partStart += delimiter.size();
            if (request.body.mid(partStart, 2) == "--")
                break;
            if (request.body.mid(partStart, 2) == "\r\n")
                partStart += 2;

            const qsizetype headerEnd = request.body.indexOf("\r\n\r\n", partStart);
            if (headerEnd < 0) {
                *errorMessage = QStringLiteral("invalid multipart part");
                return false;
            }

            const QByteArray headerBlock = request.body.mid(partStart, headerEnd - partStart);
            QHash<QByteArray, QByteArray> partHeaders;
            for (const QByteArray &lineWithEnd : headerBlock.split('\n')) {
                const QByteArray line = lineWithEnd.trimmed();
                const qsizetype separator = line.indexOf(':');
                if (separator > 0)
                    partHeaders.insert(line.left(separator).toLower(), line.mid(separator + 1).trimmed());
            }

            const qsizetype dataStart = headerEnd + 4;
            const qsizetype nextDelimiter = request.body.indexOf("\r\n" + delimiter, dataStart);
            if (nextDelimiter < 0) {
                *errorMessage = QStringLiteral("unterminated multipart part");
                return false;
            }

            const QByteArray contentDisposition = partHeaders.value("content-disposition");
            const QString fieldName = multipartParameter(contentDisposition, QStringLiteral("name"));
            const QString uploadedName = sanitizeFileName(multipartParameter(contentDisposition, QStringLiteral("filename")));
            const QByteArray partBody = request.body.mid(dataStart, nextDelimiter - dataStart);

            if (!uploadedName.isEmpty()) {
                int index = indexedField(fieldName, QStringLiteral("packageFile"));
                if (index < 0)
                    index = nextUploadIndex;
                nextUploadIndex = std::max(nextUploadIndex, index + 1);
                indexedPackages[index].fileName = uploadedName;
                indexedPackages[index].bytes = partBody;
            } else if (fieldName == QStringLiteral("arguments")) {
                globalArguments = QString::fromUtf8(partBody).trimmed();
            } else if (fieldName == QStringLiteral("targetDir")) {
                globalTargetDir = QString::fromUtf8(partBody).trimmed();
            } else {
                const int argumentsIndex = indexedField(fieldName, QStringLiteral("arguments"));
                const int targetDirIndex = indexedField(fieldName, QStringLiteral("targetDir"));
                if (argumentsIndex >= 0) {
                    indexedPackages[argumentsIndex].arguments = QString::fromUtf8(partBody).trimmed();
                } else if (targetDirIndex >= 0) {
                    indexedPackages[targetDirIndex].targetDir = QString::fromUtf8(partBody).trimmed();
                }
            }

            position = nextDelimiter + 2;
        }

        for (PackageUpload package : indexedPackages) {
            if (package.arguments.isEmpty())
                package.arguments = globalArguments;
            if (package.targetDir.isEmpty())
                package.targetDir = globalTargetDir;
            if (!package.fileName.isEmpty())
                packages->append(std::move(package));
        }

        return true;
    }

    void handleReport(const HttpRequest &request, HttpConnection *connection)
    {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(request.body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            connection->sendResponse(400, "application/json",
                                     jsonResponse({{QStringLiteral("error"), QStringLiteral("invalid json")}}));
            return;
        }

        QJsonObject report = document.object();
        report.insert(QStringLiteral("receivedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        m_reports.prepend(report);
        while (m_reports.size() > 50)
            m_reports.removeLast();

        broadcastEvent("report", QJsonDocument(report).toJson(QJsonDocument::Compact), true);
        connection->sendResponse(201, "application/json", jsonResponse({{QStringLiteral("ok"), true}}));
    }

    void servePackage(const QString &path, HttpConnection *connection)
    {
        const QStringList parts = path.split('/', Qt::SkipEmptyParts);
        if (parts.size() != 3 || parts.at(0) != QStringLiteral("packages")) {
            connection->sendResponse(404, "application/json",
                                     jsonResponse({{QStringLiteral("error"), QStringLiteral("not found")}}));
            return;
        }

        const QString id = parts.at(1);
        const QString fileName = sanitizeFileName(QUrl::fromPercentEncoding(parts.at(2).toUtf8()));
        const QString packagePath = QDir(QDir(m_packageRoot).filePath(id)).filePath(fileName);
        const QFileInfo packageInfo(packagePath);
        const QFileInfo rootInfo(m_packageRoot);
        if (!packageInfo.exists() || !packageInfo.isFile() ||
            !packageInfo.canonicalFilePath().startsWith(rootInfo.canonicalFilePath() + QDir::separator())) {
            connection->sendResponse(404, "application/json",
                                     jsonResponse({{QStringLiteral("error"), QStringLiteral("package not found")}}));
            return;
        }

        QFile file(packageInfo.filePath());
        if (!file.open(QIODevice::ReadOnly)) {
            connection->sendResponse(500, "application/json",
                                     jsonResponse({{QStringLiteral("error"), QStringLiteral("cannot read package")}}));
            return;
        }
        connection->sendResponse(200, "application/octet-stream", file.readAll(), {
            {"Content-Disposition", "attachment; filename=\"" + packageInfo.fileName().toUtf8() + "\""}
        });
    }

    void removeSse(QTcpSocket *socket)
    {
        m_sseConnections.erase(std::remove_if(m_sseConnections.begin(), m_sseConnections.end(),
                                              [socket](const SseConnection &connection) {
                                                  return connection.socket == socket || !connection.socket;
                                              }),
                               m_sseConnections.end());
    }

    void broadcastEvent(const QByteArray &event, const QByteArray &data, bool includeAdmins)
    {
        for (const SseConnection &connection : std::as_const(m_sseConnections)) {
            if (!connection.socket)
                continue;
            if (!includeAdmins && connection.role == QStringLiteral("admin"))
                continue;
            writeSse(connection.socket, event, data);
        }
    }

    void broadcastRaw(const QByteArray &data, bool includeAdmins)
    {
        for (const SseConnection &connection : std::as_const(m_sseConnections)) {
            if (!connection.socket)
                continue;
            if (!includeAdmins && connection.role == QStringLiteral("admin"))
                continue;
            connection.socket->write(data);
        }
    }

    static void writeSse(QTcpSocket *socket, const QByteArray &event, const QByteArray &data)
    {
        if (!socket || socket->state() != QAbstractSocket::ConnectedState)
            return;

        socket->write("event: " + event + "\n");
        const QList<QByteArray> lines = data.split('\n');
        for (QByteArray line : lines) {
            if (line.endsWith('\r'))
                line.chop(1);
            if (!line.isEmpty())
                socket->write("data: " + line + "\n");
        }
        socket->write("\n");
    }

    QByteArray adminHtml() const
    {
        return R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Silence Installer</title>
  <style>
    :root { color-scheme: light; font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; }
    body { margin: 0; background: #f6f7f9; color: #1f2933; }
    main { max-width: 920px; margin: 32px auto; padding: 0 20px; }
    h1 { font-size: 24px; margin: 0 0 20px; }
    section { background: #fff; border: 1px solid #d9dee7; border-radius: 6px; padding: 18px; margin-bottom: 16px; }
    label { display: block; font-weight: 600; margin: 14px 0 6px; }
    input { width: 100%; box-sizing: border-box; padding: 9px 10px; border: 1px solid #b8c0cc; border-radius: 4px; font: inherit; }
    button { margin-top: 16px; padding: 9px 14px; border: 0; border-radius: 4px; background: #1f6feb; color: white; font-weight: 600; cursor: pointer; }
    button:disabled { background: #9aa4b2; cursor: wait; }
    .row { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
    .task-list { display: grid; gap: 10px; margin-top: 14px; }
    .task { border: 1px solid #d9dee7; border-radius: 6px; padding: 12px; background: #fbfcfe; }
    .task-head { display: flex; justify-content: space-between; gap: 12px; align-items: baseline; margin-bottom: 10px; }
    .task-name { font-weight: 600; overflow-wrap: anywhere; }
    .task-meta { display: flex; align-items: center; gap: 8px; flex-shrink: 0; }
    .badge { color: #4b5563; background: #eef2f7; border-radius: 999px; padding: 2px 8px; font-size: 12px; }
    .remove-task { margin: 0; padding: 3px 8px; background: #e5e7eb; color: #1f2933; font-size: 12px; }
    .unsupported { border-color: #f2b8b5; background: #fff7f7; }
    .muted { color: #5f6b7a; font-size: 13px; }
    pre { max-height: 280px; overflow: auto; background: #111827; color: #d1d5db; padding: 12px; border-radius: 4px; white-space: pre-wrap; }
    @media (max-width: 720px) { .row { grid-template-columns: 1fr; } main { margin: 20px auto; } }
  </style>
</head>
<body>
<main>
  <h1>Silence Installer 管理</h1>
  <section>
    <div class="row">
      <div><strong>在线客户端</strong><div id="clients" class="muted">读取中</div></div>
      <div><strong>待下发任务</strong><div id="detected" class="muted">请选择安装包</div></div>
    </div>
  </section>
  <section>
    <form id="jobForm">
      <label for="packageFile">安装包</label>
      <input id="packageFile" type="file" multiple>
      <div id="taskList" class="task-list"></div>
      <button id="submitBtn" type="submit">批量推送安装任务</button>
    </form>
  </section>
  <section>
    <strong>事件</strong>
    <pre id="log"></pre>
  </section>
</main>
<script>
const fileInput = document.getElementById('packageFile');
const detected = document.getElementById('detected');
const log = document.getElementById('log');
const clients = document.getElementById('clients');
const submitBtn = document.getElementById('submitBtn');
const taskList = document.getElementById('taskList');
const selectedTasks = [];

function appendLog(message, data) {
  const text = `[${new Date().toLocaleTimeString()}] ${message}` + (data ? `\n${JSON.stringify(data, null, 2)}` : '');
  log.textContent = `${text}\n\n${log.textContent}`.slice(0, 12000);
}

function detectType(name) {
  const ext = name.split('.').pop().toLowerCase();
  if (ext === 'zip') return 'zip';
  if (['msi', 'exe', 'bat', 'cmd', 'ps1'].includes(ext)) return 'installer';
  return 'unsupported';
}

function defaultArguments(fileName) {
  const lower = fileName.toLowerCase();
  if (lower.endsWith('.msi')) return '/qn /norestart';
  if (lower.endsWith('.exe')) return '/S';
  return '';
}

function addFiles(files) {
  Array.from(files).forEach(file => {
    selectedTasks.push({
      file,
      type: detectType(file.name),
      arguments: defaultArguments(file.name),
      targetDir: ''
    });
  });
  fileInput.value = '';
  renderTasks();
}

function renderTasks() {
  taskList.textContent = '';
  if (!selectedTasks.length) {
    detected.textContent = '请选择安装包';
    return;
  }

  const supported = selectedTasks.filter(task => task.type !== 'unsupported').length;
  detected.textContent = `${selectedTasks.length} 个文件，${supported} 个可下发`;

  selectedTasks.forEach((entry, index) => {
    const file = entry.file;
    const type = entry.type;
    const task = document.createElement('div');
    task.className = `task ${type === 'unsupported' ? 'unsupported' : ''}`;
    task.dataset.index = String(index);
    task.dataset.type = type;

    const head = document.createElement('div');
    head.className = 'task-head';
    const name = document.createElement('div');
    name.className = 'task-name';
    name.textContent = file.name;
    const badge = document.createElement('span');
    badge.className = 'badge';
    badge.textContent = type;
    const remove = document.createElement('button');
    remove.className = 'remove-task';
    remove.type = 'button';
    remove.textContent = '移除';
    remove.addEventListener('click', () => {
      selectedTasks.splice(index, 1);
      renderTasks();
    });
    const meta = document.createElement('div');
    meta.className = 'task-meta';
    meta.append(badge, remove);
    head.append(name, meta);
    task.append(head);

    if (type === 'zip') {
      const label = document.createElement('label');
      label.htmlFor = `targetDir_${index}`;
      label.textContent = 'ZIP 解压目录';
      const input = document.createElement('input');
      input.id = `targetDir_${index}`;
      input.value = entry.targetDir;
      input.placeholder = '例如 C:\\Program Files\\AppName';
      input.addEventListener('input', () => {
        entry.targetDir = input.value;
      });
      task.append(label, input);
    } else if (type === 'installer') {
      const label = document.createElement('label');
      label.htmlFor = `arguments_${index}`;
      label.textContent = '安装参数';
      const input = document.createElement('input');
      input.id = `arguments_${index}`;
      input.value = entry.arguments;
      input.placeholder = file.name.toLowerCase().endsWith('.msi') ? '/qn /norestart' : '/S 或 /quiet';
      input.addEventListener('input', () => {
        entry.arguments = input.value;
      });
      task.append(label, input);
    } else {
      const info = document.createElement('div');
      info.className = 'muted';
      info.textContent = '不支持该文件类型';
      task.append(info);
    }

    taskList.append(task);
  });
}

fileInput.addEventListener('change', () => {
  addFiles(fileInput.files);
});
if (fileInput.files && fileInput.files.length) {
  addFiles(fileInput.files);
} else {
  renderTasks();
}

document.getElementById('jobForm').addEventListener('submit', async event => {
  event.preventDefault();
  if (!selectedTasks.length) return;
  submitBtn.disabled = true;
  try {
    const payload = new FormData();
    let jobCount = 0;
    selectedTasks.forEach((entry, index) => {
      const file = entry.file;
      const type = entry.type;
      if (type === 'unsupported') return;
      payload.append(`packageFile_${index}`, file);
      if (type === 'zip') {
        payload.append(`targetDir_${index}`, entry.targetDir);
      } else {
        payload.append(`arguments_${index}`, entry.arguments);
      }
      jobCount += 1;
    });
    if (!jobCount) throw new Error('没有可下发的安装包');
    const response = await fetch('/api/jobs', {
      method: 'POST',
      body: payload
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || response.statusText);
    appendLog(`已推送 ${result.jobCount || jobCount} 个安装任务`, result);
    selectedTasks.length = 0;
    renderTasks();
  } catch (error) {
    appendLog('推送失败', {error: String(error)});
  } finally {
    submitBtn.disabled = false;
  }
});

async function refreshStatus() {
  try {
    const response = await fetch('/api/status');
    const status = await response.json();
    clients.textContent = `${status.clients} 个客户端，${status.admins} 个管理连接`;
  } catch {
    clients.textContent = '状态读取失败';
  }
}
setInterval(refreshStatus, 2500);
refreshStatus();

const events = new EventSource('/events?role=admin&name=browser');
events.addEventListener('report', event => appendLog('客户端报告', JSON.parse(event.data)));
events.addEventListener('hello', event => appendLog('SSE 已连接', JSON.parse(event.data)));
events.onerror = () => appendLog('SSE 连接异常');
</script>
</body>
</html>)HTML";
    }

    QTcpServer m_tcpServer;
    QVector<SseConnection> m_sseConnections;
    QTimer m_heartbeatTimer;
    QString m_packageRoot;
    QJsonArray m_reports;
};

HttpConnection::HttpConnection(InstallerServer *server, QTcpSocket *socket)
    : QObject(server),
      m_server(server),
      m_socket(socket)
{
    connect(socket, &QTcpSocket::readyRead, this, [this] { onReadyRead(); });
    connect(socket, &QTcpSocket::disconnected, this, [this] {
        if (m_socket)
            m_socket->deleteLater();
        deleteLater();
    });
}

void HttpConnection::onReadyRead()
{
    if (!m_socket)
        return;

    m_buffer += m_socket->readAll();
    HttpRequest request;
    if (tryParseRequest(&request))
        m_server->handleRequest(request, this);
}

bool HttpConnection::tryParseRequest(HttpRequest *request)
{
    const qsizetype headerEnd = m_buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0)
        return false;

    const QByteArray headerBlock = m_buffer.left(headerEnd);
    const QList<QByteArray> lines = headerBlock.split('\n');
    if (lines.isEmpty())
        return false;

    const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
    if (requestLine.size() < 2) {
        sendResponse(400, "application/json",
                     jsonResponse({{QStringLiteral("error"), QStringLiteral("invalid request line")}}));
        return false;
    }

    QHash<QByteArray, QByteArray> headers;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const qsizetype separator = line.indexOf(':');
        if (separator <= 0)
            continue;
        headers.insert(line.left(separator).toLower(), line.mid(separator + 1).trimmed());
    }

    bool ok = false;
    const int contentLength = headers.value("content-length", "0").toInt(&ok);
    if (!ok || contentLength < 0) {
        sendResponse(400, "application/json",
                     jsonResponse({{QStringLiteral("error"), QStringLiteral("invalid content length")}}));
        return false;
    }
    if (contentLength > 1024 * 1024 * 1024) {
        sendResponse(413, "application/json",
                     jsonResponse({{QStringLiteral("error"), QStringLiteral("request too large")}}));
        return false;
    }

    const qsizetype bodyStart = headerEnd + 4;
    if (m_buffer.size() < bodyStart + contentLength)
        return false;

    request->method = requestLine.at(0).toUpper();
    request->url = QUrl::fromEncoded(requestLine.at(1));
    request->headers = headers;
    request->body = m_buffer.mid(bodyStart, contentLength);
    return true;
}

void HttpConnection::sendResponse(int statusCode, const QByteArray &contentType, const QByteArray &body,
                                  const QList<QPair<QByteArray, QByteArray>> &extraHeaders)
{
    if (!m_socket)
        return;

    QByteArray response;
    response += "HTTP/1.1 " + statusLine(statusCode) + "\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Connection: close\r\n";
    response += "Access-Control-Allow-Origin: *\r\n";
    response += "Access-Control-Allow-Headers: Content-Type\r\n";
    response += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    for (const auto &header : extraHeaders)
        response += header.first + ": " + header.second + "\r\n";
    response += "\r\n";
    response += body;

    m_socket->write(response);
    m_socket->disconnectFromHost();
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("SilenceInstallerServer"));

    quint16 port = 8080;
    QString packageRoot = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("packages"));

    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i) == QStringLiteral("--port") && i + 1 < args.size()) {
            bool ok = false;
            const int parsed = args.at(++i).toInt(&ok);
            if (ok && parsed > 0 && parsed <= 65535)
                port = static_cast<quint16>(parsed);
        } else if (args.at(i) == QStringLiteral("--packages") && i + 1 < args.size()) {
            packageRoot = args.at(++i);
        }
    }

    InstallerServer server(packageRoot, port);
    return app.exec();
}
