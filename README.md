# SilenceInstaller

SilenceInstaller is a Qt-based silent installation dispatcher. It contains one server binary and one Windows client binary.

The server provides a simple web management UI. Operators upload installer packages from the browser, and the server pushes installation jobs to connected clients through Server-Sent Events (SSE). The client keeps an SSE connection open, downloads assigned packages, runs the installer silently, reports the result, and removes the downloaded package files after the job finishes.

## Features

- Two binaries:
  - `SilenceInstallerServer`: HTTP/SSE server and web management UI.
  - `SilenceInstallerClient`: Windows installation client.
- Automatic package type detection:
  - `exe/msi/bat/cmd/ps1`: installer packages.
  - `zip`: archive packages.
- Batch job dispatch from the web UI:
  - Installer packages show an installation arguments field.
  - ZIP packages show an extraction directory field.
  - Each package can have its own parameters.
- Client-side job queue:
  - Multiple jobs can be received continuously.
  - Jobs are executed serially in the order received.
  - Parallel installation is intentionally not supported to avoid MSI locks, registry conflicts, service conflicts, and file conflicts.
- Client result reporting:
  - The client reports success or failure back to the server.
  - The client deletes the local job temporary directory after reporting.

## Requirements

- CMake 3.19+
- Qt 6.5+
- Qt modules:
  - `Core`
  - `Network`

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

On Windows, the project can also be opened and built with Qt Creator.

## Run the Server

```bash
./build/SilenceInstallerServer --port 8080 --packages packages
```

Options:

- `--port`: listen port. Default: `8080`.
- `--packages`: server-side package storage directory. Default: `packages` under the server executable directory.

Open the management UI:

```text
http://<server-ip>:8080/
```

## Run the Client

Windows client example:

```bat
SilenceInstallerClient.exe --server http://10.100.15.244:8080 --name KVM-WIN11
```

Options:

- `--server`: server base URL or full SSE URL.
- `--name`: client name shown in server status and job reports.
- `--reconnect-ms`: SSE reconnect delay in milliseconds. Default: `5000`.

The client connects to:

```text
http://<server-ip>:<port>/events?role=client&name=<client-name>
```

## Web UI Usage

1. Open the server web UI.
2. Select one or more package files.
3. If the native file picker only selects one file at a time, repeat selection. The page appends files to the pending job list.
4. Configure each package:
   - `exe/msi/bat/cmd/ps1`: enter installation arguments.
   - `zip`: enter the ZIP extraction directory.
5. Click `批量推送安装任务` to dispatch all pending jobs.

The server splits a batch submission into multiple SSE `install` events. The client queues those events and runs them serially.

## Default Installation Arguments

Client defaults:

- `msi`: `/qn /norestart`
- `exe`: `/S`
- `zip`: PowerShell `Expand-Archive`

ZIP extraction command:

```powershell
Expand-Archive -LiteralPath <zip> -DestinationPath <targetDir> -Force
```

Job execution mapping:

- `msi`: `msiexec.exe /i <package> <arguments>`
- `exe`: `<package> <arguments>`
- `bat/cmd`: `cmd.exe /c <package> <arguments>`
- `ps1`: `powershell.exe -NoProfile -ExecutionPolicy Bypass -File <package> <arguments>`

Silent arguments depend on the installer implementation. Some EXE installers do not support `/S`; use the argument supported by that specific installer.

## Administrator Privileges

Most installers require administrator privileges. The client process must run elevated. Otherwise installer startup may fail with:

```text
The requested operation requires elevation.
```

When debugging with Qt Creator, start Qt Creator as administrator first, then run `SilenceInstallerClient` from Qt Creator. The client process inherits the elevated token.

For production usage, run the client as a Windows service or through another controlled elevated startup mechanism.

## Local Client Cleanup

The client downloads each package to:

```text
%TEMP%\SilenceInstaller\<jobId>\<packageName>
```

After the install process exits, the client:

1. Sends the job report to the server.
2. Deletes the local temporary job directory:

```text
%TEMP%\SilenceInstaller\<jobId>\
```

Cleanup failure is logged but does not block the next queued job.

## HTTP API

### Management UI

```http
GET /
```

### SSE Event Stream

```http
GET /events?role=client&name=<client-name>
GET /events?role=admin&name=browser
```

### Server Status

```http
GET /api/status
```

Returns the number of connected clients, connected admin pages, and recent job reports.

### Create Installation Jobs

```http
POST /api/jobs
Content-Type: multipart/form-data
```

Single-job fields:

- `packageFile`
- `arguments`
- `targetDir`

Batch fields:

- `packageFile_0`
- `arguments_0`
- `targetDir_0`
- `packageFile_1`
- `arguments_1`
- `targetDir_1`

The response contains `jobs[]` and `jobCount`.

### Client Reports

```http
POST /api/reports
Content-Type: application/json
```

The client calls this endpoint after each job finishes.

## Limitations

- No authentication or authorization is implemented. Do not expose the server to untrusted networks.
- The built-in HTTP server is intended for simple internal management, not as a general-purpose high-performance web framework.
- Upload request size is limited to `1 GiB`.
- Installer execution is Windows-only on the client side.
- Batch jobs are executed serially on each client. Parallel installation is not supported.
- Silent installation arguments are installer-specific and may need to be adjusted per package.
