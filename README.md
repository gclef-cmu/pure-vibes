# Pure Vibes (Pd-vibes)

**Pure Vibes is an unofficial, experimental fork of [Pure Data](https://puredata.info) with added functionality for agentic AI usage**. It is vibe-coded and intended for experimental use only. It is not affiliated with or endorsed by Miller Puckette or the Pure Data community.

Pure Vibes integrates a [Model Context Protocol (MCP)](https://modelcontextprotocol.io/) directly into Pure Data, allowing AI agents (Claude, ChatGPT, etc.) to read, create, and manipulate Pd patches in real time. Once Pure Vibes is installed on your machine and registered w/ your AI agent, you can ask it to do things in natural language like "explain this patch I'm working on" or "build me a new 8 voice FM osc patch w/ real-time MIDI control".

---

## Quick Start

### 1. Install Pd-vibes

Download the latest release for your platform from the **[Releases](../../releases)** tab:

- **macOS**: Download `Pd-vibes-macos.zip`, unzip, drag `Pd-vibes.app` to Applications. On first launch, macOS may warn that it "can't check the app for malicious software." Right-click the app, choose **Open**, then click **Open** again to bypass this.
- **Windows**: Download `Pd-vibes-windows-x86_64.tar.gz`, extract, run `bin\pd.exe`
- **Linux**: Download `Pd-vibes-linux-x86_64.tar.gz`, extract, run `bin/pd`

Launch Pd-vibes. You will see an "MCP" checkbox in the main window (next to DSP). It is off by default. Enable it when you want AI tools to connect.

### 2. Connect to AI agent

Download and install an AI agent tool such as [Claude Desktop](https://code.claude.com/docs/en/desktop-quickstart#install). Other agents that support MCP will work as well with similar instructions.

Add this to your Claude Desktop config file:

**macOS**: `~/Library/Application Support/Claude/claude_desktop_config.json`
**Windows**: `%APPDATA%\Claude\claude_desktop_config.json`

**macOS config:**

```json
{
  "mcpServers": {
    "Pure Vibes": {
      "command": "/Applications/Pd-vibes.app/Contents/Resources/bin/pd-mcp"
    }
  }
}
```

**Windows config:**

```json
{
  "mcpServers": {
    "Pure Vibes": {
      "command": "C:\\Program Files\\pd-vibes\\bin\\pd-mcp.exe"
    }
  }
}
```

**Linux config:**

```json
{
  "mcpServers": {
    "Pure Vibes": {
      "command": "pd-mcp"
    }
  }
}
```

Restart Claude Desktop. Pure Vibes should appear as a connected MCP server.

### 3. Try it out

Open Pd-vibes, click to enable "MCP" in the main window, then ask Claude:

> "Using Pure Vibes, build me a simple synthesizer in Pure Data with an oscillator, envelope, and volume control"

Claude will use the MCP tools to create objects, wire them together, and you will see the patch build itself in real time.

Other things to try:

> "What patches do I have open?"

> "Turn on DSP"

> "Add a reverb to my patch"

---

## Pure Data tools exposed by the MCP

The built-in MCP server exposes 23 tools:

| Category    | Tools                                                                      |
| ----------- | -------------------------------------------------------------------------- |
| Patches     | `list_patches`, `get_patch_state`, `open_patch`, `save_patch`, `new_patch` |
| Objects     | `create_object`, `delete_object`, `modify_object`, `move_object`           |
| Connections | `connect`, `disconnect`                                                    |
| Batch       | `batch_update`, `clear_patch`                                              |
| Runtime     | `send_message`, `send_bang`, `set_number`                                  |
| DSP         | `set_dsp`, `get_dsp_state`                                                 |
| Selection   | `get_selection`                                                            |
| Docs        | `list_object_names`, `get_object_doc`                                      |
| Other       | `get_audio_midi_settings`, `get_pd_log`                                    |

---

## Advanced Configuration

### Connect to other MCP clients

Pd-vibes supports two MCP connection modes:

- **Claude Desktop via stdio proxy**: Point Claude at `pd-mcp`. The proxy speaks stdio to Claude and forwards tool calls to Pd-vibes over HTTP on localhost.
- **Direct Streamable HTTP**: Pd-vibes listens on `http://localhost:4330/mcp` when MCP is enabled. Any MCP client that supports Streamable HTTP can connect directly.

### MCP Configuration

- **Toggle**: Check/uncheck "MCP" in the main Pd window, or use the Media menu
- **Port**: Media > MCP Port... (default: 4330). CLI: `-mcpport 4331`
- **Network**: Media > MCP Allow Network (default: localhost only). CLI: `-mcpnetwork`
- **Default state**: MCP is off until you enable it via the checkbox or `-mcpport`
- **Disable**: Uncheck "MCP" or start with `-nomcp`

---

## Building from Source

### macOS (arm64 / x86_64)

Install Xcode command line tools and Homebrew, then:

```sh
brew install autoconf automake libtool gettext
LIBTOOLIZE=$(brew --prefix libtool)/bin/glibtoolize ./autogen.sh
./configure
make -j$(sysctl -n hw.logicalcpu)
```

To create an app bundle:

```sh
make app
# Creates Pd-vibes.app in the build directory
```

### Linux (Debian/Ubuntu)

```sh
sudo apt-get install autoconf automake libtool gettext \
    libasound2-dev libjack-jackd2-dev tcl-dev tk-dev
./autogen.sh
./configure
make -j$(nproc)
sudo make install
```

### Windows (MSYS2)

Install [MSYS2](https://www.msys2.org), open a MINGW64 shell, then:

```sh
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-autotools make autoconf automake libtool
./autogen.sh
./configure
make -j$(nproc)
```

For more detailed build instructions, see INSTALL.txt.

---

## About Pure Data

Pure Data (Pd) is a free, open-source visual programming language for multimedia,
created by Miller Puckette. For information about vanilla Pd, visit:

- https://puredata.info
- http://msp.ucsd.edu/software.html

---

## Copyright & Licensing

### Pure Data

Except as otherwise noted, all files in the Pd distribution are:

    Copyright (c) 1997-2024 Miller Puckette and others.

Licensed under the **BSD 3-Clause License**. See LICENSE.txt for details.

### cJSON (embedded JSON library)

The files `src/mcp/cJSON.c` and `src/mcp/cJSON.h` are from the
[cJSON](https://github.com/DaveGamble/cJSON) project (v1.7.18):

    Copyright (c) 2009-2017 Dave Gamble and cJSON contributors.

Licensed under the **MIT License**. The full license text is included at the
top of each file.

### MCP Server Code

The files `src/mcp/mcp_server.c`, `src/mcp/mcp_server.h`,
`src/mcp/mcp_tools.c`, `src/mcp/mcp_tools.h`, and `src/mcp/mcp_proxy.c`
are new additions to this fork, written for the Pure Vibes project.
They are released under the same
**BSD 3-Clause License** as the rest of Pure Data.

### Compatibility

The BSD 3-Clause (Pd) and MIT (cJSON) licenses are fully compatible.
Both are permissive open-source licenses that allow free use, modification,
and redistribution.
