# KiCad Copilot

KiCad Copilot is an AI assistant built into KiCad. You describe a board in plain words, and it:

1. creates the project,
2. draws the schematic (it finds parts in your libraries, or imports them from LCSC/EasyEDA when they're missing),
3. checks it with ERC,
4. builds the PCB, places parts and autoroutes with Freerouting,
5. adds a ground pour and checks it with DRC,
6. exports Gerbers, drill, BOM and pick-and-place files for JLCPCB or PCBWay,
7. hands them to the board house.

It works with Claude, OpenAI, any OpenAI-compatible server, or a local model through Ollama.

It is a patch series on top of KiCad `master` (base commit `48340549`, 2026-09-22), on a branch called **`kicad-copilot`**.

> **It never orders or pays for anything.** The last step uploads your files, or opens the quote page with your files ready. You review and check out yourself.

---

## What you can say to it

| Where | Example |
|---|---|
| **Project Manager** (the Copilot pane is on the right) | "Make a new project: a USB-C powered 5 V to 3.3 V board with an AMS1117, a power LED and a 4-pin header. Lay it out small, check it and prepare JLCPCB files." |
| | "Import LCSC part C2040 and use it for U1." |
| | "Export for PCBWay and send it to them." |
| **Schematic Editor** (View → Panels → Copilot) | "Add a 10 k pull-up from SDA to +3V3." · "Run ERC and fix what you can." · "Suggest a MOSFET for a 2 A load switch." |
| **PCB Editor** (View → Panels → Copilot, or the toolbar toggle) | "Review this board." · "Where is U3's decoupling?" · "Make all +5V tracks 0.5 mm." · "Autoroute and pour GND on both layers." |

The project-level Copilot can use every schematic and PCB tool. The editor panes are handy when you're working in one editor.

---

## How it stays safe

- **Allow edits** (a checkbox in each pane) must be ticked before it may change anything. Every change is one **Ctrl+Z** undo step labelled "Copilot: …".
- **It asks first** before it:
  - runs a program (Freerouting, the LCSC importer),
  - downloads something (Freerouting, a web page, a datasheet),
  - uploads your files (PCBWay).
- You can choose **Always Allow This Session** for most of these. For web pages and PDFs, that choice applies to one website at a time.
- Running an arbitrary shell command (`run_command`) asks **every time**, and the prompt shows the full command with any hidden characters made visible.
- **It won't act while you're mid-task.** If a dialog is open or you're in the middle of an action (routing, moving, a running DRC), the Copilot says "Waiting for…" and continues by itself once you finish or press Esc.
- Long operations (autorouting, downloads, kicad-cli exports) show a progress window with **Cancel**. **Stop** in the pane ends the current request.
- You can't close a window while the Copilot is running one of its tools there.
- **What gets sent to the model provider:** your prompts, plus the parts of the design the Copilot looks at (references, values, nets, coordinates). To keep everything on your computer, use Ollama.

---

## 1. Put it in your GitHub fork

You need Git for Windows (it includes Git Bash) or Git inside WSL.

1. On GitHub, open <https://github.com/KiCad/kicad-source-mirror> and click **Fork**.
2. Clone your fork. The repo is big, so `--filter=blob:none` keeps the download small:
   ```bash
   git clone --filter=blob:none https://github.com/<your-user>/kicad-source-mirror.git kicad
   cd kicad
   ```
3. Bring in the Copilot branch from the bundle file (adjust the path):
   ```bash
   git fetch /path/to/kicad-copilot.bundle kicad-copilot:kicad-copilot
   git checkout kicad-copilot
   git push -u origin kicad-copilot
   ```
   Or, with the patch files instead: `git checkout -b kicad-copilot 48340549`, then `git am /path/to/patches/*.patch`.

To update to newer KiCad later: `git fetch https://github.com/KiCad/kicad-source-mirror master`, then `git rebase FETCH_HEAD`.

---

## 2. Build it on Windows

### Option A: WSL2 + Ubuntu 24.04 (recommended)

KiCad runs as a Linux app and its windows appear on your Windows desktop. This is the setup it was tested on.

In **PowerShell (as Administrator)**:
```powershell
wsl --install -d Ubuntu-24.04
```
Reboot if asked, open **Ubuntu** from the Start menu, then run:

```bash
sudo apt update
sudo apt install -y git build-essential clang cmake ninja-build pkg-config gettext \
  libcairo2-dev libpng-dev libglu1-mesa-dev libgl1-mesa-dev libx11-dev mesa-common-dev \
  libgtk-3-dev libglm-dev libwxgtk3.2-dev libwxgtk-webview3.2-dev libbz2-dev libssl-dev \
  libzstd-dev zlib1g-dev python3-dev python3-venv python3-wxgtk4.0 swig \
  libocct-modeling-algorithms-dev libocct-modeling-data-dev libocct-data-exchange-dev \
  libocct-visualization-dev libocct-foundation-dev libocct-ocaf-dev \
  libgit2-dev libsecret-1-dev libboost-all-dev libcurl4-openssl-dev libngspice0-dev \
  unixodbc-dev libspnav-dev libnng-dev libprotobuf-dev protobuf-compiler \
  shared-mime-info libzint-dev libpoppler-dev libpoppler-glib-dev \
  libharfbuzz-dev libfontconfig-dev libglew-dev \
  kicad-footprints kicad-symbols kicad-templates \
  openjdk-25-jre                                    # for the Freerouting autorouter

# Build inside the Linux filesystem (~), not /mnt/c. It's much faster.
cd ~
git clone --filter=blob:none https://github.com/<your-user>/kicad-source-mirror.git kicad
cd kicad && git checkout kicad-copilot
mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/kicad-copilot \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DKICAD_BUILD_QA_TESTS=OFF -DKICAD_BUILD_I18N=OFF ..
ninja            # 30-90 min on a typical 8-16 core PC
ninja install
```

> **Why clang?** On Ubuntu 24.04, GCC 13 and 14 both crash with an internal compiler error on two of KiCad's own new files (`pcbnew/constraints/*.cpp`). That's upstream code, unrelated to the Copilot. Clang compiles them fine.
>
> **No `openjdk-25-jre` package?** Install Java 25 from <https://adoptium.net> instead. Freerouting 2.4 needs Java 25 or newer, and the Copilot automatically uses the newest Java it finds.

Start it with `~/kicad-copilot/bin/kicad`. Your Windows files are under `/mnt/c/Users/<you>/...`. The first time it starts, accept the default library tables.

### Option B: Native Windows (Visual Studio 2022 + vcpkg)

This is KiCad's official Windows developer build. It gives you a real `.exe` but takes more setup. Follow <https://dev-docs.kicad.org/en/build/windows-msvc/index.html>:

1. Install Visual Studio 2022 with *Desktop development with C++* and *C++ CMake tools for Windows*.
2. Clone vcpkg to a short path (e.g. `C:\vcpkg`), check out the commit KiCad pins, and run `.\bootstrap-vcpkg.bat`.
3. Install SWIG and set up KiCad's NuGet binary cache as the guide describes. Without the cache, dependencies take hours to build.
4. Copy `CMakePresets.json.sample` to `CMakePresets.json` and set `VCPKG_ROOT`.
5. Open the `kicad` folder in Visual Studio and build the `x64-Release` preset.
6. For autorouting, install **Java 25** (Eclipse Temurin from adoptium.net).

Nothing in the Copilot is Linux-specific. It uses KiCad's own HTTP, process and keychain code, and on Windows the API key goes into Windows Credential Manager.

---

## 3. Connect Claude

1. Create an API key at <https://console.anthropic.com/> → **API Keys**.
   - API usage is billed per token, separately from a Claude.ai subscription.
   - Chatting about a board costs cents. A whole "make me a board" run makes many requests, so it costs noticeably more. The pane shows the tokens used, and the Anthropic console shows the cost.
2. In the Copilot pane, click **Settings...**, pick **Anthropic (Claude)**, paste the key and click **Test Connection**.
3. Optionally, change **Model**. The default is `claude-sonnet-5`. Use `claude-opus-5-5` for harder designs, or `claude-haiku-4-5-20251001` for quick, cheap answers.
4. Leave **Web search** on so it can look up parts and datasheets.

The key is stored in the OS keychain, never in KiCad's settings files. You can also set `ANTHROPIC_API_KEY` (or `KICAD_COPILOT_API_KEY`, or `OPENAI_API_KEY`) in the environment instead.

**On WSL** there's usually no keychain running, so a key typed into Settings only lasts until you close KiCad (the dialog tells you when this happens). To keep it, add it to your shell profile once:
```bash
echo 'export ANTHROPIC_API_KEY=sk-ant-...your key...' >> ~/.bashrc && source ~/.bashrc
```

| Provider | Base URL (default) | Key | Notes |
|---|---|---|---|
| Anthropic (Claude) | `https://api.anthropic.com` | required | Best tool use. The only provider that can read datasheet PDFs and search the web. |
| OpenAI | `https://api.openai.com/v1` | required | Default model `gpt-5`. |
| Ollama (local) | `http://localhost:11434/v1` | none | Free and offline. Use a tool-capable model (`ollama pull qwen3`). Expect weaker results on full-board runs. |
| OpenAI-compatible | *(you set it)* | optional | LM Studio, llama.cpp, vLLM, OpenRouter, ... |

---

## 4. The helpers it uses

These all ask for approval the first time they're used:

- **Freerouting** (autorouter). The Copilot offers to download it into KiCad's cache folder. If you already have a `freerouting.jar`, set its path in Copilot Settings instead. It needs **Java 25+**.
- **easyeda2kicad** (imports LCSC parts with symbol, footprint and 3D model). On first use, it's installed into a private Python environment in KiCad's cache folder.
  - On Windows, KiCad's bundled Python is used (or `python` from your PATH).
  - On WSL, `python3-venv` is needed; it's in the apt line above.
  - Imported parts go into the project's `libs/lcsc.*` libraries.
- **kicad-cli** (Gerber, drill, BOM and pick-and-place export). It's part of the build.

## 5. Board houses

`export_fabrication` writes everything to `<project>/fabrication/<house>/`, then `send_to_board_house` hands it over:

- **JLCPCB**: The Copilot writes a Gerber zip, a BOM with LCSC part numbers (from each symbol's `LCSC` field), and a CPL (pick-and-place) file. It then opens JLCPCB's quote page and the output folder; drag the zip onto the page. JLCPCB has no public upload API.
- **PCBWay**: The Copilot uploads the package the same way PCBWay's official KiCad plugin does, then opens the quote page in your browser.
- **generic**: Only exports the files.

Check JLCPCB's 3D preview for part rotations before you order. Some footprints need a rotation correction there.

---

## 6. Tips

- **Enter** sends, **Shift+Enter** adds a new line, **Up** recalls your last prompt, **Esc** or **Stop** cancels.
- **New Chat** starts over. The editor panes also reset when you open a different file.
- A full board run can take many steps. If it says "Paused after N tool rounds", reply "continue".
- Autorouting is only a starting point. Ask it to fix what DRC reports, or finish critical nets by hand.
- The schematic is the source of truth for parts and values. On the board, ask it to "update from schematic" after schematic changes.

---

## 7. What's in the patch

```
include/copilot/, common/copilot/      core: providers, session/tool loop, registry, shared tools, settings
include/widgets/copilot_panel.h, common/widgets/copilot_panel.cpp       the chat pane
include/dialogs/dialog_copilot_settings.h, common/dialogs/...           settings dialog
pcbnew/copilot/                         PCB tools (inspection, review, layout, autoroute)
eeschema/copilot/                       schematic tools (symbols, wiring, ERC, custom symbols)
kicad/copilot/                          project-level orchestrator (projects, LCSC import, fab export)
*_edit_frame.*, kicad_manager_frame.*, tools/*, menubar*, toolbars*   pane, action and menu wiring
qa/tests/common/test_copilot.cpp        unit tests
```

Commits:
1. `3d-viewer: include wx/defs.h before testing __WXGTK3__`: a one-line fix for a current KiCad master bug (on Linux/WSL the PCB Editor fails to start with "Failed to load kiface library"). Drop it once upstream fixes it.
2. `Copilot: add provider-neutral AI assistant core and chat panel`
3. `Copilot: add the Copilot to the PCB editor`
4. `Copilot: add the Copilot to the schematic editor`
5. `Copilot: add the project-level Copilot to the project manager`
6. `Copilot: add unit tests`
7. `Copilot: add user guide` (this file, as `COPILOT.md` in the repo)

---

## How it was tested, and limits

**Tested** on Ubuntu 24.04, with a real KiCad build running in the real GUI. The model was a scripted, Claude-compatible test server, because the build machine has no API key or internet access to Anthropic. The test typed "make a new project…" into the project manager's Copilot and checked every step:

- **Project and schematic:** creating the project, library search, placing seven parts, wiring, ERC with 0 errors.
- **PCB:** update from schematic, outline, rules, placement.
- **Routing:** real Freerouting 2.4.1 on Java 25, with all 12 connections routed.
- **Checks:** GND pour and DRC with 0 errors.
- **Fab output:** real kicad-cli export (12 Gerber/drill files, BOM, CPL, IPC netlist).
- **Safety behaviour:**
  - approval prompts,
  - waiting while a dialog was open or an action was in progress, then continuing on its own,
  - re-opening the project.
- **Unit tests:** 14, also run under AddressSanitizer and UndefinedBehaviorSanitizer.
- **Code review:** two independent reviews, with every finding fixed.

**Not tested:**
- The live Claude API.
- LCSC/EasyEDA import and the PCBWay upload, because those sites weren't reachable from the build machine.
- The native MSVC build.

**Limits:**
- Replies arrive all at once; there's no streaming yet.
- Placement and routing are good for simple boards. Dense, high-speed or RF designs need your hands.
- Review heuristics find power nets by name (`+3V3`, `VCC`, `GND`, ...). Treat findings as prompts to check.
- The toolbar button reuses an existing KiCad icon.
