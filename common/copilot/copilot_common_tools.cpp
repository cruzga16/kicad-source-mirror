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

#include <copilot/copilot_common_tools.h>

#include <atomic>
#include <cctype>
#include <memory>
#include <thread>

#include <wx/app.h>
#include <wx/progdlg.h>
#include <wx/translation.h>
#include <wx/base64.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/process.h>
#include <wx/stream.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>
#include <wx/wfstream.h>

#include <build_version.h>
#include <gestfich.h>
#include <libraries/library_manager.h>
#include <libraries/library_table.h>
#include <kicad_curl/kicad_curl_easy.h>
#include <kiway.h>
#include <mail_type.h>
#include <paths.h>
#include <pgm_base.h>
#include <settings/common_settings.h>

#include <curl/curl.h>


using json = nlohmann::json;


namespace
{

class CAPTURE_PROCESS : public wxProcess
{
public:
    CAPTURE_PROCESS() : wxProcess( wxPROCESS_REDIRECT ) {}

    void OnTerminate( int aPid, int aStatus ) override
    {
        m_status = aStatus;
        m_done = true;
    }

    void Drain( std::string& aOut )
    {
        while( IsInputAvailable() )
            drainStream( GetInputStream(), aOut );

        while( IsErrorAvailable() )
            drainStream( GetErrorStream(), aOut );
    }

    std::atomic<bool> m_done{ false };
    int               m_status = -1;

private:
    static void drainStream( wxInputStream* aStream, std::string& aOut )
    {
        if( !aStream )
            return;

        // Called only when data is available, so a single bounded read cannot block.
        char buf[4096];
        aStream->Read( buf, sizeof( buf ) );
        size_t n = aStream->LastRead();

        if( n == 0 )
            return;

        aOut.append( buf, n );

        // Keep memory bounded; the tail is what matters for errors.
        if( aOut.size() > 400000 )
            aOut.erase( 0, aOut.size() - 200000 );
    }
};


wxString s_activity;     // see COPILOT_ACTIVITY


/**
 * Keeps the GUI painted while the Copilot waits for something.  After a second a progress
 * dialog (application modal, with Cancel) appears so the user can see what is happening.
 */
class COPILOT_WAIT_UI
{
public:
    COPILOT_WAIT_UI() : m_start( wxGetLocalTimeMillis() ) {}

    ~COPILOT_WAIT_UI()
    {
        delete m_dialog;
    }

    /// Process pending events.  Returns false once the user has pressed Cancel.
    bool Pump()
    {
        if( !wxTheApp )
            return !m_cancelled;

        if( !m_dialog && ( wxGetLocalTimeMillis() - m_start ).ToLong() > 1000 )
        {
            wxString text = COPILOT_ACTIVITY::Current();

            if( text.IsEmpty() )
                text = _( "The Copilot is working..." );

            m_dialog = new wxProgressDialog( _( "KiCad Copilot" ), text, 100, nullptr,
                                             wxPD_APP_MODAL | wxPD_CAN_ABORT
                                                     | wxPD_ELAPSED_TIME );
        }

        if( m_dialog )
        {
            if( !m_dialog->Pulse() )
                m_cancelled = true;

            // Keep the dialog (and its Cancel button) usable; everything else stays disabled.
            wxSafeYield( m_dialog, true );
        }
        else
        {
            wxSafeYield( nullptr, true );
        }

        return !m_cancelled;
    }

private:
    wxLongLong        m_start;
    wxProgressDialog* m_dialog = nullptr;
    bool              m_cancelled = false;
};


std::string tail( const std::string& aText, size_t aMax )
{
    if( aText.size() <= aMax )
        return aText;

    return "...[" + std::to_string( aText.size() - aMax ) + " bytes omitted]...\n"
           + aText.substr( aText.size() - aMax );
}

} // namespace


COPILOT_ACTIVITY::COPILOT_ACTIVITY( const wxString& aText ) :
        m_previous( s_activity )
{
    s_activity = aText;
}


COPILOT_ACTIVITY::~COPILOT_ACTIVITY()
{
    s_activity = m_previous;
}


wxString COPILOT_ACTIVITY::Current()
{
    return s_activity;
}


bool CopilotRunInBackground( const std::function<void( const std::atomic<bool>& aCancel )>& aWork )
{
    auto cancel = std::make_shared<std::atomic<bool>>( false );
    auto done = std::make_shared<std::atomic<bool>>( false );

    std::thread worker(
            [&aWork, cancel, done]()
            {
                try
                {
                    aWork( *cancel );
                }
                catch( ... )
                {
                }

                done->store( true );
            } );

    COPILOT_WAIT_UI ui;

    while( !done->load() )
    {
        wxMilliSleep( 20 );

        if( !ui.Pump() )
            cancel->store( true );
    }

    worker.join();
    return !cancel->load();
}


namespace
{

/// Start a process with @a aLaunch and wait for it, pumping the GUI (see CopilotRunProcess()).
COPILOT_PROCESS_RESULT runAndWait( const std::function<long( wxProcess*, wxExecuteEnv* )>& aLaunch,
                                   const wxString& aWorkingDir, int aTimeoutSecs,
                                   const std::map<wxString, wxString>& aExtraEnv )
{
    COPILOT_PROCESS_RESULT result;

    wxExecuteEnv env;
    wxGetEnvMap( &env.env );
    env.cwd = aWorkingDir;

    for( const auto& [key, value] : aExtraEnv )
        env.env[key] = value;

    // Owned by us; wx does not delete a process object whose OnTerminate we override
    // unless Detach() is called.
    std::unique_ptr<CAPTURE_PROCESS> process = std::make_unique<CAPTURE_PROCESS>();

    long pid = aLaunch( process.get(), &env );

    if( pid <= 0 )
        return result;

    result.started = true;

    wxLongLong      start = wxGetLocalTimeMillis();
    COPILOT_WAIT_UI ui;

    while( !process->m_done )
    {
        process->Drain( result.output );

        bool timeUp = aTimeoutSecs > 0
                      && ( wxGetLocalTimeMillis() - start ).ToLong() > aTimeoutSecs * 1000L;

        if( timeUp || result.cancelled )
        {
            wxProcess::Kill( pid, wxSIGKILL, wxKILL_CHILDREN );
            result.timedOut = timeUp;

            // Give the event loop a moment to deliver the termination.
            for( int i = 0; i < 100 && !process->m_done; ++i )
            {
                wxMilliSleep( 20 );

                if( wxTheApp )
                    wxTheApp->Yield( true );
            }

            break;
        }

        wxMilliSleep( 25 );

        // Keep the UI painted (and let wx deliver OnTerminate) without allowing user input
        // other than the progress dialog's Cancel button.
        if( !ui.Pump() )
            result.cancelled = true;
    }

    process->Drain( result.output );
    result.exitCode = ( result.timedOut || result.cancelled ) ? -1 : process->m_status;

    if( result.cancelled )
        result.output += "\n[Cancelled by the user.]";

    if( !process->m_done )
        process.release()->Detach();     // Still running somehow: let wx clean it up later.

    return result;
}

const int EXEC_FLAGS = wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE | wxEXEC_MAKE_GROUP_LEADER;

} // namespace


COPILOT_PROCESS_RESULT CopilotRunProcess( const std::vector<wxString>& aArgv,
                                          const wxString& aWorkingDir, int aTimeoutSecs,
                                          const std::map<wxString, wxString>& aExtraEnv )
{
    if( aArgv.empty() )
        return COPILOT_PROCESS_RESULT();

    std::vector<const wchar_t*> argv;

    for( const wxString& a : aArgv )
        argv.push_back( a.wc_str() );

    argv.push_back( nullptr );

    return runAndWait(
            [&]( wxProcess* aProcess, wxExecuteEnv* aEnv )
            {
                return wxExecute( argv.data(), EXEC_FLAGS, aProcess, aEnv );
            },
            aWorkingDir, aTimeoutSecs, aExtraEnv );
}


COPILOT_PROCESS_RESULT CopilotRunShell( const wxString& aCommand, const wxString& aWorkingDir,
                                        int aTimeoutSecs )
{
#ifdef __WXMSW__
    // Hand cmd.exe the command line verbatim.  Going through an argv would make wx quote it
    // with backslash escapes, which cmd.exe does not understand.  /s strips exactly the outer
    // quotes, so the user's own quoting survives.
    wxString cmdLine = wxS( "cmd.exe /d /s /c \"" ) + aCommand + wxS( "\"" );

    return runAndWait(
            [&]( wxProcess* aProcess, wxExecuteEnv* aEnv )
            {
                return wxExecute( cmdLine, EXEC_FLAGS, aProcess, aEnv );
            },
            aWorkingDir, aTimeoutSecs, {} );
#else
    return CopilotRunProcess( { wxS( "/bin/sh" ), wxS( "-c" ), aCommand }, aWorkingDir,
                              aTimeoutSecs );
#endif
}


wxString CopilotFindOnPath( const wxString& aName )
{
    wxString path;

    if( !wxGetEnv( wxS( "PATH" ), &path ) )
        return wxEmptyString;

#ifdef __WXMSW__
    const wxString sep = wxS( ";" );
    std::vector<wxString> names = { aName + wxS( ".exe" ), aName + wxS( ".bat" ), aName };
#else
    const wxString sep = wxS( ":" );
    std::vector<wxString> names = { aName };
#endif

    wxStringTokenizer tok( path, sep );

    while( tok.HasMoreTokens() )
    {
        wxString dir = tok.GetNextToken();

        for( const wxString& n : names )
        {
            wxFileName fn( dir, n );

            if( fn.FileExists() )
                return fn.GetFullPath();
        }
    }

    return wxEmptyString;
}


wxString CopilotKicadCliPath()
{
#ifdef __WXMSW__
    wxString name = wxS( "kicad-cli.exe" );
#else
    wxString name = wxS( "kicad-cli" );
#endif

    wxString path = FindKicadFile( name );

    if( wxFileExists( path ) )
        return path;

    // Running from a build tree: kicad-cli lives in <build>/kicad/.
    wxFileName fn( Pgm().GetExecutablePath(), wxEmptyString );
    fn.RemoveLastDir();
    fn.AppendDir( wxS( "kicad" ) );
    fn.SetFullName( name );

    if( fn.FileExists() )
        return fn.GetFullPath();

    return CopilotFindOnPath( wxS( "kicad-cli" ) );
}


wxString CopilotDataDir()
{
    wxFileName fn( PATHS::GetUserCachePath(), wxEmptyString );
    fn.AppendDir( wxS( "copilot" ) );

    if( !fn.DirExists() )
        fn.Mkdir( wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );

    return fn.GetPath();
}


wxString CopilotPythonPath()
{
    if( COMMON_SETTINGS* common = Pgm().GetCommonSettings() )
    {
        if( !common->m_Api.python_interpreter.IsEmpty()
            && wxFileExists( common->m_Api.python_interpreter ) )
        {
            return common->m_Api.python_interpreter;
        }
    }

#ifdef __WXMSW__
    // KiCad's Windows installer ships a Python next to its binaries.
    wxFileName bundled( Pgm().GetExecutablePath(), wxS( "python.exe" ) );

    if( bundled.FileExists() )
        return bundled.GetFullPath();

    for( wxString name : { wxS( "python" ), wxS( "py" ) } )
    {
        wxString p = CopilotFindOnPath( name );

        if( !p.IsEmpty() )
            return p;
    }
#else
    for( wxString name : { wxS( "python3" ), wxS( "python" ) } )
    {
        wxString p = CopilotFindOnPath( name );

        if( !p.IsEmpty() )
            return p;
    }
#endif

    return wxEmptyString;
}


bool CopilotHttpGet( const std::string& aUrl, std::string& aBody, std::string& aContentType,
                     std::string& aError, long* aStatus, int aTimeoutSecs,
                     bool aCrossHostRedirects )
{
    if( aUrl.rfind( "http://", 0 ) != 0 && aUrl.rfind( "https://", 0 ) != 0 )
    {
        aError = "Only http:// and https:// URLs are supported.";
        return false;
    }

    std::string userAgent = "Mozilla/5.0 (compatible; KiCad-Copilot/"
                            + std::string( GetBuildVersion().utf8_str() ) + ")";
    std::string body, type, error;
    long        status = 0;
    bool        ok = false;

    bool finished = CopilotRunInBackground(
            [&]( const std::atomic<bool>& aCancel )
            {
                const std::string host = CopilotUrlHost( aUrl );
                std::string       url = aUrl;

                // Redirects are followed by hand when they must stay on the approved host.
                for( int hop = 0; hop < 8; ++hop )
                {
                    KICAD_CURL_EASY curl;

                    if( !curl.SetURL( url ) )
                    {
                        error = "Invalid URL.";
                        return;
                    }

                    // Some sites refuse requests without a browser-like agent.
                    curl.SetUserAgent( userAgent );
                    curl.SetFollowRedirects( aCrossHostRedirects );
                    curl.SetConnectTimeout( 20 );
                    curl.SetTimeout( aTimeoutSecs );
                    curl.SetHeader( "Accept", "text/html,application/xhtml+xml,application/json,"
                                              "application/pdf,*/*" );
                    curl.SetTransferCallback(
                            [&aCancel]( size_t, size_t, size_t, size_t ) -> int
                            {
                                return aCancel.load() ? 1 : 0;
                            },
                            0 );

                    int rc = curl.Perform();

                    if( rc != CURLE_OK )
                    {
                        error = "Could not fetch " + url + ": " + curl.GetErrorText( rc );
                        return;
                    }

                    status = curl.GetResponseStatusCode();

                    if( !aCrossHostRedirects && status >= 300 && status < 400 )
                    {
                        char* location = nullptr;
                        curl_easy_getinfo( curl.GetCurl(), CURLINFO_REDIRECT_URL, &location );

                        if( !location || !*location )
                        {
                            error = "HTTP " + std::to_string( status ) + " without a target from "
                                    + url;
                            return;
                        }

                        std::string next = location;

                        if( CopilotUrlHost( next ) != host )
                        {
                            error = "The address redirects to another site: " + next
                                    + " . Call the tool again with that address (the user "
                                      "approves each site).";
                            return;
                        }

                        url = next;
                        continue;
                    }

                    char* ct = nullptr;
                    curl_easy_getinfo( curl.GetCurl(), CURLINFO_CONTENT_TYPE, &ct );
                    type = ct ? ct : "";
                    body = curl.GetBuffer();
                    ok = true;
                    return;
                }

                error = "Too many redirects from " + aUrl;
            } );

    if( !finished )
    {
        aError = "Cancelled by the user.";
        return false;
    }

    if( !ok )
    {
        aError = error;
        return false;
    }

    if( aStatus )
        *aStatus = status;

    aContentType = type;
    aBody = std::move( body );

    if( status >= 400 )
    {
        aError = "HTTP " + std::to_string( status ) + " from " + aUrl;
        return false;
    }

    return true;
}


bool CopilotHttpDownload( const std::string& aUrl, const wxString& aPath, std::string& aError )
{
    std::string body, type;

    if( !CopilotHttpGet( aUrl, body, type, aError, nullptr, 600 ) )
        return false;

    // Write next to the target and rename, so a failed write never leaves a truncated file
    // where a working one is expected.
    wxString partial = aPath + wxS( ".part" );

    {
        wxFFileOutputStream out( partial );

        if( !out.IsOk() )
        {
            aError = "Cannot write " + std::string( partial.utf8_str() );
            return false;
        }

        out.Write( body.data(), body.size() );
        bool wroteAll = out.LastWrite() == body.size();

        if( !out.Close() || !wroteAll )
        {
            wxRemoveFile( partial );
            aError = "Writing " + std::string( aPath.utf8_str() ) + " failed (disk full?).";
            return false;
        }
    }

    if( !wxRenameFile( partial, aPath, true ) )
    {
        wxRemoveFile( partial );
        aError = "Cannot replace " + std::string( aPath.utf8_str() );
        return false;
    }

    return true;
}


std::string CopilotHtmlToText( const std::string& aHtml )
{
    std::string out;
    out.reserve( aHtml.size() / 3 );

    auto lower = [&]( size_t aPos, size_t aLen )
    {
        std::string s = aHtml.substr( aPos, aLen );

        for( char& c : s )
            c = (char) std::tolower( (unsigned char) c );

        return s;
    };

    size_t i = 0;

    while( i < aHtml.size() )
    {
        char c = aHtml[i];

        if( c == '<' )
        {
            // Skip script/style blocks entirely.
            for( const char* tag : { "script", "style", "noscript", "svg" } )
            {
                std::string open = std::string( "<" ) + tag;

                if( lower( i, open.size() ) == open )
                {
                    std::string close = std::string( "</" ) + tag;
                    size_t      end = i;

                    while( end < aHtml.size() && lower( end, close.size() ) != close )
                        ++end;

                    i = end;
                    break;
                }
            }

            size_t end = aHtml.find( '>', i );

            if( end == std::string::npos )
                break;

            std::string tag = lower( i + 1, std::min<size_t>( end - i - 1, 8 ) );

            // Block-level tags become line breaks.
            for( const char* b : { "p", "/p", "br", "div", "/div", "li", "tr", "/tr", "h1", "h2",
                                   "h3", "h4", "/h", "table", "/table" } )
            {
                std::string bs = b;

                if( tag.rfind( bs, 0 ) == 0
                    && ( tag.size() == bs.size() || !std::isalpha( (unsigned char) tag[bs.size()] ) ) )
                {
                    out += '\n';
                    break;
                }
            }

            if( tag.rfind( "td", 0 ) == 0 || tag.rfind( "th", 0 ) == 0 )
                out += " | ";

            i = end + 1;
            continue;
        }

        if( c == '&' )
        {
            size_t semi = aHtml.find( ';', i );

            if( semi != std::string::npos && semi - i < 10 )
            {
                std::string ent = aHtml.substr( i + 1, semi - i - 1 );

                if( ent == "amp" ) out += '&';
                else if( ent == "lt" ) out += '<';
                else if( ent == "gt" ) out += '>';
                else if( ent == "quot" ) out += '"';
                else if( ent == "#39" || ent == "apos" ) out += '\'';
                else if( ent == "nbsp" ) out += ' ';
                else if( ent == "micro" || ent == "#181" ) out += "\xC2\xB5";
                else if( ent == "Omega" || ent == "ohm" ) out += "\xCE\xA9";
                else if( ent == "deg" ) out += "\xC2\xB0";
                else if( ent == "plusmn" ) out += "\xC2\xB1";
                else out += ' ';

                i = semi + 1;
                continue;
            }
        }

        out += c;
        ++i;
    }

    // Collapse whitespace runs and blank lines.
    std::string collapsed;
    collapsed.reserve( out.size() );
    int  newlines = 0;
    bool space = false;

    for( char ch : out )
    {
        if( ch == '\r' || ch == '\t' )
            ch = ' ';

        if( ch == '\n' )
        {
            if( ++newlines <= 2 )
                collapsed += '\n';

            space = false;
            continue;
        }

        if( ch == ' ' )
        {
            if( !space && newlines == 0 )
                collapsed += ' ';

            space = true;
            continue;
        }

        newlines = 0;
        space = false;
        collapsed += ch;
    }

    return collapsed;
}


bool CopilotIsRemotePath( const wxString& aPath )
{
    wxString p = aPath;
    p.Trim( false );

    // \\server\share, //server/share, \\?\UNC\..., and URL-style locations.
    if( p.StartsWith( wxS( "\\\\" ) ) || p.StartsWith( wxS( "//" ) ) )
        return true;

    wxString lower = p.Lower();

    for( const wxString& scheme : { wxS( "smb:" ), wxS( "file:" ), wxS( "http:" ),
                                    wxS( "https:" ), wxS( "ftp:" ) } )
    {
        if( lower.StartsWith( scheme ) )
            return true;
    }

    return false;
}


void CopilotWaitForLibraries( LIBRARY_MANAGER_ADAPTER* aAdapter )
{
    if( !aAdapter )
        return;

    // Starts loading whatever is not loaded yet (a no-op if a load is already running).
    aAdapter->AsyncLoad();

    if( !aAdapter->AsyncLoadProgress() )
        return;

    COPILOT_ACTIVITY activity( _( "Loading libraries..." ) );

    CopilotRunInBackground(
            [aAdapter]( const std::atomic<bool>& )
            {
                aAdapter->BlockUntilLoaded();
            } );
}


bool CopilotRegisterProjectLibrary( LIBRARY_TABLE_TYPE aType, const wxString& aNickname,
                                    const wxString& aUri, wxString& aError, KIWAY* aKiway )
{
    LIBRARY_MANAGER&              manager = Pgm().GetLibraryManager();
    std::optional<LIBRARY_TABLE*> tableOpt = manager.Table( aType, LIBRARY_TABLE_SCOPE::PROJECT );

    if( !tableOpt || !*tableOpt )
    {
        aError = "The project library table is not available (is a project open?).";
        return false;
    }

    LIBRARY_TABLE* table = *tableOpt;
    bool           changed = false;

    // Never repoint or shadow a library the user already has under this name (a project row
    // would also hide a global library of the same name).
    if( std::optional<LIBRARY_TABLE_ROW*> existing =
                manager.GetRow( aType, aNickname, LIBRARY_TABLE_SCOPE::BOTH );
        existing && *existing && ( *existing )->URI() != aUri )
    {
        aError = wxString::Format( wxS( "A library named '%s' already exists (%s); use a "
                                        "different name." ),
                                   aNickname, ( *existing )->URI() );
        return false;
    }

    if( table->HasRow( aNickname ) )
    {
        if( std::optional<LIBRARY_TABLE_ROW*> row = table->Row( aNickname ); row && *row )
        {
            if( ( *row )->URI() != aUri )
            {
                ( *row )->SetURI( aUri );
                changed = true;
            }
        }
    }
    else
    {
        LIBRARY_TABLE_ROW& row = table->InsertRow();
        row.SetNickname( aNickname );
        row.SetURI( aUri );
        row.SetType( wxS( "KiCad" ) );
        row.SetOptions( wxString() );
        row.SetDescription( wxS( "Added by KiCad Copilot" ) );
        row.SetOk( true );
        changed = true;
    }

    if( changed )
    {
        bool saved = true;

        table->Save().map_error(
                [&]( const LIBRARY_ERROR& aErr )
                {
                    aError = aErr.message;
                    saved = false;
                } );

        if( !saved )
            return false;

        // Rebuild the project table from disk (this invalidates cached project libraries, the
        // same way the library table dialog does) ...
        manager.ReloadTables( LIBRARY_TABLE_SCOPE::PROJECT, { aType } );
    }
    else
    {
        // ... or, if only the library contents changed, reopen that one library.
        manager.ReloadLibraryEntry( aType, aNickname, LIBRARY_TABLE_SCOPE::PROJECT );
    }

    // Load it now so searches and placement see its contents immediately.  (Adapters only
    // exist once the matching editor has been started; they load libraries on demand.)
    manager.LoadLibraryEntry( aType, aNickname );

    if( aKiway && aType == LIBRARY_TABLE_TYPE::FOOTPRINT )
    {
        std::string payload;
        aKiway->ExpressMail( FRAME_FOOTPRINT_EDITOR, MAIL_RELOAD_LIB, payload );
        aKiway->ExpressMail( FRAME_FOOTPRINT_VIEWER, MAIL_RELOAD_LIB, payload );
        aKiway->ExpressMail( FRAME_CVPCB, MAIL_RELOAD_LIB, payload );
    }

    return true;
}


namespace COPILOT_COMMON_TOOLS
{

void Register( const std::function<void( const std::string&, const std::string&, const json&,
                                         const std::vector<std::string>&, int,
                                         COPILOT_REGISTRY_TOOLBOX::HANDLER )>& aAdd,
               int aWhich, const std::function<wxString()>& aDefaultWorkingDir )
{
    using namespace COPILOT_ARGS;

    if( aWhich & WEB )
    {
        aAdd( "fetch_url",
              "Fetch a web page or JSON API and return its readable text (HTML is converted to "
              "text). Use for part pages, application notes and manufacturer capability pages. "
              "Use read_pdf_url for PDF datasheets.",
              { { "url", String( "http(s) URL." ) },
                { "max_chars", Integer( "Maximum characters to return (default 20000)." ) } },
              { "url" }, COPILOT_REGISTRY_TOOLBOX::APPROVAL,
              []( const json& aIn ) -> COPILOT_TOOL_RESULT
              {
                  std::string url = Str( aIn, "url" );
                  int         maxChars = Int( aIn, "max_chars", 20000, 500, 100000 );
                  std::string body, type, error;
                  long        status = 0;

                  wxBusyCursor     busy;
                  COPILOT_ACTIVITY activity( wxString::Format( _( "Fetching %s..." ),
                                                               wxString::FromUTF8( url ) ) );

                  if( !CopilotHttpGet( url, body, type, error, &status, 45, false ) )
                      return COPILOT_TOOL_RESULT::Error( error );

                  if( type.find( "pdf" ) != std::string::npos )
                  {
                      return COPILOT_TOOL_RESULT::Error( "This URL is a PDF; use read_pdf_url "
                                                         "instead." );
                  }

                  std::string text = type.find( "html" ) != std::string::npos
                                             ? CopilotHtmlToText( body )
                                             : body;
                  bool truncated = (int) text.size() > maxChars;

                  if( truncated )
                      text = CopilotTruncateUtf8( text, maxChars );

                  json j = { { "url", url }, { "content_type", type }, { "text", text } };

                  if( truncated )
                      j["truncated"] = true;

                  if( text.size() < 400 && type.find( "html" ) != std::string::npos )
                  {
                      j["note"] = "Very little text; the page is probably rendered with "
                                  "JavaScript. Try web search or another source.";
                  }

                  return COPILOT_TOOL_RESULT::Ok( j, std::to_string( text.size() ) + " chars" );
              } );

        aAdd( "read_pdf_url",
              "Download a PDF (for example a component datasheet) and attach it so you can read "
              "it: pinouts, recommended land patterns, typical application circuits. Only "
              "Claude (Anthropic) models can read the attachment. Limits: 18 MB and about 100 "
              "pages.",
              { { "url", String( "URL of the PDF." ) } },
              { "url" }, COPILOT_REGISTRY_TOOLBOX::APPROVAL,
              []( const json& aIn ) -> COPILOT_TOOL_RESULT
              {
                  std::string url = Str( aIn, "url" );
                  std::string body, type, error;

                  wxBusyCursor     busy;
                  COPILOT_ACTIVITY activity( wxString::Format( _( "Downloading %s..." ),
                                                               wxString::FromUTF8( url ) ) );

                  if( !CopilotHttpGet( url, body, type, error, nullptr, 90, false ) )
                      return COPILOT_TOOL_RESULT::Error( error );

                  if( body.rfind( "%PDF", 0 ) != 0 )
                  {
                      return COPILOT_TOOL_RESULT::Error( "The URL did not return a PDF (got '"
                                                         + type + "'). It may be a landing "
                                                         "page; look for a direct PDF link." );
                  }

                  // The request limit applies to the base64 encoded document (4/3 larger).
                  if( body.size() > 18 * 1024 * 1024 )
                  {
                      return COPILOT_TOOL_RESULT::Error( "The PDF is larger than 18 MB, too "
                                                         "large to attach. Look for a shorter "
                                                         "document (e.g. a product brief) or use "
                                                         "fetch_url on an HTML page." );
                  }

                  // Rough page count ("/Type /Page" objects, not "/Pages").  Compressed object
                  // streams hide pages, so this can only undercount.
                  int pages = 0;

                  for( size_t pos = body.find( "/Type" ); pos != std::string::npos;
                       pos = body.find( "/Type", pos + 5 ) )
                  {
                      size_t p = pos + 5;

                      while( p < body.size() && ( body[p] == ' ' || body[p] == '\r'
                                                  || body[p] == '\n' ) )
                          ++p;

                      if( body.compare( p, 5, "/Page" ) == 0
                          && ( p + 5 >= body.size() || body[p + 5] != 's' ) )
                      {
                          ++pages;
                      }
                  }

                  if( pages > 100 )
                  {
                      return COPILOT_TOOL_RESULT::Error( "The PDF has about "
                                                         + std::to_string( pages )
                                                         + " pages; at most 100 can be attached. "
                                                         "Look for a shorter document." );
                  }

                  COPILOT_TOOL_RESULT r = COPILOT_TOOL_RESULT::Ok(
                          { { "url", url }, { "bytes", body.size() },
                            { "pages_estimate", pages },
                            { "note", "The PDF is attached to this result." } },
                          std::to_string( body.size() / 1024 ) + " kB PDF" );

                  wxString b64 = wxBase64Encode( body.data(), body.size() );
                  r.attachments.push_back( { "application/pdf", std::string( b64.ToAscii() ), url } );
                  return r;
              } );
    }

    if( aWhich & SHELL )
    {
        aAdd( "run_command",
              "Run a command line on the user's computer (after they approve it) and return its "
              "output. Use for things no other tool covers, e.g. installing a helper package. "
              "Never use it to delete user files.",
              { { "command", String( "Command line to run through the system shell." ) },
                { "working_dir", String( "Working directory (default: the project folder)." ) },
                { "timeout_s", Integer( "Timeout in seconds (default 300)." ) } },
              { "command" }, COPILOT_REGISTRY_TOOLBOX::ASK_EVERY_TIME,
              [aDefaultWorkingDir]( const json& aIn ) -> COPILOT_TOOL_RESULT
              {
                  wxString cmd = wxString::FromUTF8( Str( aIn, "command" ) );
                  wxString cwd = wxString::FromUTF8( Str( aIn, "working_dir" ) );

                  if( cwd.IsEmpty() && aDefaultWorkingDir )
                      cwd = aDefaultWorkingDir();

                  if( cwd.IsEmpty() || !wxDirExists( cwd ) )
                      cwd = wxGetHomeDir();

                  wxBusyCursor           busy;
                  COPILOT_ACTIVITY       activity( wxString::Format( _( "Running: %s" ), cmd ) );
                  COPILOT_PROCESS_RESULT res = CopilotRunShell( cmd, cwd,
                                                                Int( aIn, "timeout_s", 300, 5, 3600 ) );

                  if( !res.started )
                      return COPILOT_TOOL_RESULT::Error( "The command could not be started." );

                  json j = { { "exit_code", res.exitCode },
                             { "timed_out", res.timedOut },
                             { "cancelled", res.cancelled },
                             { "output", tail( res.output, 12000 ) } };

                  COPILOT_TOOL_RESULT r = COPILOT_TOOL_RESULT::Ok(
                          j, res.cancelled ? std::string( "cancelled" )
                                           : "exit " + std::to_string( res.exitCode ) );
                  r.isError = res.timedOut || res.cancelled || res.exitCode != 0;
                  return r;
              } );
    }
}

} // namespace COPILOT_COMMON_TOOLS
