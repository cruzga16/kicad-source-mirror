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

#include <copilot/kicad_copilot_toolbox.h>

#include <algorithm>
#include <regex>
#include <set>
#include <sstream>

#include <wx/app.h>
#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/utils.h>
#include <wx/wfstream.h>
#include <wx/txtstrm.h>
#include <wx/zipstrm.h>

#include <build_version.h>
#include <copilot/copilot_common_tools.h>
#include <kicad_curl/kicad_curl_easy.h>
#include <kicad_manager_frame.h>
#include <kiway.h>
#include <kiway_mail.h>
#include <kiway_player.h>
#include <libraries/library_manager.h>
#include <libraries/library_table.h>
#include <mail_type.h>
#include <paths.h>
#include <pgm_base.h>
#include <project.h>
#include <tool/tool_manager.h>
#include <tools/kicad_manager_actions.h>
#include <wildcards_and_files_ext.h>

#include <curl/curl.h>


using json = nlohmann::json;
using namespace COPILOT_ARGS;


namespace
{

std::string S( const wxString& aStr )
{
    return std::string( aStr.utf8_str() );
}


wxString W( const std::string& aStr )
{
    return wxString::FromUTF8( aStr );
}


std::string tail( const std::string& aText, size_t aMax )
{
    return aText.size() <= aMax ? aText : "..." + aText.substr( aText.size() - aMax );
}


/// Split a CSV line (KiCad writes simple quoted CSV).
std::vector<std::string> splitCsv( const std::string& aLine )
{
    std::vector<std::string> out;
    std::string              cur;
    bool                     quoted = false;

    for( size_t i = 0; i < aLine.size(); ++i )
    {
        char c = aLine[i];

        if( c == '"' )
        {
            if( quoted && i + 1 < aLine.size() && aLine[i + 1] == '"' )
            {
                cur += '"';
                ++i;
            }
            else
            {
                quoted = !quoted;
            }
        }
        else if( c == ',' && !quoted )
        {
            out.push_back( cur );
            cur.clear();
        }
        else if( c != '\r' )
        {
            cur += c;
        }
    }

    out.push_back( cur );
    return out;
}


std::string csvField( const std::string& aText )
{
    std::string out = "\"";

    for( char c : aText )
        out += ( c == '"' ) ? std::string( "\"\"" ) : std::string( 1, c );

    return out + "\"";
}


bool readFile( const wxString& aPath, std::string& aOut )
{
    wxFFileInputStream in( aPath );

    if( !in.IsOk() )
        return false;

    aOut.resize( in.GetLength() );
    in.Read( aOut.data(), aOut.size() );
    aOut.resize( in.LastRead() );
    return true;
}


bool writeFile( const wxString& aPath, const std::string& aText )
{
    wxFFileOutputStream out( aPath );

    if( !out.IsOk() )
        return false;

    out.Write( aText.data(), aText.size() );
    return out.Close();
}


bool zipFiles( const wxString& aZipPath, const std::vector<wxString>& aFiles )
{
    wxFFileOutputStream file( aZipPath );

    if( !file.IsOk() )
        return false;

    wxZipOutputStream zip( file, 9 );

    for( const wxString& f : aFiles )
    {
        wxFFileInputStream in( f );

        if( !in.IsOk() )
            continue;

        zip.PutNextEntry( wxFileName( f ).GetFullName() );
        in.Read( zip );
    }

    return zip.Close() && file.Close();
}

} // namespace


KICAD_COPILOT_TOOLBOX::KICAD_COPILOT_TOOLBOX( KICAD_MANAGER_FRAME* aFrame ) :
        m_frame( aFrame )
{
    registerTools();
}


bool KICAD_COPILOT_TOOLBOX::projectActive() const
{
    return m_frame && m_frame->IsProjectActive();
}


wxString KICAD_COPILOT_TOOLBOX::projectDir() const
{
    return projectActive() ? m_frame->Prj().GetProjectPath() : wxString();
}


bool KICAD_COPILOT_TOOLBOX::closeProject()
{
    if( !m_frame->CloseProject( true ) )
        return false;

    // Fabrication outputs belong to the project that made them.
    m_lastFabZip.clear();
    m_lastFabHouse.clear();
    m_lastBoardInfo = nlohmann::json();

    // Closed editor windows are only destroyed once the application is idle, and their
    // destructors release the file locks.  Let that happen now, before the next project (or
    // the same one again) opens its editors, or they would find their own stale locks.
    if( wxTheApp )
    {
        wxTheApp->ProcessPendingEvents();
        wxTheApp->ProcessIdle();
    }

    return true;
}


bool KICAD_COPILOT_TOOLBOX::editorOpen( FRAME_T aFrame ) const
{
    return m_frame && m_frame->Kiway().Player( aFrame, false ) != nullptr;
}


bool KICAD_COPILOT_TOOLBOX::openEditor( FRAME_T aFrame )
{
    if( !projectActive() )
        return false;

    if( !editorOpen( aFrame ) || !m_frame->Kiway().Player( aFrame, false )->IsShown() )
    {
        m_frame->GetToolManager()->RunAction( aFrame == FRAME_SCH ? KICAD_MANAGER_ACTIONS::editSchematic
                                                                  : KICAD_MANAGER_ACTIONS::editPCB );
    }

    // Keep the Copilot's window in front so the user can follow the conversation.
    m_frame->Raise();
    return editorOpen( aFrame );
}


COPILOT_TOOL_RESULT KICAD_COPILOT_TOOLBOX::callEditor( FRAME_T aFrame, const std::string& aName,
                                                       const json& aInput )
{
    const char* editorName = aFrame == FRAME_SCH ? "schematic editor" : "PCB editor";

    if( !projectActive() )
        return COPILOT_TOOL_RESULT::Error( "No project is open; use create_project or "
                                           "open_project first." );

    if( !editorOpen( aFrame ) && !openEditor( aFrame ) )
        return COPILOT_TOOL_RESULT::Error( std::string( "Could not open the " ) + editorName + "." );

    std::string payload = MakeCallRequest( aName, aInput, EditsAllowed() );
    std::string request = payload;

    m_frame->Kiway().ExpressMail( aFrame, MAIL_COPILOT, payload );

    if( payload == request )
        return COPILOT_TOOL_RESULT::Error( std::string( "The " ) + editorName + " did not answer." );

    return ParseCallResult( payload );
}


std::vector<COPILOT_TOOL_DEF> KICAD_COPILOT_TOOLBOX::GetTools( bool aAllowEdits )
{
    std::vector<COPILOT_TOOL_DEF> defs = COPILOT_REGISTRY_TOOLBOX::GetTools( aAllowEdits );

    static const std::set<std::string> skip = { "fetch_url", "read_pdf_url", "run_command" };

    for( auto [frame, prefix, label] : { std::tuple{ FRAME_SCH, "sch_", "[Schematic editor] " },
                                         std::tuple{ FRAME_PCB_EDITOR, "pcb_", "[PCB editor] " } } )
    {
        if( !editorOpen( frame ) )
            continue;

        std::string payload = MakeListRequest();
        m_frame->Kiway().ExpressMail( frame, MAIL_COPILOT, payload );

        for( COPILOT_TOOL_DEF def : ParseToolList( payload ) )
        {
            if( skip.count( def.name ) || ( def.modifiesDesign && !aAllowEdits ) )
                continue;

            def.name = prefix + def.name;
            def.description = label + def.description;
            defs.push_back( def );
        }
    }

    return defs;
}


bool KICAD_COPILOT_TOOLBOX::FindTool( const std::string& aName, COPILOT_TOOL_DEF& aDef )
{
    if( COPILOT_REGISTRY_TOOLBOX::FindTool( aName, aDef ) )
        return true;

    FRAME_T frame;

    if( aName.rfind( "sch_", 0 ) == 0 )
        frame = FRAME_SCH;
    else if( aName.rfind( "pcb_", 0 ) == 0 )
        frame = FRAME_PCB_EDITOR;
    else
        return false;

    // The model may call an editor tool it saw earlier in the conversation after the editor
    // was closed; open it again so the call can go through.
    if( !editorOpen( frame ) && !openEditor( frame ) )
        return false;

    for( const COPILOT_TOOL_DEF& def : GetTools( true ) )
    {
        if( def.name == aName )
        {
            aDef = def;
            return true;
        }
    }

    return false;
}


bool KICAD_COPILOT_TOOLBOX::IsReadyForTools( std::string& aWhy )
{
    if( !COPILOT_TOOLBOX::IsReadyForTools( aWhy ) )
        return false;

    // The editors must not be in the middle of an interactive tool either.
    for( FRAME_T frame : { FRAME_SCH, FRAME_PCB_EDITOR } )
    {
        if( !editorOpen( frame ) )
            continue;

        std::string payload = MakeReadyRequest();
        m_frame->Kiway().ExpressMail( frame, MAIL_COPILOT, payload );

        if( !ParseReadyReply( payload, aWhy ) )
            return false;
    }

    return true;
}


COPILOT_TOOL_RESULT KICAD_COPILOT_TOOLBOX::ExecuteTool( const std::string& aName, const json& aInput )
{
    if( aName.rfind( "sch_", 0 ) == 0 )
        return callEditor( FRAME_SCH, aName.substr( 4 ), aInput );

    if( aName.rfind( "pcb_", 0 ) == 0 )
        return callEditor( FRAME_PCB_EDITOR, aName.substr( 4 ), aInput );

    return COPILOT_REGISTRY_TOOLBOX::ExecuteTool( aName, aInput );
}


std::string KICAD_COPILOT_TOOLBOX::DescribeToolCall( const std::string& aName, const json& aInput )
{
    if( aName == "import_lcsc_part" )
    {
        return "Download " + json( StrList( aInput, "lcsc_ids" ) ).dump()
               + " from LCSC/EasyEDA and convert them with the easyeda2kicad Python package "
                 "(installed into a private virtual environment on first use) into the "
                 "project's 'lcsc' libraries.";
    }

    if( aName == "send_to_board_house" )
    {
        std::string house = Str( aInput, "house", m_lastFabHouse );
        std::string zip = m_lastFabZip.empty() ? std::string( "the fabrication files" )
                                               : m_lastFabZip;

        if( house == "pcbway" )
        {
            return "Upload " + zip + " to PCBWay and open the resulting quote page in your "
                   "browser. Nothing is ordered or paid; you review and check out yourself.";
        }

        return "Open " + house + "'s online quote page in your browser and the folder containing "
               + zip + " so you can upload it there. Nothing is ordered or paid; you review "
               "and check out yourself.";
    }

    if( aName == "pcb_autoroute" )
        return "Run the Freerouting autorouter (Java) on the board.";

    if( aName == "pcb_install_freerouting" )
        return "Download the Freerouting autorouter from GitHub into KiCad's cache folder.";

    return COPILOT_REGISTRY_TOOLBOX::DescribeToolCall( aName, aInput );
}


// ---------------------------------------------------------------------------------------------
// Registration and prompts
// ---------------------------------------------------------------------------------------------

void KICAD_COPILOT_TOOLBOX::registerTools()
{
    auto bind = [this]( COPILOT_TOOL_RESULT ( KICAD_COPILOT_TOOLBOX::*aFn )( const json& ) )
    {
        return [this, aFn]( const json& aIn ) { return ( this->*aFn )( aIn ); };
    };

    add( "get_project_info",
         "The open project (if any): folder, files, which editors are open, and the "
         "project-specific libraries.",
         json::object(), {}, READ_ONLY, bind( &KICAD_COPILOT_TOOLBOX::getProjectInfo ) );

    add( "create_project",
         "Create a new KiCad project (project, empty schematic and board) in its own folder, "
         "open it, and open the schematic and PCB editors. After this the sch_* and pcb_* tools "
         "are available.",
         { { "name", String( "Project name (used for the folder and file names)." ) },
           { "directory", String( "Parent folder (default: the KiCad projects folder)." ) } },
         { "name" }, MODIFIES, bind( &KICAD_COPILOT_TOOLBOX::createProject ) );

    add( "open_project",
         "Open an existing .kicad_pro project and its editors.",
         { { "path", String( "Path to the .kicad_pro file or its folder." ) } },
         { "path" }, READ_ONLY, bind( &KICAD_COPILOT_TOOLBOX::openProject ) );

    add( "open_editors",
         "Open (or bring up) the schematic and/or PCB editor for the current project so their "
         "sch_* / pcb_* tools become available.",
         { { "which", Enum( { "both", "schematic", "pcb" }, "Default both." ) } },
         {}, READ_ONLY, bind( &KICAD_COPILOT_TOOLBOX::openEditors ) );

    add( "import_lcsc_part",
         "Import parts that are not in the KiCad libraries from LCSC/EasyEDA by LCSC number "
         "(e.g. C2040): symbol, footprint and 3D model go into the project's 'lcsc' libraries, "
         "which are registered automatically. Returns the lib_ids to use with sch_add_symbol. "
         "Find LCSC numbers with web search (Claude) or from the user.",
         { { "lcsc_ids", StringArray( "LCSC part numbers, e.g. [\"C2040\", \"C82942\"]." ) } },
         { "lcsc_ids" }, MODIFIES | APPROVAL, bind( &KICAD_COPILOT_TOOLBOX::importLcscPart ) );

    add( "export_fabrication",
         "Save the design and generate manufacturing files with kicad-cli: Gerbers + drill "
         "(zipped), and for assembly a BOM and pick-and-place (CPL) file in the board house's "
         "format, plus an IPC-D-356 netlist. Run pcb_run_drc first and fix errors.",
         { { "house", Enum( { "jlcpcb", "pcbway", "generic" }, "Board house format (default "
                                                                 "jlcpcb)." ) },
           { "assembly", Boolean( "Also produce BOM and CPL for assembly (default true)." ) } },
         {}, MODIFIES, bind( &KICAD_COPILOT_TOOLBOX::exportFabrication ) );

    add( "send_to_board_house",
         "Hand the exported files to the board house: PCBWay receives an upload and opens its "
         "quote page; for JLCPCB the quote page and the output folder are opened so the user can "
         "drop the zip in. Never orders or pays. Only call when the user asked to send the "
         "board.",
         { { "house", Enum( { "jlcpcb", "pcbway" }, "Board house (default: the last export)." ) } },
         {}, APPROVAL, bind( &KICAD_COPILOT_TOOLBOX::sendToBoardHouse ) );

    add( "open_folder", "Open a folder (default: the project folder) in the file manager.",
         { { "path", String( "Folder path." ) } },
         {}, READ_ONLY, bind( &KICAD_COPILOT_TOOLBOX::openFolder ) );

    COPILOT_COMMON_TOOLS::Register(
            [this]( const std::string& n, const std::string& d, const json& p,
                    const std::vector<std::string>& r, int f, HANDLER h )
            {
                AddTool( n, d, p, r, f, std::move( h ) );
            },
            COPILOT_COMMON_TOOLS::WEB | COPILOT_COMMON_TOOLS::SHELL,
            [this]() { return projectDir(); } );
}


std::string KICAD_COPILOT_TOOLBOX::GetSystemPrompt()
{
    return R"(You are the project-level KiCad Copilot in the KiCad project manager. You can take a
hardware idea all the way to manufacturing files by driving the schematic editor (sch_* tools)
and the PCB editor (pcb_* tools), which you operate through tools just like a user would.

End-to-end workflow for "make a new project that does X":
1. Plan briefly: key requirements (power source, voltages, interfaces, size, quantity,
   assembled or bare board). If something essential is ambiguous, ask ONE concise question;
   otherwise state your assumptions and proceed.
2. Choose parts: prefer common parts that exist in KiCad's libraries and, if the user wants
   assembly, JLCPCB basic/extended parts with LCSC numbers (add an "LCSC" field). Use web
   search / fetch_url / read_pdf_url for datasheets and part numbers. Import missing parts with
   import_lcsc_part, or create a symbol with sch_create_symbol from the datasheet pinout.
3. create_project (opens both editors).
4. Schematic: sch_add_symbol for each part (value + footprint + MPN/LCSC fields), sch_connect
   every net once with all its pins, sch_no_connect unused pins, sch_add_power_flag on nets fed
   from connectors/batteries, include decoupling capacitors, pull-ups, protection and
   indicators a competent engineer would add. sch_run_erc and fix all errors. sch_save_schematic.
5. PCB: pcb_update_from_schematic (fix any missing-footprint errors in the schematic), then
   pcb_set_board_outline (size to fit, with margin), pcb_set_design_rules for the board house
   (and a Power net class for supply nets), pcb_arrange_footprints, refine with
   pcb_move_footprint (connectors on edges, decoupling next to pins), pcb_autoroute, pcb_add_zone
   GND on both layers, pcb_run_drc, fix, repeat. pcb_save_board.
6. export_fabrication for the chosen house. Report a short summary (parts, board size, layer
   count, DRC status, output folder).
7. Only if the user asked, send_to_board_house. You can never place orders or pay; the user
   finishes checkout in their browser.

Rules: keep going without asking for confirmation between steps unless you hit a real
decision; report progress in short messages; when a tool fails, read the error, fix the cause
and retry; never invent pin numbers - read them with sch_get_symbol or sch_get_library_symbol.
Autorouted boards and generated schematics must be reviewed by the user before ordering: say so.
)";
}


std::string KICAD_COPILOT_TOOLBOX::GetContextSnapshot()
{
    json ctx;

    if( projectActive() )
    {
        ctx["project"] = S( m_frame->Prj().GetProjectFullName() );
        ctx["schematic_editor_open"] = editorOpen( FRAME_SCH );
        ctx["pcb_editor_open"] = editorOpen( FRAME_PCB_EDITOR );
    }
    else
    {
        ctx["project"] = "none open";
    }

    return ctx.dump();
}


std::vector<std::pair<std::string, std::string>> KICAD_COPILOT_TOOLBOX::GetQuickActions()
{
    return {
        { S( _( "New project..." ) ),
          "Let's design a new board. Ask me what it should do, then take it all the way to "
          "fabrication files." },
        { S( _( "Review project" ) ),
          "Review the open project end to end: run ERC and DRC, check the schematic and layout "
          "for design problems and manufacturability, and give me a prioritised list of fixes." },
        { S( _( "Make fab files" ) ),
          "Run ERC and DRC, fix anything blocking, then export JLCPCB fabrication and assembly "
          "files and tell me where they are." },
    };
}


// ---------------------------------------------------------------------------------------------
// Project tools
// ---------------------------------------------------------------------------------------------

COPILOT_TOOL_RESULT KICAD_COPILOT_TOOLBOX::getProjectInfo( const json& aIn )
{
    json j;

    if( !projectActive() )
    {
        j["project"] = nullptr;
        j["default_projects_folder"] = S( PATHS::GetDefaultUserProjectsPath() );
        return COPILOT_TOOL_RESULT::Ok( j, "no project open" );
    }

    wxFileName pro( m_frame->Prj().GetProjectFullName() );
    j["project"] = S( pro.GetFullPath() );
    j["folder"] = S( pro.GetPath() );
    j["schematic"] = S( m_frame->SchFileName() );
    j["board"] = S( m_frame->PcbFileName() );
    j["schematic_editor_open"] = editorOpen( FRAME_SCH );
    j["pcb_editor_open"] = editorOpen( FRAME_PCB_EDITOR );

    json    files = json::array();
    wxDir   dir( pro.GetPath() );
    wxString name;

    for( bool ok = dir.GetFirst( &name ); ok && files.size() < 80; ok = dir.GetNext( &name ) )
        files.push_back( S( name ) );

    j["files"] = files;

    json libs = json::object();

    for( LIBRARY_TABLE_TYPE type : { LIBRARY_TABLE_TYPE::SYMBOL, LIBRARY_TABLE_TYPE::FOOTPRINT } )
    {
        std::optional<LIBRARY_TABLE*> table = Pgm().GetLibraryManager().Table(
                type, LIBRARY_TABLE_SCOPE::PROJECT );
        json list = json::array();

        if( table && *table )
        {
            for( const LIBRARY_TABLE_ROW& row : ( *table )->Rows() )
                list.push_back( S( row.Nickname() ) + " = " + S( row.URI() ) );
        }

        libs[type == LIBRARY_TABLE_TYPE::SYMBOL ? "symbol" : "footprint"] = list;
    }

    j["project_libraries"] = libs;
    return COPILOT_TOOL_RESULT::Ok( j, S( pro.GetName() ) );
}


COPILOT_TOOL_RESULT KICAD_COPILOT_TOOLBOX::createProject( const json& aIn )
{
    wxString name = W( Str( aIn, "name" ) );
    name.Trim().Trim( false );

    // Keep names portable.
    for( wxUniChar bad : { '/', '\\', ':', '*', '?', '"', '<', '>', '|' } )
        name.Replace( wxString( bad ), wxS( "_" ) );

    name.Replace( wxS( " " ), wxS( "_" ) );

    if( name.IsEmpty() )
        return COPILOT_TOOL_RESULT::Error( "A project name is required." );

    wxString parent = W( Str( aIn, "directory" ) );

    if( CopilotIsRemotePath( parent ) )
    {
        return COPILOT_TOOL_RESULT::Error( "Network locations are not allowed; use a local "
                                           "folder." );
    }

    if( parent.IsEmpty() )
        parent = PATHS::GetDefaultUserProjectsPath();

    wxFileName dir( parent, wxEmptyString );
    dir.AppendDir( name );

    wxFileName pro( dir.GetPath(), name, FILEEXT::ProjectFileExtension );

    if( pro.FileExists() )
    {
        return COPILOT_TOOL_RESULT::Error( "A project already exists at " + S( pro.GetFullPath() )
                                           + "; choose another name or use open_project." );
    }

    if( !dir.DirExists() && !dir.Mkdir( wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
        return COPILOT_TOOL_RESULT::Error( "Cannot create folder " + S( dir.GetPath() ) );

    // Switching projects closes the editors; give the user the chance to save.
    if( projectActive() && !closeProject() )
        return COPILOT_TOOL_RESULT::Error( "The current project could not be closed." );

    m_frame->CreateNewProject( pro, true );

    if( !m_frame->LoadProject( pro ) )
        return COPILOT_TOOL_RESULT::Error( "The new project could not be opened." );

    openEditor( FRAME_SCH );
    openEditor( FRAME_PCB_EDITOR );

    json j = { { "project", S( pro.GetFullPath() ) },
               { "folder", S( pro.GetPath() ) },
               { "schematic_editor_open", editorOpen( FRAME_SCH ) },
               { "pcb_editor_open", editorOpen( FRAME_PCB_EDITOR ) } };

    return COPILOT_TOOL_RESULT::Ok( j, "created " + S( name ) );
}


COPILOT_TOOL_RESULT KICAD_COPILOT_TOOLBOX::openProject( const json& aIn )
{
    if( CopilotIsRemotePath( W( Str( aIn, "path" ) ) ) )
    {
        return COPILOT_TOOL_RESULT::Error( "Network locations are not allowed; use a local "
                                           "folder." );
    }

    wxFileName fn( W( Str( aIn, "path" ) ) );

    if( fn.DirExists() && fn.GetExt().IsEmpty() )
    {
        wxDir    dir( fn.GetFullPath() );
        wxString found;

        if( dir.GetFirst( &found, wxS( "*." ) + FILEEXT::ProjectFileExtension, wxDIR_FILES ) )
            fn = wxFileName( fn.GetFullPath(), found );
    }

    if( !fn.FileExists() || fn.GetExt() != FILEEXT::ProjectFileExtension )
        return COPILOT_TOOL_RESULT::Error( "No .kicad_pro file at " + S( fn.GetFullPath() ) );

    if( projectActive() && !closeProject() )
        return COPILOT_TOOL_RESULT::Error( "The current project could not be closed." );

    if( !m_frame->LoadProject( fn ) )
        return COPILOT_TOOL_RESULT::Error( "The project could not be opened." );

    openEditor( FRAME_SCH );
    openEditor( FRAME_PCB_EDITOR );

    return COPILOT_TOOL_RESULT::Ok( { { "project", S( fn.GetFullPath() ) } },
                                    "opened " + S( fn.GetName() ) );
}


COPILOT_TOOL_RESULT KICAD_COPILOT_TOOLBOX::openEditors( const json& aIn )
{
    if( !projectActive() )
        return COPILOT_TOOL_RESULT::Error( "No project is open." );

    std::string which = Str( aIn, "which", "both" );

    if( which != "pcb" )
        openEditor( FRAME_SCH );

    if( which != "schematic" )
        openEditor( FRAME_PCB_EDITOR );

    return COPILOT_TOOL_RESULT::Ok( { { "schematic_editor_open", editorOpen( FRAME_SCH ) },
                                      { "pcb_editor_open", editorOpen( FRAME_PCB_EDITOR ) } },
                                    "editors open" );
}


COPILOT_TOOL_RESULT KICAD_COPILOT_TOOLBOX::importLcscPart( const json& aIn )
{
    if( !projectActive() )
        return COPILOT_TOOL_RESULT::Error( "Open or create a project first." );

    std::vector<std::string> ids;

    for( std::string id : StrList( aIn, "lcsc_ids" ) )
    {
        for( char& c : id )
            c = (char) std::toupper( (unsigned char) c );

        if( !std::regex_match( id, std::regex( R"(C[0-9]{1,10})" ) ) )
            return COPILOT_TOOL_RESULT::Error( "'" + id + "' is not an LCSC number (C12345)." );

        ids.push_back( id );
    }

    if( ids.empty() )
        return COPILOT_TOOL_RESULT::Error( "lcsc_ids is required." );

    wxString python = CopilotPythonPath();

    if( python.IsEmpty() )
        return COPILOT_TOOL_RESULT::Error( "No Python interpreter was found. Install Python 3 or "
                                           "set it in Preferences > Scripting." );

    wxBusyCursor busy;

    // A private virtual environment keeps easyeda2kicad away from system packages.
    wxFileName venv( CopilotDataDir(), wxEmptyString );
    venv.AppendDir( wxS( "pyenv" ) );

#ifdef __WXMSW__
    wxFileName venvPython( venv.GetPath(), wxS( "python.exe" ) );
    venvPython.AppendDir( wxS( "Scripts" ) );
#else
    wxFileName venvPython( venv.GetPath(), wxS( "python" ) );
    venvPython.AppendDir( wxS( "bin" ) );
#endif

    std::string setupLog;

    if( !venvPython.FileExists() )
    {
        COPILOT_ACTIVITY       activity( _( "Creating a Python environment for part imports..." ) );
        COPILOT_PROCESS_RESULT r = CopilotRunProcess( { python, wxS( "-m" ), wxS( "venv" ),
                                                        venv.GetPath() },
                                                      CopilotDataDir(), 300 );
        setupLog += r.output;

        if( !venvPython.FileExists() )
        {
            return COPILOT_TOOL_RESULT::Error( "Could not create a Python virtual environment: "
                                               + tail( r.output, 2000 ) );
        }
    }

    COPILOT_PROCESS_RESULT check = CopilotRunProcess( { venvPython.GetFullPath(), wxS( "-m" ),
                                                        wxS( "easyeda2kicad" ), wxS( "-h" ) },
                                                      CopilotDataDir(), 120 );

    if( check.exitCode != 0 )
    {
        COPILOT_ACTIVITY       activity( _( "Installing easyeda2kicad (first use only)..." ) );
        COPILOT_PROCESS_RESULT pip = CopilotRunProcess( { venvPython.GetFullPath(), wxS( "-m" ),
                                                          wxS( "pip" ), wxS( "install" ),
                                                          wxS( "--upgrade" ),
                                                          wxS( "easyeda2kicad" ) },
                                                        CopilotDataDir(), 600 );
        setupLog += pip.output;

        if( pip.exitCode != 0 )
        {
            return COPILOT_TOOL_RESULT::Error( "Installing easyeda2kicad failed: "
                                               + tail( pip.output, 3000 ) );
        }
    }

    wxFileName libsDir( projectDir(), wxEmptyString );
    libsDir.AppendDir( wxS( "libs" ) );
    libsDir.Mkdir( wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );

    // Library nickname: "lcsc", unless the user already has a different library by that name.
    wxString nick = wxS( "lcsc" );

    {
        LIBRARY_MANAGER& manager = Pgm().GetLibraryManager();

        auto clashes = [&]( LIBRARY_TABLE_TYPE aType, const wxString& aNick, const wxString& aExt )
        {
            std::optional<LIBRARY_TABLE_ROW*> row =
                    manager.GetRow( aType, aNick, LIBRARY_TABLE_SCOPE::BOTH );
            return row && *row
                   && ( *row )->URI() != wxS( "${KIPRJMOD}/libs/" ) + aNick + aExt;
        };

        if( clashes( LIBRARY_TABLE_TYPE::SYMBOL, nick, wxS( ".kicad_sym" ) )
            || clashes( LIBRARY_TABLE_TYPE::FOOTPRINT, nick, wxS( ".pretty" ) ) )
        {
            nick = m_frame->Prj().GetProjectName() + wxS( "_lcsc" );

            for( size_t i = 0; i < nick.length(); ++i )
            {
                if( !( wxIsalnum( nick[i] ) || nick[i] == '_' || nick[i] == '-' ) )
                    nick[i] = '_';
            }
        }
    }

    wxFileName outBase( libsDir.GetPath(), nick );

    json imported = json::array();
    json failed = json::array();

    for( const std::string& id : ids )
    {
        COPILOT_ACTIVITY       activity( wxString::Format( _( "Importing %s from LCSC/EasyEDA..." ),
                                                           W( id ) ) );
        COPILOT_PROCESS_RESULT r = CopilotRunProcess(
                { venvPython.GetFullPath(), wxS( "-m" ), wxS( "easyeda2kicad" ), wxS( "--full" ),
                  wxS( "--lcsc_id=" ) + W( id ), wxS( "--output" ), outBase.GetFullPath(),
                  wxS( "--overwrite" ), wxS( "--project-relative" ) },
                projectDir(), 300 );

        // easyeda2kicad reports e.g. "Symbol name : RP2040" and "Footprint name : LQFN-56...".
        std::smatch m;
        std::string symbol, footprint;

        if( std::regex_search( r.output, m, std::regex( R"(Symbol name\s*:\s*([^\s\r\n]+))" ) ) )
            symbol = m[1];

        if( std::regex_search( r.output, m, std::regex( R"(Footprint name\s*:\s*([^\s\r\n]+))" ) ) )
            footprint = m[1];

        if( r.cancelled )
        {
            failed.push_back( { { "lcsc_id", id }, { "output", "Cancelled by the user." } } );
            break;
        }

        if( r.exitCode != 0 || ( symbol.empty() && footprint.empty() ) )
        {
            failed.push_back( { { "lcsc_id", id }, { "output", tail( r.output, 1500 ) } } );
            continue;
        }

        json part = { { "lcsc_id", id } };

        if( !symbol.empty() )
            part["symbol"] = S( nick ) + ":" + symbol;

        if( !footprint.empty() )
            part["footprint"] = S( nick ) + ":" + footprint;

        imported.push_back( part );
    }

    wxString error;

    if( !imported.empty() )
    {
        wxFileName symLib( libsDir.GetPath(), nick + wxS( ".kicad_sym" ) );
        wxFileName fpLib( libsDir.GetPath(), nick + wxS( ".pretty" ) );

        if( symLib.FileExists()
            && !CopilotRegisterProjectLibrary( LIBRARY_TABLE_TYPE::SYMBOL, nick,
                                               wxS( "${KIPRJMOD}/libs/" ) + nick
                                                       + wxS( ".kicad_sym" ),
                                               error ) )
        {
            failed.push_back( { { "library", "symbol" }, { "error", S( error ) } } );
        }

        if( wxDirExists( fpLib.GetFullPath() )
            && !CopilotRegisterProjectLibrary( LIBRARY_TABLE_TYPE::FOOTPRINT, nick,
                                               wxS( "${KIPRJMOD}/libs/" ) + nick
                                                       + wxS( ".pretty" ),
                                               error, &m_frame->Kiway() ) )
        {
            failed.push_back( { { "library", "footprint" }, { "error", S( error ) } } );
        }
    }

    json j = { { "imported", imported } };

    if( !failed.empty() )
    {
        j["failed"] = failed;
        j["hint"] = "Check the LCSC number; EasyEDA must be reachable from this computer.";
    }

    COPILOT_TOOL_RESULT result = COPILOT_TOOL_RESULT::Ok(
            j, std::to_string( imported.size() ) + " imported, " + std::to_string( failed.size() )
                       + " failed" );
    result.isError = imported.empty();
    return result;
}


// ---------------------------------------------------------------------------------------------
// Fabrication
// ---------------------------------------------------------------------------------------------

COPILOT_TOOL_RESULT KICAD_COPILOT_TOOLBOX::exportFabrication( const json& aIn )
{
    if( !projectActive() )
        return COPILOT_TOOL_RESULT::Error( "No project is open." );

    std::string house = Str( aIn, "house", "jlcpcb" );
    bool        assembly = Bool( aIn, "assembly" ).value_or( true );

    if( house != "jlcpcb" && house != "pcbway" && house != "generic" )
        return COPILOT_TOOL_RESULT::Error( "house must be jlcpcb, pcbway or generic." );

    wxString cli = CopilotKicadCliPath();

    if( cli.IsEmpty() )
        return COPILOT_TOOL_RESULT::Error( "kicad-cli was not found next to KiCad." );

    // Save both documents so kicad-cli sees the current design.
    json saveLog = json::object();

    if( editorOpen( FRAME_SCH ) )
    {
        COPILOT_TOOL_RESULT r = callEditor( FRAME_SCH, "save_schematic", json::object() );
        saveLog["schematic"] = r.isError ? r.content : "saved";
    }

    json boardInfo;

    if( editorOpen( FRAME_PCB_EDITOR ) )
    {
        COPILOT_TOOL_RESULT r = callEditor( FRAME_PCB_EDITOR, "save_board", json::object() );
        saveLog["board"] = r.isError ? r.content : "saved";

        COPILOT_TOOL_RESULT info = callEditor( FRAME_PCB_EDITOR, "get_board_summary",
                                               json::object() );

        if( !info.isError )
            boardInfo = json::parse( info.content, nullptr, false );
    }

    wxFileName board( m_frame->PcbFileName() );
    wxFileName schematic( m_frame->SchFileName() );

    if( !board.FileExists() )
        return COPILOT_TOOL_RESULT::Error( "The board file does not exist yet." );

    wxFileName outDir( projectDir(), wxEmptyString );
    outDir.AppendDir( wxS( "fabrication" ) );
    outDir.AppendDir( W( house ) );
    wxFileName gerberDir( outDir );
    gerberDir.AppendDir( wxS( "gerbers" ) );

    if( gerberDir.DirExists() )
        wxFileName::Rmdir( gerberDir.GetPath(), wxPATH_RMDIR_RECURSIVE );

    gerberDir.Mkdir( wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );

    wxString name = board.GetName();
    json     steps = json::array();
    bool     ok = true;
    bool     cancelled = false;

    auto run = [&]( const std::string& aLabel, const std::vector<wxString>& aArgs ) -> bool
    {
        if( cancelled )
            return false;

        std::vector<wxString> argv = { cli };
        argv.insert( argv.end(), aArgs.begin(), aArgs.end() );

        COPILOT_ACTIVITY activity( wxString::Format( _( "Generating fabrication outputs (%s)..." ),
                                                     wxString::FromUTF8( aLabel ) ) );
        COPILOT_PROCESS_RESULT r = CopilotRunProcess( argv, projectDir(), 600 );
        bool                   good = r.started && r.exitCode == 0;

        cancelled = r.cancelled;

        json step = { { "step", aLabel }, { "ok", good } };

        if( !good )
            step["output"] = tail( r.output, 2000 );

        steps.push_back( step );
        ok = ok && good;
        return good;
    };

    // Layers: every copper layer plus the usual technical layers.
    wxString layers;

    if( boardInfo.is_object() && boardInfo.contains( "copper_layer_names" ) )
    {
        for( const json& l : boardInfo["copper_layer_names"] )
            layers += W( l.get<std::string>() ) + wxS( "," );
    }
    else
    {
        layers = wxS( "F.Cu,B.Cu," );
    }

    layers += wxS( "F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts" );

    // No trailing separator: on Windows an argument ending in a backslash would be quoted as
    // ...\" and the closing quote misread (kicad-cli accepts a plain directory path).
    wxString gerberOut = gerberDir.GetPath();

    // --check-zones refills zones that are out of date, so the copper matches the design.
    run( "gerbers", { wxS( "pcb" ), wxS( "export" ), wxS( "gerbers" ), wxS( "--output" ), gerberOut,
                      wxS( "--layers" ), layers, wxS( "--subtract-soldermask" ),
                      wxS( "--check-zones" ), board.GetFullPath() } );

    run( "drill", { wxS( "pcb" ), wxS( "export" ), wxS( "drill" ), wxS( "--output" ), gerberOut,
                    wxS( "--format" ), wxS( "excellon" ), wxS( "--excellon-units" ), wxS( "mm" ),
                    wxS( "--generate-map" ), wxS( "--map-format" ), wxS( "gerberx2" ),
                    board.GetFullPath() } );

    wxFileName ipc( outDir.GetPath(), name + wxS( ".ipc" ) );
    run( "ipc-d-356 netlist", { wxS( "pcb" ), wxS( "export" ), wxS( "ipcd356" ), wxS( "--output" ),
                                ipc.GetFullPath(), board.GetFullPath() } );

    std::vector<wxString> gerberFiles;
    {
        wxDir    dir( gerberDir.GetPath() );
        wxString f;

        for( bool more = dir.GetFirst( &f, wxEmptyString, wxDIR_FILES ); more; more = dir.GetNext( &f ) )
            gerberFiles.push_back( wxFileName( gerberDir.GetPath(), f ).GetFullPath() );
    }

    wxFileName zip( outDir.GetPath(), name + wxS( "-" ) + W( house ) + wxS( "-gerbers.zip" ) );

    if( gerberFiles.empty() || !zipFiles( zip.GetFullPath(), gerberFiles ) )
    {
        ok = false;
        steps.push_back( { { "step", "zip" }, { "ok", false } } );
    }

    json outputs = { { "folder", S( outDir.GetPath() ) },
                     { "gerber_zip", S( zip.GetFullPath() ) },
                     { "gerber_files", (int) gerberFiles.size() },
                     { "ipc_netlist", S( ipc.GetFullPath() ) } };

    if( assembly )
    {
        // Pick and place
        wxFileName pos( outDir.GetPath(), name + wxS( "-pos-raw.csv" ) );

        if( run( "placement", { wxS( "pcb" ), wxS( "export" ), wxS( "pos" ), wxS( "--output" ),
                                pos.GetFullPath(), wxS( "--format" ), wxS( "csv" ), wxS( "--units" ),
                                wxS( "mm" ), wxS( "--side" ), wxS( "both" ), wxS( "--exclude-dnp" ),
                                board.GetFullPath() } ) )
        {
            std::string raw;
            readFile( pos.GetFullPath(), raw );

            // KiCad: Ref,Val,Package,PosX,PosY,Rot,Side  ->  JLCPCB CPL
            std::stringstream in( raw );
            std::string       line, cpl;
            bool              header = true;

            while( std::getline( in, line ) )
            {
                if( line.empty() )
                    continue;

                std::vector<std::string> f = splitCsv( line );

                if( header )
                {
                    cpl += house == "jlcpcb" ? "Designator,Mid X,Mid Y,Layer,Rotation\n"
                                             : "Designator,Footprint,Mid X,Mid Y,Layer,Rotation\n";
                    header = false;
                    continue;
                }

                if( f.size() < 7 )
                    continue;

                std::string side = f[6] == "bottom" ? "Bottom" : "Top";

                if( house == "jlcpcb" )
                {
                    cpl += csvField( f[0] ) + "," + f[3] + "mm," + f[4] + "mm," + side + ","
                           + f[5] + "\n";
                }
                else
                {
                    cpl += csvField( f[0] ) + "," + csvField( f[2] ) + "," + f[3] + "mm," + f[4]
                           + "mm," + side + "," + f[5] + "\n";
                }
            }

            wxFileName cplFile( outDir.GetPath(), name + wxS( "-CPL.csv" ) );
            writeFile( cplFile.GetFullPath(), cpl );
            wxRemoveFile( pos.GetFullPath() );
            outputs["cpl"] = S( cplFile.GetFullPath() );
        }

        // Bill of materials from the schematic
        if( schematic.FileExists() )
        {
            wxFileName bom( outDir.GetPath(), name + wxS( "-BOM.csv" ) );
            wxString   fields, labels;

            if( house == "jlcpcb" )
            {
                fields = wxS( "Value,Reference,Footprint,LCSC,${QUANTITY}" );
                labels = wxS( "Comment,Designator,Footprint,LCSC Part #,Quantity" );
            }
            else
            {
                fields = wxS( "Reference,${QUANTITY},Value,Footprint,Manufacturer,MPN,LCSC" );
                labels = wxS( "Designator,Quantity,Value,Package,Manufacturer,Manufacturer Part "
                              "Number,LCSC" );
            }

            if( run( "bom", { wxS( "sch" ), wxS( "export" ), wxS( "bom" ), wxS( "--output" ),
                              bom.GetFullPath(), wxS( "--fields" ), fields, wxS( "--labels" ),
                              labels, wxS( "--group-by" ), wxS( "Value,Footprint,LCSC" ),
                              wxS( "--ref-range-delimiter" ), wxEmptyString,
                              wxS( "--exclude-dnp" ), schematic.GetFullPath() } ) )
            {
                outputs["bom"] = S( bom.GetFullPath() );

                // Warn about parts the assembler cannot source automatically.
                std::string text;
                readFile( bom.GetFullPath(), text );
                std::stringstream in( text );
                std::string       line;
                int               lines = 0, missing = 0;

                std::getline( in, line );    // header

                while( std::getline( in, line ) )
                {
                    std::vector<std::string> f = splitCsv( line );

                    if( f.size() < 4 )
                        continue;

                    ++lines;

                    if( house == "jlcpcb" && f[3].empty() )
                        ++missing;
                }

                outputs["bom_lines"] = lines;

                if( missing > 0 )
                {
                    outputs["bom_warning"] = std::to_string( missing )
                                             + " BOM lines have no LCSC part number; JLCPCB "
                                               "assembly needs one per line (add an LCSC field).";
                }
            }
        }
    }

    m_lastFabZip = S( zip.GetFullPath() );
    m_lastFabHouse = house;
    m_lastBoardInfo = boardInfo;

    json j = { { "house", house }, { "ok", ok }, { "outputs", outputs }, { "steps", steps },
               { "saved", saveLog } };

    COPILOT_TOOL_RESULT r = COPILOT_TOOL_RESULT::Ok(
            j, std::string( ok ? "exported " : "export had errors: " ) + S( outDir.GetPath() ) );
    r.isError = !ok;
    return r;
}


COPILOT_TOOL_RESULT KICAD_COPILOT_TOOLBOX::sendToBoardHouse( const json& aIn )
{
    std::string house = Str( aIn, "house", m_lastFabHouse.empty() ? "jlcpcb" : m_lastFabHouse );

    if( m_lastFabZip.empty() || !wxFileExists( W( m_lastFabZip ) ) )
        return COPILOT_TOOL_RESULT::Error( "Run export_fabrication first." );

    // Only ever hand over the current project's files (the project may have been switched
    // without going through this toolbox).
    wxFileName zipFn( W( m_lastFabZip ) );
    wxFileName projFn( projectDir(), wxEmptyString );

    if( !projectActive()
        || !zipFn.GetPath().StartsWith( projFn.GetPath( wxPATH_GET_SEPARATOR ) ) )
    {
        m_lastFabZip.clear();
        return COPILOT_TOOL_RESULT::Error( "The last export belongs to another project; run "
                                           "export_fabrication for this project first." );
    }

    if( house != m_lastFabHouse && m_lastFabHouse != "generic" )
    {
        return COPILOT_TOOL_RESULT::Error( "The last export was for " + m_lastFabHouse
                                           + "; run export_fabrication for " + house + " first." );
    }

    wxFileName zip( W( m_lastFabZip ) );

    if( house == "jlcpcb" )
    {
        // JLCPCB has no public upload API: open the quote page and the folder with the zip.
        wxLaunchDefaultBrowser( wxS( "https://cart.jlcpcb.com/quote" ) );
        wxLaunchDefaultApplication( zip.GetPath() );

        return COPILOT_TOOL_RESULT::Ok(
                { { "opened", "https://cart.jlcpcb.com/quote" },
                  { "folder", S( zip.GetPath() ) },
                  { "next", "Drag " + S( zip.GetFullName() ) + " onto the JLCPCB page ('Add "
                            "gerber file'); for assembly, enable PCB Assembly and upload the "
                            "BOM and CPL CSV files from the same folder." } },
                "JLCPCB quote page opened" );
    }

    if( house != "pcbway" )
        return COPILOT_TOOL_RESULT::Error( "Unsupported board house '" + house + "'." );

    // PCBWay: the same upload their official KiCad plugin performs.  Bundle everything.
    std::vector<wxString> files = { zip.GetFullPath() };
    {
        wxDir    dir( zip.GetPath() );
        wxString f;

        for( bool more = dir.GetFirst( &f, wxEmptyString, wxDIR_FILES ); more; more = dir.GetNext( &f ) )
        {
            if( f.EndsWith( wxS( ".csv" ) ) || f.EndsWith( wxS( ".ipc" ) ) )
                files.push_back( wxFileName( zip.GetPath(), f ).GetFullPath() );
        }
    }

    wxFileName bundle( zip.GetPath(), zip.GetName() + wxS( "-pcbway-upload.zip" ) );

    // Put the gerber files themselves (not the gerber zip) plus netlist/BOM/CPL in one zip.
    std::vector<wxString> contents;
    {
        wxFileName gerberDir( zip.GetPath(), wxEmptyString );
        gerberDir.AppendDir( wxS( "gerbers" ) );
        wxDir    dir( gerberDir.GetPath() );
        wxString f;

        for( bool more = dir.GetFirst( &f, wxEmptyString, wxDIR_FILES ); more; more = dir.GetNext( &f ) )
            contents.push_back( wxFileName( gerberDir.GetPath(), f ).GetFullPath() );

        for( size_t i = 1; i < files.size(); ++i )
            contents.push_back( files[i] );
    }

    if( !zipFiles( bundle.GetFullPath(), contents ) )
        return COPILOT_TOOL_RESULT::Error( "Could not build the upload package." );

    double width = 0, height = 0;
    int    layers = 2;

    if( m_lastBoardInfo.is_object() )
    {
        if( m_lastBoardInfo.contains( "outline_bbox_mm" )
            && m_lastBoardInfo["outline_bbox_mm"].is_object() )
        {
            width = m_lastBoardInfo["outline_bbox_mm"].value( "w", 0.0 );
            height = m_lastBoardInfo["outline_bbox_mm"].value( "h", 0.0 );
        }

        layers = m_lastBoardInfo.value( "copper_layers", 2 );
    }

    // Read the package here: curl_mime_filedata() takes a narrow path, which fails for
    // non-ASCII user folders on Windows.
    std::string bundleData;
    std::string bundleName( bundle.GetFullName().utf8_str() );

    if( !readFile( bundle.GetFullPath(), bundleData ) || bundleData.empty() )
        return COPILOT_TOOL_RESULT::Error( "Could not read the upload package." );

    std::string userAgent = "KiCad-Copilot/" + std::string( GetBuildVersion().utf8_str() );
    std::string replyBody, curlError;
    long        status = 0;
    int         rc = CURLE_OK;

    COPILOT_ACTIVITY activity( _( "Uploading the fabrication files to PCBWay..." ) );
    wxBusyCursor     busy;

    bool finished = CopilotRunInBackground(
            [&]( const std::atomic<bool>& aCancel )
            {
                KICAD_CURL_EASY curl;
                curl.SetURL( "https://www.pcbway.com/Common/KiCadUpFile/" );
                curl.SetUserAgent( userAgent );
                curl.SetFollowRedirects( true );
                curl.SetConnectTimeout( 20 );
                curl.SetTimeout( 180 );
                curl.SetTransferCallback(
                        [&aCancel]( size_t, size_t, size_t, size_t ) -> int
                        {
                            return aCancel.load() ? 1 : 0;
                        },
                        0 );

                curl_mime*     mime = curl_mime_init( curl.GetCurl() );
                curl_mimepart* part = curl_mime_addpart( mime );
                curl_mime_name( part, "upload[file]" );
                curl_mime_data( part, bundleData.data(), bundleData.size() );
                curl_mime_filename( part, bundleName.c_str() );
                curl_mime_type( part, "application/zip" );

                auto addField = [&]( const char* aName, const std::string& aValue )
                {
                    curl_mimepart* p = curl_mime_addpart( mime );
                    curl_mime_name( p, aName );
                    curl_mime_data( p, aValue.c_str(), CURL_ZERO_TERMINATED );
                };

                addField( "boardWidth", std::to_string( width ) );
                addField( "boardHeight", std::to_string( height ) );
                addField( "boardLayer", std::to_string( layers ) );

                curl_easy_setopt( curl.GetCurl(), CURLOPT_MIMEPOST, mime );

                rc = curl.Perform();

                if( rc != CURLE_OK )
                    curlError = curl.GetErrorText( rc );

                status = curl.GetResponseStatusCode();
                replyBody = curl.GetBuffer();
                curl_mime_free( mime );
            } );

    if( !finished )
        return COPILOT_TOOL_RESULT::Error( "The upload was cancelled by the user." );

    if( rc != CURLE_OK )
        return COPILOT_TOOL_RESULT::Error( "Upload to PCBWay failed: " + curlError );

    json reply = json::parse( replyBody, nullptr, false );

    if( reply.is_discarded() || !reply.contains( "redirect" ) || !reply["redirect"].is_string() )
    {
        return COPILOT_TOOL_RESULT::Error( "Unexpected reply from PCBWay (HTTP "
                                           + std::to_string( status ) + "): "
                                           + CopilotTruncateUtf8( replyBody, 500 ) );
    }

    std::string url = reply["redirect"].get<std::string>();

    if( !url.empty() && url[0] == '/' )
        url = "https://www.pcbway.com" + url;

    // Only ever open PCBWay's own pages from this reply.
    bool trusted = url.rfind( "https://www.pcbway.com/", 0 ) == 0
                   || url.rfind( "https://pcbway.com/", 0 ) == 0
                   || url.rfind( "https://member.pcbway.com/", 0 ) == 0;

    if( !trusted )
    {
        return COPILOT_TOOL_RESULT::Error( "PCBWay returned an unexpected address (" + url
                                           + "); it was not opened. The upload package is "
                                           + S( bundle.GetFullPath() ) + "." );
    }

    wxLaunchDefaultBrowser( W( url ) );

    return COPILOT_TOOL_RESULT::Ok( { { "uploaded", S( bundle.GetFullName() ) },
                                      { "quote_page", url },
                                      { "next", "Review the options and price on the PCBWay page "
                                                "and use 'Save to Cart' there to order." } },
                                    "uploaded to PCBWay" );
}


COPILOT_TOOL_RESULT KICAD_COPILOT_TOOLBOX::openFolder( const json& aIn )
{
    wxString path = W( Str( aIn, "path" ) );

    if( CopilotIsRemotePath( path ) )
        return COPILOT_TOOL_RESULT::Error( "Network locations are not allowed." );

    if( path.IsEmpty() )
        path = projectDir();

    if( path.IsEmpty() || !wxDirExists( path ) )
        return COPILOT_TOOL_RESULT::Error( "Folder not found." );

    wxLaunchDefaultApplication( path );
    return COPILOT_TOOL_RESULT::Ok( { { "opened", S( path ) } }, S( path ) );
}
