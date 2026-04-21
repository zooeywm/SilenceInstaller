#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
        QString fileName;
        QByteArray packageBytes;
        QString installArguments;
        QString targetDir;
        QString parseError;
        if (!parseCreateJobBody(request, &fileName, &packageBytes, &installArguments, &targetDir, &parseError)) {
            connection->sendResponse(400, "application/json",
                                     jsonResponse({{QStringLiteral("error"), parseError}}));
            return;
        }

        const QString type = packageTypeForFile(fileName);

        if (fileName.isEmpty() || type.isEmpty() || packageBytes.isEmpty()) {
            connection->sendResponse(400, "application/json", jsonResponse({
                {QStringLiteral("error"), QStringLiteral("missing or unsupported package")}
            }));
            return;
        }

        const QString id = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmsszzz")) +
                           QStringLiteral("-") +
                           QString::number(QRandomGenerator::global()->bounded(100000, 999999));
        const QString packageDir = QDir(m_packageRoot).filePath(id);
        if (!QDir().mkpath(packageDir)) {
            connection->sendResponse(500, "application/json",
                                     jsonResponse({{QStringLiteral("error"), QStringLiteral("cannot create package directory")}}));
            return;
        }

        const QString packagePath = QDir(packageDir).filePath(fileName);
        QFile packageFile(packagePath);
        if (!packageFile.open(QIODevice::WriteOnly) || packageFile.write(packageBytes) != packageBytes.size()) {
            connection->sendResponse(500, "application/json",
                                     jsonResponse({{QStringLiteral("error"), QStringLiteral("cannot store package")}}));
            return;
        }
        packageFile.close();

        const QString encodedFileName = QString::fromUtf8(QUrl::toPercentEncoding(fileName));
        const QJsonObject job{
            {QStringLiteral("id"), id},
            {QStringLiteral("type"), type},
            {QStringLiteral("packageName"), fileName},
            {QStringLiteral("downloadUrl"), QStringLiteral("/packages/%1/%2").arg(id, encodedFileName)},
            {QStringLiteral("arguments"), installArguments},
            {QStringLiteral("targetDir"), targetDir},
            {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}
        };

        broadcastEvent("install", QJsonDocument(job).toJson(QJsonDocument::Compact), false);
        connection->sendResponse(201, "application/json", jsonResponse({
            {QStringLiteral("ok"), true},
            {QStringLiteral("job"), job},
            {QStringLiteral("clientCount"), statusJson().value(QStringLiteral("clients")).toInt()}
        }));
    }

    bool parseCreateJobBody(const HttpRequest &request, QString *fileName, QByteArray *packageBytes,
                            QString *installArguments, QString *targetDir, QString *errorMessage) const
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
            *fileName = sanitizeFileName(input.value(QStringLiteral("fileName")).toString());
            *packageBytes = QByteArray::fromBase64(input.value(QStringLiteral("fileDataBase64")).toString().toUtf8());
            *installArguments = input.value(QStringLiteral("arguments")).toString();
            *targetDir = input.value(QStringLiteral("targetDir")).toString();
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
                *fileName = uploadedName;
                *packageBytes = partBody;
            } else if (fieldName == QStringLiteral("arguments")) {
                *installArguments = QString::fromUtf8(partBody).trimmed();
            } else if (fieldName == QStringLiteral("targetDir")) {
                *targetDir = QString::fromUtf8(partBody).trimmed();
            }

            position = nextDelimiter + 2;
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
      <div><strong>识别类型</strong><div id="detected" class="muted">请选择安装包</div></div>
    </div>
  </section>
  <section>
    <form id="jobForm">
      <label for="packageFile">安装包</label>
      <input id="packageFile" type="file" required>
      <label for="arguments">安装参数</label>
      <input id="arguments" placeholder='例如 MSI: /qn /norestart，EXE: /S 或 /quiet'>
      <label for="targetDir">ZIP 解压目录</label>
      <input id="targetDir" placeholder='仅 zip 使用，例如 C:\Program Files\AppName'>
      <button id="submitBtn" type="submit">推送安装任务</button>
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

fileInput.addEventListener('change', () => {
  const file = fileInput.files[0];
  detected.textContent = file ? `${detectType(file.name)} (${file.name})` : '请选择安装包';
});

document.getElementById('jobForm').addEventListener('submit', async event => {
  event.preventDefault();
  const file = fileInput.files[0];
  if (!file) return;
  submitBtn.disabled = true;
  try {
    const payload = new FormData();
    payload.append('packageFile', file);
    payload.append('arguments', document.getElementById('arguments').value);
    payload.append('targetDir', document.getElementById('targetDir').value);
    const response = await fetch('/api/jobs', {
      method: 'POST',
      body: payload
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || response.statusText);
    appendLog('已推送安装任务', result);
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
