# GNZ Notification System

A lightweight Jira webhook notification system for Windows, consisting of two components:

- **GNZ Notification Server** — a Windows Service that receives Jira webhooks and exposes a REST API for agents to poll.
- **GNZ Notification Agent** — a system-tray application that polls the server and delivers desktop notifications to individual users.

---

## Table of Contents

1. [Prerequisites](#prerequisites)
2. [Architecture](#architecture)
3. [Building](#building)
4. [GNZ Notification Server](#gnz-notification-server)
5. [GNZ Notification Agent](#gnz-notification-agent)
6. [Jira Webhook Configuration](#jira-webhook-configuration)
7. [Firewall Rules](#firewall-rules)
8. [Troubleshooting](#troubleshooting)

---

## Prerequisites

| Requirement | Details |
|---|---|
| Operating System | Windows 7 or later (64-bit recommended) |
| Compiler | MSVC — Visual Studio 2019 or later (Developer Command Prompt) |
| Jira | Server, Data Center, or Cloud with webhook support |
| Network | The server machine must be reachable from the Jira instance on the webhook port |

No third-party libraries are required. The server uses raw Winsock2; the agent uses WinHTTP, both of which ship with Windows 7 and later.

---

## Architecture

```
┌─────────────┐   POST /webhook     ┌──────────────────────────┐
│ Jira Server │ ──────────────────▶ │  GNZ Notification Server │
└─────────────┘   port 8080         │  (Windows Service)        │
                                    │                           │
                                    │  Stores alerts in memory  │
                                    │  (configurable cache)     │
                                    └──────────┬────────────────┘
                                               │  GET /api/alerts
                                               │  port 8081
                              ┌────────────────▼────────────────┐
                              │    GNZ Notification Agent        │
                              │    (System Tray — per user)      │
                              │                                  │
                              │  Polls every 5 s (default)       │
                              │  Delivers banner / sound / icon  │
                              └──────────────────────────────────┘
```

The server and agent can run on the **same machine** or on **separate machines**. Multiple agents on different workstations can connect to the same server simultaneously.

---

## Building

Open a **Visual Studio Developer Command Prompt** and run the appropriate command.

### Server

```bat
cl /EHsc /std:c++17 /W3 /O2 gnz_notification_server.cpp ^
   /link ws2_32.lib advapi32.lib
```

### Agent

The agent requires a resource file for the application icon. The project must contain:

| File | Purpose |
|---|---|
| `gnz_notification_agent.cpp` | Main source |
| `resource.h` | Defines `IDI_GNZNOTIFICATIONAGENT` |
| `GNZNotificationAgent.rc` | Embeds the `.ico` file |
| `GNZNotificationAgent.ico` | Application icon |

**`resource.h`** minimum content:
```c
#pragma once
#define IDI_GNZNOTIFICATIONAGENT 101
```

**`GNZNotificationAgent.rc`** minimum content:
```rc
#include "resource.h"
IDI_GNZNOTIFICATIONAGENT ICON "GNZNotificationAgent.ico"
```

Build from the command line:
```bat
cl /EHsc /std:c++17 /W3 /O2 /DUNICODE /D_UNICODE gnz_notification_agent.cpp ^
   /link winhttp.lib comctl32.lib shell32.lib user32.lib gdi32.lib ^
   /SUBSYSTEM:WINDOWS
```

Or build through Visual Studio: open the project, set the platform to **x64**, and build normally. The `.rc` file is compiled automatically by the IDE.

---

## GNZ Notification Server

### Overview

The server runs as a Windows Service and listens on two HTTP ports:

| Port | Purpose |
|---|---|
| **8080** (default) | Receives Jira webhook `POST /webhook` requests |
| **8081** (default) | Exposes a REST API for agents to poll |

Alerts are stored in memory up to a configurable cache size. The oldest alerts are discarded automatically when the cache is full.

### Installation

Run the following from an **elevated (Administrator) command prompt**:

```bat
gnz_notification_server.exe install
```

This registers the service for automatic start at boot and writes default registry values to `HKLM\SOFTWARE\GNZ\NotificationService`.

To remove the service:

```bat
gnz_notification_server.exe uninstall
```

### Starting and Stopping

Using the Service Control Manager:

```bat
sc start GNZ_Notification_Service
sc stop  GNZ_Notification_Service
```

Or open **Services** (`services.msc`), find **GNZ Notification Service**, and use the toolbar buttons.

### Console / Debug Mode

To run the server interactively in a command window (useful for diagnosing problems):

```bat
gnz_notification_server.exe run
```

The console mode prints diagnostic output for every incoming connection and stored alert:

```
[http] connection accepted on port
[webhook] POST /webhook  body_len=1423
[webhook] stored alert id=1  event=jira:issue_created  key=PROJ-123  link=http://jira/browse/PROJ-123
```

Press **Ctrl+C** to stop.

### Registry Configuration

All settings are stored under:

```
HKEY_LOCAL_MACHINE\SOFTWARE\GNZ\NotificationService
```

| Value | Type | Default | Description |
|---|---|---|---|
| `CacheSize` | `REG_DWORD` | `300` | Maximum number of alerts kept in memory. Oldest are discarded when the limit is reached. |
| `WebhookPort` | `REG_DWORD` | `8080` | TCP port on which the server listens for Jira webhook `POST` requests. |
| `ApiPort` | `REG_DWORD` | `8081` | TCP port on which the server exposes the agent REST API. |
| `JiraPath` | `REG_SZ` | *(empty)* | Base URL of the Jira instance, e.g. `http://jira.example.com`. Used to build browse links (`/browse/PROJ-123`). If left empty, the server auto-detects the URL from the `self` field in each incoming webhook payload. |

**Changing settings takes effect immediately** — the server reads `JiraPath` fresh on every webhook event. Changes to port numbers and cache size require a service restart.

To edit with regedit:
1. Press `Win+R`, type `regedit`, press Enter.
2. Navigate to `HKEY_LOCAL_MACHINE\SOFTWARE\GNZ\NotificationService`.
3. Double-click the value to edit it.

To edit from the command line:
```bat
:: Set Jira base URL
reg add "HKLM\SOFTWARE\GNZ\NotificationService" /v JiraPath /t REG_SZ /d "http://jira.example.com" /f

:: Change webhook port
reg add "HKLM\SOFTWARE\GNZ\NotificationService" /v WebhookPort /t REG_DWORD /d 9090 /f
```

### REST API Reference (Port 8081)

These endpoints are used by the agent but can also be queried manually for diagnostics.

| Method | Path | Description |
|---|---|---|
| `GET` | `/api/count` | Returns the number of alerts currently stored: `{"count": N}` |
| `GET` | `/api/alerts` | Returns all stored alerts as a JSON array |
| `GET` | `/api/alerts?from=N1&to=N2` | Returns a 0-based index range of alerts (inclusive) |
| `DELETE` | `/api/alerts` | Clears all stored alerts |
| `DELETE` | `/api/alerts/:id` | Removes a single alert by its numeric ID |

**Example — fetch all alerts:**
```bat
curl http://localhost:8081/api/alerts
```

**Example — send a test webhook:**
```bat
curl -X POST http://localhost:8080/webhook ^
  -H "Content-Type: application/json" ^
  -d "{\"webhookEvent\":\"jira:issue_created\",\"issue\":{\"key\":\"TEST-1\",\"self\":\"http://jira.example.com/rest/api/2/issue/1\",\"fields\":{\"summary\":\"Test alert\",\"priority\":{\"name\":\"High\"},\"status\":{\"name\":\"Open\"},\"assignee\":{\"name\":\"jdoe\"},\"reporter\":{\"displayName\":\"Jane Smith\"}}}}"
```

Expected response: `{"received":true,"id":1}`

### Alert JSON Schema

Each alert returned by the API has the following fields:

| Field | Type | Description |
|---|---|---|
| `id` | integer | Auto-incrementing alert ID assigned by the server |
| `timestamp` | string | Local server time when the alert was received (ISO 8601) |
| `event` | string | Jira webhook event type, e.g. `jira:issue_created` |
| `issue_key` | string | Jira issue key, e.g. `PROJ-123` |
| `summary` | string | Issue summary text |
| `project` | string | Project key, e.g. `PROJ` |
| `priority` | string | Issue priority name, e.g. `High` |
| `reporter` | string | Display name of the user who created the issue |
| `status` | string | Current issue status, e.g. `In Progress` |
| `link` | string | Full Jira browse URL for the issue, or empty if not determinable |
| `user` | string | Login name of the current assignee, or empty if unassigned |

---

## GNZ Notification Agent

### Overview

The agent runs as a **system-tray application** (no visible window at startup). It polls the server periodically, shows a banner notification for new alerts, and maintains an alerts list window.

Each user runs their own instance of the agent. Configuration is stored per-user in `HKCU`.

### Installation (Auto-start on Login)

To register the agent so it starts automatically when the current user logs in:

```bat
GNZNotificationAgent.exe install
```

To remove the auto-start registration:

```bat
GNZNotificationAgent.exe uninstall
```

Both commands show a confirmation dialog and exit immediately without launching the tray application.

To run the agent manually without installing:

```bat
GNZNotificationAgent.exe
```

The application icon appears in the system tray (notification area, bottom-right). If it is hidden behind the overflow arrow (`^`), expand the tray to find it.

### First-Time Configuration

Right-click the tray icon → **Configure…**

| Setting | Default | Description |
|---|---|---|
| Server address | `localhost` | Hostname or IP address of the machine running the server |
| API port | `8081` | Must match `ApiPort` in the server registry settings |
| Poll interval | `5000 ms` | How often the agent checks for new alerts. Minimum 1000 ms, maximum 300000 ms |
| Show banner | On | Display a popup notification in the bottom-right corner when a new alert arrives |
| Play sound | On | Play the Windows default notification sound for new alerts |
| Change tray icon | Off | Switch the tray icon to a warning symbol while unacknowledged alerts are pending |
| Active | On | When off, the agent connects silently and updates the list but delivers no notifications |
| Addressed to me only | On | Deliver notifications only for alerts where the Jira issue assignee matches the current Windows login name. The alerts list always shows all events regardless of this setting |

Click **OK** to apply — the polling thread restarts immediately with the new settings.

### Tray Icon

| Action | Result |
|---|---|
| Double-click | Open the Alerts window |
| Right-click | Open context menu |
| Context menu → Show Alerts | Open the Alerts window |
| Context menu → Configure… | Open the configuration dialog |
| Context menu → Exit | Quit the agent |

The tray tooltip indicates connection status: `[connected]` or `[disconnected]`.

### Alerts Window

The alerts window displays all events received since the server started (subject to the server's cache limit). It opens by double-clicking the tray icon or selecting **Show Alerts** from the context menu.

**Columns:**

| Column | Description |
|---|---|
| ID | Numeric alert ID assigned by the server |
| Time | Timestamp when the alert was received by the server |
| Link | Full Jira browse URL (double-click to open in the default browser) |
| Event | Jira webhook event type |
| Key | Jira issue key (e.g. `PROJ-123`) |
| User | Login name of the assignee |
| Summary | Issue summary text |

**Sorting:** Click any column header to sort by that column. Click the same header again to reverse the sort direction. The ID column sorts numerically; all others sort alphabetically.

**Opening links:** Double-click a row in the **Link** column to open the Jira issue in the default browser.

**Toolbar buttons:**

| Button | Action |
|---|---|
| Delete Selected | Removes the selected rows from the server and the local list |
| Clear All | Clears all alerts from the server after confirmation |
| Refresh | Wakes the poll thread immediately instead of waiting for the next interval |

Column widths and window position/size are saved to the registry when the window is closed and restored on next open.

### Banner Notification

When a new alert arrives, a banner popup appears in the bottom-right corner of the screen. The banner shows:

- **Line 1:** Issue key and priority, e.g. `PROJ-123  [High]`
- **Line 2:** Issue summary (truncated at 60 characters)
- **Line 3:** `Click to open in Jira` (only shown when a link is available)

Clicking anywhere on the banner opens the Jira issue in the default browser (if a link is available) and dismisses the banner. The banner dismisses automatically after 7 seconds.

### Registry Configuration

All settings are stored under:

```
HKEY_CURRENT_USER\SOFTWARE\GNZ\NotificationAgent
```

| Value | Type | Default | Description |
|---|---|---|---|
| `ServerAddress` | `REG_SZ` | `localhost` | Server hostname or IP |
| `ApiPort` | `REG_DWORD` | `8081` | Server API port |
| `PollInterval` | `REG_DWORD` | `5000` | Poll interval in milliseconds |
| `ShowBanner` | `REG_DWORD` | `1` | `1` = show banner notifications |
| `EmitSound` | `REG_DWORD` | `1` | `1` = play sound on new alert |
| `ChangeIcon` | `REG_DWORD` | `0` | `1` = change tray icon when alerts are pending |
| `Active` | `REG_DWORD` | `1` | `1` = deliver notifications |
| `AddressedOnly` | `REG_DWORD` | `1` | `1` = notify only for alerts assigned to the current user |
| `AlertsLeft` | `REG_DWORD` | *(auto)* | Saved X position of the Alerts window |
| `AlertsTop` | `REG_DWORD` | *(auto)* | Saved Y position of the Alerts window |
| `AlertsWidth` | `REG_DWORD` | `760` | Saved width of the Alerts window |
| `AlertsHeight` | `REG_DWORD` | `480` | Saved height of the Alerts window |
| `ColWidth0` – `ColWidth6` | `REG_DWORD` | *(defaults)* | Saved widths of each column in the Alerts window |

These values are managed automatically by the configuration dialog and the Alerts window. Direct registry editing is not normally required.

---

## Jira Webhook Configuration

1. Log in to Jira as an administrator.
2. Go to **Administration → System → WebHooks**.
3. Click **Create a WebHook**.
4. Fill in the form:

| Field | Value |
|---|---|
| **Name** | GNZ Notification |
| **URL** | `http://<server-hostname>:8080/webhook` |
| **Events** | Tick the issue events you want: *Issue Created*, *Issue Updated*, *Issue Deleted*, etc. |
| **Exclude body** | Leave **unchecked** — the server parses the JSON body for issue details |

5. Click **Create**.

Jira will `POST` to that URL every time a matching event fires. The server stores the alert immediately and makes it available to all polling agents.

**Testing the webhook:** Use the **Test** button on the webhook configuration page (Jira Server/Data Center), or trigger an event by creating or updating an issue. Watch the server console for:

```
[webhook] stored alert id=1  event=jira:issue_created  key=PROJ-123  link=http://jira.example.com/browse/PROJ-123
```

---

## Firewall Rules

The server machine requires two inbound TCP rules. Run from an **elevated command prompt**:

```bat
:: Allow Jira to send webhooks
netsh advfirewall firewall add rule ^
  name="GNZ Webhook" dir=in action=allow protocol=TCP localport=8080

:: Allow agents to poll
netsh advfirewall firewall add rule ^
  name="GNZ API" dir=in action=allow protocol=TCP localport=8081
```

To remove the rules:

```bat
netsh advfirewall firewall delete rule name="GNZ Webhook"
netsh advfirewall firewall delete rule name="GNZ API"
```

---

## Troubleshooting

### Tray icon does not appear after starting the agent

The tray icon may be hidden in the overflow area. Click the `^` arrow in the notification area to expand it. If the icon is not there at all, run the agent from a command prompt to check for error messages.

### No notifications are received

Work through these checks in order:

**1. Verify the server is listening**

```bat
netstat -an | findstr 8080
netstat -an | findstr 8081
```

Both ports should show `LISTENING`. If not, the service is not running or failed to bind (port already in use).

**2. Check tray tooltip**

Hover over the tray icon. The tooltip should show `[connected]`. If it shows `[disconnected]`, the agent cannot reach the server — check the configured server address and port.

**3. Verify the server receives webhooks**

Run the server in console mode (`gnz_notification_server.exe run`) and trigger a Jira event. The console should show:

```
[http] connection accepted on port
[webhook] POST /webhook  body_len=...
[webhook] stored alert id=1  ...
```

If the first line appears but not the others, the request is arriving but the body is not being processed — verify the Jira webhook has **Exclude body** unchecked.

**4. Test manually with curl**

From the machine running Jira, test connectivity to the server:

```bat
curl -v http://<server>:8080/webhook
```

A connection refused or timeout means a network or firewall issue.

**5. Check the "Active" and "Addressed to me only" settings**

Open the agent configuration (right-click tray → Configure…). Ensure **Active** is checked. If **Addressed to me only** is checked, notifications only fire for alerts where the Jira issue assignee matches your Windows login name exactly (case-insensitive).

### Links are empty

Set `JiraPath` in the server registry to your Jira base URL (e.g. `http://jira.example.com`). If left empty, the server attempts to auto-detect the URL from the `self` field in the webhook payload — this requires that Jira includes a valid `self` URL in the webhook body, which it does by default for Jira Server and Data Center.

### The wrong issue key appears (shows a user ID like JIRAUSER10071)

This was a known parsing bug now fixed. Ensure you are running the latest build of the server. The fix correctly scopes issue key extraction to the `issue` object in the webhook payload, avoiding false matches on the `user` object.

### JiraPath is cleared after running the install command

This was a known bug now fixed. The `install` command no longer writes any value for `JiraPath`, so a manually configured value is preserved across reinstalls.

### The agent shows a notification immediately on startup

This is expected behaviour only for **genuinely new alerts** — alerts that arrived while the agent was not running but after the server started. Alerts that were already present when the agent last ran should not trigger notifications on the next start. If they do, ensure you are running the latest build.
