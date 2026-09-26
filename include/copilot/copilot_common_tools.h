/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <wx/string.h>

#include <copilot/copilot_tool_registry.h>


struct COPILOT_PROCESS_RESULT
{
    bool        started = false;
    bool        timedOut = false;
    bool        cancelled = false;  ///< The user pressed Cancel in the progress dialog.
    int         exitCode = -1;
    std::string output;         ///< Combined stdout + stderr (tail, if very long).
};


/**
 * Describes what the Copilot is doing for the lifetime of the object.  Long-running helpers
 * (external programs, downloads) show it in a progress dialog with a Cancel button.
 */
class COPILOT_ACTIVITY
{
public:
    explicit COPILOT_ACTIVITY( const wxString& aText );
    ~COPILOT_ACTIVITY();

    COPILOT_ACTIVITY( const COPILOT_ACTIVITY& ) = delete;
    COPILOT_ACTIVITY& operator=( const COPILOT_ACTIVITY& ) = delete;

    static wxString Current();

private:
    wxString m_previous;
};


/**
 * Run @a aWork on a worker thread while the GUI stays painted (user input is blocked; after a
 * moment a progress dialog with a Cancel button appears).  @a aWork must not touch the GUI or
 * the design and should return soon after the cancel flag it is given becomes true.
 *
 * @return false if the user cancelled.
 */
bool CopilotRunInBackground( const std::function<void( const std::atomic<bool>& aCancel )>& aWork );


/**
 * Run an external program without a shell, capturing its output.  The GUI stays painted
 * (but not interactive) while it runs and the user can cancel it from a progress dialog.
 * @a aArgv[0] is the program.
 */
COPILOT_PROCESS_RESULT CopilotRunProcess( const std::vector<wxString>& aArgv,
                                          const wxString& aWorkingDir, int aTimeoutSecs,
                                          const std::map<wxString, wxString>& aExtraEnv = {} );

/// Run a command line through the platform shell (cmd.exe /c or /bin/sh -c).
COPILOT_PROCESS_RESULT CopilotRunShell( const wxString& aCommand, const wxString& aWorkingDir,
                                        int aTimeoutSecs );

/// Look for an executable on PATH (adds .exe on Windows).  Empty if not found.
wxString CopilotFindOnPath( const wxString& aName );

/// Path to kicad-cli next to the running KiCad binaries (empty if not found).
wxString CopilotKicadCliPath();

/// A per-user directory for Copilot downloads (tools such as Freerouting).
wxString CopilotDataDir();

/// A Python interpreter suitable for running helper packages (empty if none).
wxString CopilotPythonPath();

/**
 * HTTP GET into memory (runs in the background; cancellable).  Returns false with @a aError
 * on failure.
 *
 * @param aCrossHostRedirects false for requests the user approved per web host: redirects are
 *                            then only followed within the same host.
 */
bool CopilotHttpGet( const std::string& aUrl, std::string& aBody, std::string& aContentType,
                     std::string& aError, long* aStatus = nullptr, int aTimeoutSecs = 45,
                     bool aCrossHostRedirects = true );

/// Download to a file.
bool CopilotHttpDownload( const std::string& aUrl, const wxString& aPath, std::string& aError );

/// Strip HTML to readable text (scripts, styles and tags removed, entities decoded).
std::string CopilotHtmlToText( const std::string& aHtml );

/// True for paths that would reach another computer (UNC shares, URLs),
/// which the Copilot must not open on the model's say-so (Windows would send the user's
/// credentials to that server).
bool CopilotIsRemotePath( const wxString& aPath );

enum class LIBRARY_TABLE_TYPE;
class KIWAY;
class LIBRARY_MANAGER_ADAPTER;

/**
 * Make sure the adapter's libraries are loaded.  KiCad loads libraries in the background after
 * an editor opens; library queries only see libraries that have finished loading.
 */
void CopilotWaitForLibraries( LIBRARY_MANAGER_ADAPTER* aAdapter );

/**
 * Add (or update) a row in the current project's symbol or footprint library table and load
 * it, so editors can use it immediately.  @a aUri may use ${KIPRJMOD}.  If @a aKiway is given,
 * open library browsers are told to refresh.
 */
bool CopilotRegisterProjectLibrary( LIBRARY_TABLE_TYPE aType, const wxString& aNickname,
                                    const wxString& aUri, wxString& aError,
                                    KIWAY* aKiway = nullptr );


namespace COPILOT_COMMON_TOOLS
{
    enum
    {
        WEB = 1,        ///< fetch_url, read_pdf_url
        SHELL = 2       ///< run_command (always requires approval)
    };

    /// Register the shared tools selected by @a aWhich on a toolbox.
    void Register( const std::function<void( const std::string&, const std::string&,
                                             const nlohmann::json&, const std::vector<std::string>&,
                                             int, COPILOT_REGISTRY_TOOLBOX::HANDLER )>& aAdd,
                   int aWhich, const std::function<wxString()>& aDefaultWorkingDir );
}
