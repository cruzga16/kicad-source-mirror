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

/**
 * @file pcb_copilot_layout.cpp
 *
 * The "build a board" half of the PCB Copilot toolbox: pulling the netlist from the schematic,
 * outline, design rules, cluster placement, copper pours, autorouting through Freerouting and
 * saving.
 */

#include <copilot/pcb_copilot_toolbox.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <regex>
#include <set>

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

#include <base_units.h>
#include <board.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <connectivity/connectivity_data.h>
#include <copilot/copilot_common_tools.h>
#include <footprint.h>
#include <kiface_base.h>
#include <libraries/library_table.h>
#include <netclass.h>
#include <netinfo.h>
#include <netlist_reader/board_netlist_updater.h>
#include <netlist_reader/pcb_netlist.h>
#include <pad.h>
#include <pcb_edit_frame.h>
#include <pcb_shape.h>
#include <pcb_track.h>
#include <pcbnew_settings.h>
#include <pgm_base.h>
#include <project/net_settings.h>
#include <reporter.h>
#include <settings/common_settings.h>
#include <specctra_import_export/specctra.h>
#include <tool/actions.h>
#include <tool/tool_manager.h>
#include <tools/zone_filler_tool.h>
#include <drc/drc_engine.h>
#include <zone.h>
#include <zone_settings.h>


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


double mm( long long aIU )
{
    return std::round( pcbIUScale.IUTomm( (int) aIU ) * 1000.0 ) / 1000.0;
}


int toIU( double aMM )
{
    // Keep model supplied values inside the board/sheet coordinate range.
    if( !std::isfinite( aMM ) )
        aMM = 0.0;

    return pcbIUScale.mmToIU( std::clamp( aMM, -1500.0, 1500.0 ) );
}


wxString refPrefix( const wxString& aRef )
{
    wxString prefix;

    for( wxUniChar c : aRef )
    {
        if( wxIsalpha( c ) || c == '_' )
            prefix += c;
        else
            break;
    }

    return prefix.Upper();
}


bool isGroundName( const wxString& aName )
{
    static const std::regex re( R"(^/?(.*[/_-])?(gnd|agnd|dgnd|pgnd|sgnd|gnda|gndd|vss|0v|earth|chassis)[a-z0-9_]*$)",
                                std::regex::icase );
    return std::regex_match( S( aName ), re );
}


bool isConnectorPrefix( const wxString& aPrefix )
{
    return aPrefix == wxS( "J" ) || aPrefix == wxS( "P" ) || aPrefix == wxS( "CN" )
           || aPrefix == wxS( "USB" ) || aPrefix == wxS( "CON" ) || aPrefix == wxS( "X" );
}


bool isMechanicalPrefix( const wxString& aPrefix )
{
    return aPrefix == wxS( "H" ) || aPrefix == wxS( "MH" ) || aPrefix == wxS( "FID" )
           || aPrefix == wxS( "LOGO" ) || aPrefix == wxS( "G" ) || aPrefix == wxS( "REF" );
}


/// Bounding box of a footprint's body (courtyard when present), relative to nothing.
BOX2I bodyBox( FOOTPRINT* aFp )
{
    aFp->BuildCourtyardCaches();
    PCB_LAYER_ID          layer = aFp->GetLayer() == B_Cu ? B_CrtYd : F_CrtYd;
    const SHAPE_POLY_SET& court = aFp->GetCourtyard( layer );

    if( court.OutlineCount() > 0 )
        return court.BBox();

    BOX2I box = aFp->GetBoundingBox( false );

    for( PAD* pad : aFp->Pads() )
        box.Merge( pad->GetBoundingBox() );

    return box;
}


bool overlaps( const BOX2I& aBox, const std::vector<BOX2I>& aOthers, int aMargin )
{
    BOX2I inflated = aBox;
    inflated.Inflate( aMargin / 2 );

    for( const BOX2I& o : aOthers )
    {
        BOX2I other = o;
        other.Inflate( aMargin / 2 );

        if( inflated.Intersects( other ) )
            return true;
    }

    return false;
}


/// Major version of a Java runtime (0 if it does not run).
int javaMajorVersion( const wxString& aJava )
{
    COPILOT_PROCESS_RESULT r = CopilotRunProcess( { aJava, wxS( "-version" ) }, wxEmptyString, 30 );

    // e.g. openjdk version "25.0.4" 2026-08-18   or   java version "1.8.0_401"
    static const std::regex re( R"(version \"(\d+)(\.(\d+))?)" );
    std::smatch             m;

    if( !r.started || !std::regex_search( r.output, m, re ) )
        return 0;

    int major = std::stoi( m[1] );

    if( major == 1 && m[3].matched )
        major = std::stoi( m[3] );

    return major;
}


/**
 * The newest Java runtime on this computer: JAVA_HOME, PATH and the usual install folders are
 * checked (several JDKs are often installed side by side, and Freerouting needs a recent one).
 */
wxString findNewestJava()
{
    static std::map<wxString, int> s_versions;      // java executable -> major version

#ifdef __WXMSW__
    const wxString exe = wxS( "java.exe" );
#else
    const wxString exe = wxS( "java" );
#endif

    std::vector<wxString> candidates;

    auto addHome = [&]( const wxString& aHome )
    {
        wxFileName j( aHome, wxEmptyString );
        j.AppendDir( wxS( "bin" ) );
        j.SetFullName( exe );

        if( j.FileExists() )
            candidates.push_back( j.GetFullPath() );
    };

    wxString javaHome;

    if( wxGetEnv( wxS( "JAVA_HOME" ), &javaHome ) && !javaHome.IsEmpty() )
        addHome( javaHome );

    wxString onPath = CopilotFindOnPath( wxS( "java" ) );

    if( !onPath.IsEmpty() )
        candidates.push_back( onPath );

    std::vector<wxString> roots;

#if defined( __WXMSW__ )
    for( const wxString& base : { wxS( "C:\\Program Files\\Eclipse Adoptium" ),
                                  wxS( "C:\\Program Files\\Java" ),
                                  wxS( "C:\\Program Files\\Microsoft" ),
                                  wxS( "C:\\Program Files\\Zulu" ),
                                  wxS( "C:\\Program Files\\Amazon Corretto" ) } )
    {
        roots.push_back( base );
    }
#elif defined( __WXMAC__ )
    roots.push_back( wxS( "/Library/Java/JavaVirtualMachines" ) );
#else
    roots.push_back( wxS( "/usr/lib/jvm" ) );
#endif

    for( const wxString& root : roots )
    {
        wxDir dir( root );

        if( !dir.IsOpened() )
            continue;

        wxString sub;

        for( bool more = dir.GetFirst( &sub, wxEmptyString, wxDIR_DIRS ); more;
             more = dir.GetNext( &sub ) )
        {
            wxFileName home( root, wxEmptyString );
            home.AppendDir( sub );
#ifdef __WXMAC__
            home.AppendDir( wxS( "Contents" ) );
            home.AppendDir( wxS( "Home" ) );
#endif
            addHome( home.GetPath() );
        }
    }

    wxString best;
    int      bestVersion = 0;

    for( const wxString& java : candidates )
    {
        wxString real = wxFileName( java ).GetAbsolutePath();

        if( !s_versions.count( real ) )
            s_versions[real] = javaMajorVersion( java );

        if( s_versions[real] > bestVersion )
        {
            bestVersion = s_versions[real];
            best = java;
        }
    }

    return best;
}


wxString findFreerouting( wxString& aJava, wxString& aHow )
{
    aJava = wxEmptyString;

    wxString configured;

    if( COMMON_SETTINGS* common = Pgm().GetCommonSettings() )
        configured = common->m_Copilot.freerouting_path;

    std::vector<wxString> candidates;

    if( !configured.IsEmpty() )
        candidates.push_back( configured );

    wxString onPath = CopilotFindOnPath( wxS( "freerouting" ) );

    if( !onPath.IsEmpty() )
        candidates.push_back( onPath );

    candidates.push_back( wxFileName( CopilotDataDir(), wxS( "freerouting.jar" ) ).GetFullPath() );

    for( const wxString& c : candidates )
    {
        if( !wxFileExists( c ) )
            continue;

        if( c.Lower().EndsWith( wxS( ".jar" ) ) )
        {
            aJava = findNewestJava();
            if( aJava.IsEmpty() )
            {
                aHow = wxS( "Found " ) + c + wxS( " but no Java runtime. Install Java 25 or newer "
                                                   "(e.g. Eclipse Temurin) and try again." );
                return wxEmptyString;
            }
        }

        return c;
    }

    aHow = wxS( "Freerouting was not found. Call install_freerouting (needs Java 25+), or set "
                "its path in Copilot Settings." );
    return wxEmptyString;
}

} // namespace


void PCB_COPILOT_TOOLBOX::registerLayoutTools()
{
    auto bind = [this]( COPILOT_TOOL_RESULT ( PCB_COPILOT_TOOLBOX::*aFn )( const json& ) )
    {
        return [this, aFn]( const json& aIn ) { return ( this->*aFn )( aIn ); };
    };

    add( "update_from_schematic",
         "Pull the netlist from the schematic editor into the board (like Tools > Update PCB "
         "from Schematic): adds missing footprints, updates nets and values, removes footprints "
         "no longer in the schematic. The schematic must be fully annotated and every symbol "
         "needs a valid Footprint field. Returns the update log (missing footprints are listed "
         "there).",
         { { "delete_unused", Boolean( "Remove footprints not in the schematic (default true)." ) } },
         {}, MODIFIES, bind( &PCB_COPILOT_TOOLBOX::updateFromSchematic ) );

    add( "set_board_outline",
         "Replace the board outline (Edge.Cuts) with a rectangle, optionally with rounded "
         "corners.",
         { { "width_mm", Number( "Board width." ) },
           { "height_mm", Number( "Board height." ) },
           { "x_mm", Number( "Left edge X (default 100)." ) },
           { "y_mm", Number( "Top edge Y (default 100)." ) },
           { "corner_radius_mm", Number( "Corner radius (default 0)." ) } },
         { "width_mm", "height_mm" }, MODIFIES, bind( &PCB_COPILOT_TOOLBOX::setBoardOutline ) );

    add( "set_design_rules",
         "Set board-level minimum constraints and net class values, e.g. to match a board house "
         "(JLCPCB standard 2-layer: 0.127 mm track/clearance min, 0.3 mm via drill, 0.6 mm via; "
         "use 0.2 mm/0.2 mm defaults for margin). Pass 'netclass' with 'patterns' to create a "
         "class such as Power (wider tracks) for nets matching the wildcard patterns.",
         { { "netclass", String( "Net class to edit or create (default 'Default')." ) },
           { "patterns", StringArray( "Net name wildcard patterns to assign to the net class, "
                                      "e.g. ['+*', 'GND', 'VBUS']." ) },
           { "track_width_mm", Number( "Net class track width." ) },
           { "clearance_mm", Number( "Net class clearance." ) },
           { "via_diameter_mm", Number( "Net class via diameter." ) },
           { "via_drill_mm", Number( "Net class via drill." ) },
           { "min_track_width_mm", Number( "Board minimum track width." ) },
           { "min_clearance_mm", Number( "Board minimum clearance." ) },
           { "min_via_diameter_mm", Number( "Board minimum via diameter." ) },
           { "min_through_drill_mm", Number( "Board minimum drill." ) },
           { "copper_to_edge_mm", Number( "Copper to board edge clearance." ) } },
         {}, MODIFIES, bind( &PCB_COPILOT_TOOLBOX::setDesignRules ) );

    add( "arrange_footprints",
         "Automatic placement: groups each IC with the passives on its non-ground nets (so "
         "decoupling capacitors sit next to the pins they serve), places connectors along the "
         "board edges and packs the groups inside the outline without overlaps. Run after "
         "update_from_schematic and set_board_outline. Locked footprints are not moved. "
         "Afterwards refine with move_footprint if needed.",
         { { "spacing_mm", Number( "Gap between parts (default 0.8)." ) },
           { "grow_board", Boolean( "Enlarge the outline if the parts do not fit (default "
                                    "true)." ) } },
         {}, MODIFIES, bind( &PCB_COPILOT_TOOLBOX::arrangeFootprints ) );

    add( "add_zone",
         "Add a copper pour (e.g. GND) covering the board on the given layers, then fill all "
         "zones.",
         { { "net", String( "Net name, e.g. GND." ) },
           { "layers", StringArray( "Copper layers (default ['F.Cu','B.Cu'])." ) },
           { "clearance_mm", Number( "Zone clearance (default 0.3)." ) },
           { "min_width_mm", Number( "Minimum copper width (default 0.25)." ) },
           { "priority", Integer( "Zone priority (default 0)." ) } },
         { "net" }, MODIFIES, bind( &PCB_COPILOT_TOOLBOX::addZone ) );

    add( "fill_zones", "Refill all copper zones (run after routing changes and before DRC).",
         json::object(), {}, MODIFIES, bind( &PCB_COPILOT_TOOLBOX::fillZones ) );

    add( "autoroute",
         "Route all unrouted connections with the Freerouting autorouter (exports a Specctra "
         "DSN, runs Freerouting headless, imports the session). Set design rules first; add "
         "ground pours after routing. Takes from seconds to several minutes.",
         { { "passes", Integer( "Maximum autorouter passes (default 20)." ) },
           { "timeout_s", Integer( "Give up after this many seconds (default 900)." ) } },
         {}, MODIFIES | APPROVAL, bind( &PCB_COPILOT_TOOLBOX::autoroute ) );

    add( "install_freerouting",
         "Download the latest Freerouting autorouter (.jar) from its GitHub releases into "
         "KiCad's cache folder. Requires a Java 25+ runtime to run.",
         json::object(), {}, APPROVAL, bind( &PCB_COPILOT_TOOLBOX::installFreerouting ) );

    add( "register_footprint_library",
         "Add a footprint library folder (.pretty) to the project's footprint library table so "
         "its footprints can be used.",
         { { "nickname", String( "Library nickname." ) },
           { "path", String( "Path to the .pretty folder (may use ${KIPRJMOD})." ) } },
         { "nickname", "path" }, MODIFIES,
         bind( &PCB_COPILOT_TOOLBOX::registerFootprintLibrary ) );

    add( "save_board", "Save the board file.", json::object(), {}, MODIFIES,
         bind( &PCB_COPILOT_TOOLBOX::saveBoard ) );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::updateFromSchematic( const json& aIn )
{
    if( Kiface().IsSingle() )
    {
        return COPILOT_TOOL_RESULT::Error( "The PCB editor was started on its own; open the "
                                           "project in the KiCad project manager to link it to "
                                           "the schematic." );
    }

    NETLIST netlist;

    if( !m_frame->FetchNetlistFromSchematic( netlist,
                                             _( "Updating the PCB requires a fully annotated "
                                                "schematic." ) ) )
    {
        return COPILOT_TOOL_RESULT::Error( "Could not get the netlist from the schematic (is it "
                                           "open and fully annotated?)." );
    }

    bool deleteUnused = Bool( aIn, "delete_unused" ).value_or( true );

    netlist.SortByReference();
    netlist.SetFindByTimeStamp( true );
    netlist.SetReplaceFootprints( true );

    m_frame->GetToolManager()->DeactivateTool();
    m_frame->GetToolManager()->RunAction( ACTIONS::selectionClear );

    WX_STRING_REPORTER    reporter;
    BOARD_NETLIST_UPDATER updater( m_frame, board() );
    updater.SetReporter( &reporter );
    updater.SetIsDryRun( false );
    updater.SetLookupByTimestamp( true );
    updater.SetDeleteUnusedFootprints( deleteUnused );
    updater.SetReplaceFootprints( true );
    updater.SetUpdateFields( true );
    updater.UpdateNetlist( netlist );

    bool runDrag = false;
    m_frame->OnNetlistChanged( updater, &runDrag );

    // OnNetlistChanged() selects the new footprints so the user can drag them into place, and
    // the updater flags them FP_JUST_ADDED, which keeps their pads out of the connectivity
    // database until that interactive move clears it.  The Copilot places parts itself
    // (arrange_footprints), so do what the move tool would: clear the flag, deselect, and
    // rebuild connectivity (the footprints were also spread outside of a commit).
    for( FOOTPRINT* fp : updater.GetAddedFootprints() )
        fp->SetAttributes( fp->GetAttributes() & ~FP_JUST_ADDED );

    m_frame->GetToolManager()->RunAction( ACTIONS::selectionClear );
    board()->BuildConnectivity();
    m_frame->GetCanvas()->RedrawRatsnest();

    std::string log = S( reporter.GetMessages() );

    if( log.size() > 8000 )
        log = CopilotTruncateUtf8( log, 8000 ) + "\n...[log truncated]";

    json j = { { "footprints_on_board", (int) board()->Footprints().size() },
               { "added_footprints", (int) updater.GetAddedFootprints().size() },
               { "errors", updater.GetErrorCount() },
               { "warnings", updater.GetWarningCount() },
               { "log", log } };

    if( updater.GetErrorCount() > 0 )
    {
        j["hint"] = "Errors usually mean a symbol's Footprint field names a footprint that does "
                    "not exist in the libraries. Fix it in the schematic and update again.";
    }

    COPILOT_TOOL_RESULT r = COPILOT_TOOL_RESULT::Ok(
            j, std::to_string( board()->Footprints().size() ) + " footprints, "
                       + std::to_string( updater.GetErrorCount() ) + " errors" );
    return r;
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::setBoardOutline( const json& aIn )
{
    std::optional<double> w = Num( aIn, "width_mm" );
    std::optional<double> h = Num( aIn, "height_mm" );

    if( !w || !h || *w < 2 || *h < 2 || *w > 1000 || *h > 1000 )
        return COPILOT_TOOL_RESULT::Error( "width_mm and height_mm must be between 2 and 1000." );

    double x = Num( aIn, "x_mm" ).value_or( 100.0 );
    double y = Num( aIn, "y_mm" ).value_or( 100.0 );
    double r = std::clamp( Num( aIn, "corner_radius_mm" ).value_or( 0.0 ), 0.0,
                           std::min( *w, *h ) / 2.0 );

    BOARD_COMMIT commit( m_frame );

    for( BOARD_ITEM* item : board()->Drawings() )
    {
        if( item->GetLayer() == Edge_Cuts && item->Type() == PCB_SHAPE_T )
            commit.Remove( item );
    }

    PCB_SHAPE* rect = new PCB_SHAPE( board(), SHAPE_T::RECTANGLE );
    rect->SetLayer( Edge_Cuts );
    rect->SetStart( VECTOR2I( toIU( x ), toIU( y ) ) );
    rect->SetEnd( VECTOR2I( toIU( x + *w ), toIU( y + *h ) ) );
    rect->SetWidth( toIU( 0.1 ) );

    if( r > 0 )
        rect->SetCornerRadius( toIU( r ) );

    commit.Add( rect );
    commit.Push( _( "Copilot: set board outline" ) );

    return COPILOT_TOOL_RESULT::Ok( { { "x_mm", x }, { "y_mm", y }, { "width_mm", *w },
                                      { "height_mm", *h }, { "corner_radius_mm", r } },
                                    std::to_string( (int) *w ) + " x " + std::to_string( (int) *h )
                                            + " mm outline" );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::setDesignRules( const json& aIn )
{
    BOARD_DESIGN_SETTINGS&        bds = board()->GetDesignSettings();
    std::shared_ptr<NET_SETTINGS> ns = bds.m_NetSettings;
    json                          changed = json::object();

    auto setMin = [&]( const char* aKey, int& aTarget )
    {
        if( std::optional<double> v = Num( aIn, aKey ); v && *v >= 0 && *v < 50 )
        {
            aTarget = toIU( *v );
            changed[aKey] = *v;
        }
    };

    setMin( "min_track_width_mm", bds.m_TrackMinWidth );
    setMin( "min_clearance_mm", bds.m_MinClearance );
    setMin( "min_via_diameter_mm", bds.m_ViasMinSize );
    setMin( "min_through_drill_mm", bds.m_MinThroughDrill );
    setMin( "copper_to_edge_mm", bds.m_CopperEdgeClearance );

    wxString className = W( Str( aIn, "netclass", "Default" ) );

    std::shared_ptr<NETCLASS> nc;

    if( className.CmpNoCase( wxS( "Default" ) ) == 0 )
    {
        nc = ns->GetDefaultNetclass();
    }
    else if( ns->HasNetclass( className ) )
    {
        nc = ns->GetNetclasses().at( className );
    }
    else
    {
        nc = std::make_shared<NETCLASS>( className, false );
        ns->SetNetclass( className, nc );
        changed["created_netclass"] = S( className );
    }

    auto setNc = [&]( const char* aKey, const std::function<void( int )>& aSetter )
    {
        if( std::optional<double> v = Num( aIn, aKey ); v && *v > 0 && *v < 50 )
        {
            aSetter( toIU( *v ) );
            changed[aKey] = *v;
        }
    };

    setNc( "track_width_mm", [&]( int v ) { nc->SetTrackWidth( v ); } );
    setNc( "clearance_mm", [&]( int v ) { nc->SetClearance( v ); } );
    setNc( "via_diameter_mm", [&]( int v ) { nc->SetViaDiameter( v ); } );
    setNc( "via_drill_mm", [&]( int v ) { nc->SetViaDrill( v ); } );

    for( const std::string& pattern : StrList( aIn, "patterns" ) )
    {
        ns->SetNetclassPatternAssignment( W( pattern ), className );
        changed["patterns"].push_back( pattern );
    }

    ns->ClearAllCaches();
    board()->SynchronizeNetsAndNetClasses( false );

    // Recompile the DRC rules so the router and checks use the new values right away.
    if( bds.m_DRCEngine )
    {
        try
        {
            bds.m_DRCEngine->InitEngine( board()->GetDesignRulesPath() );
        }
        catch( PARSE_ERROR& e )
        {
            changed["warning"] = "Custom design rules failed to compile: " + S( e.What() );
        }
    }

    m_frame->OnModify();
    m_frame->UpdateUserInterface();

    if( changed.empty() )
        return COPILOT_TOOL_RESULT::Error( "Nothing to change." );

    return COPILOT_TOOL_RESULT::Ok( { { "netclass", S( className ) }, { "changed", changed } },
                                    "rules updated" );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::arrangeFootprints( const json& aIn )
{
    BOARD* brd = board();
    int    gap = toIU( std::clamp( Num( aIn, "spacing_mm" ).value_or( 0.8 ), 0.1, 10.0 ) );
    bool   grow = Bool( aIn, "grow_board" ).value_or( true );

    BOX2I area = brd->GetBoardEdgesBoundingBox();

    if( (int) area.GetWidth() <= 0 || (int) area.GetHeight() <= 0 )
        return COPILOT_TOOL_RESULT::Error( "There is no board outline; call set_board_outline "
                                           "first." );

    int edgeMargin = std::max( toIU( 1.0 ), brd->GetDesignSettings().m_CopperEdgeClearance );

    std::vector<FOOTPRINT*> movable;

    for( FOOTPRINT* fp : brd->Footprints() )
    {
        if( !fp->IsLocked() )
            movable.push_back( fp );
    }

    if( movable.empty() )
        return COPILOT_TOOL_RESULT::Error( "No unlocked footprints to arrange." );

    // --- classify ------------------------------------------------------------------------
    std::vector<FOOTPRINT*> anchors, satellites, connectors, others;

    for( FOOTPRINT* fp : movable )
    {
        wxString prefix = refPrefix( fp->GetReference() );

        if( isConnectorPrefix( prefix ) || isMechanicalPrefix( prefix ) )
            connectors.push_back( fp );
        else if( fp->Pads().size() >= 3 && ( prefix == wxS( "U" ) || prefix == wxS( "IC" )
                                             || prefix == wxS( "Q" ) || prefix == wxS( "Y" )
                                             || prefix == wxS( "X" ) || fp->Pads().size() >= 6 ) )
            anchors.push_back( fp );
        else if( fp->Pads().size() <= 3 )
            satellites.push_back( fp );
        else
            others.push_back( fp );
    }

    std::sort( anchors.begin(), anchors.end(),
               []( FOOTPRINT* a, FOOTPRINT* b ) { return a->Pads().size() > b->Pads().size(); } );

    // --- assign each satellite to the anchor it shares the most non-ground nets with ----------
    std::map<FOOTPRINT*, std::vector<std::pair<FOOTPRINT*, PAD*>>> groups;  // anchor -> (sat, anchor pad)

    for( FOOTPRINT* sat : satellites )
    {
        FOOTPRINT* bestAnchor = nullptr;
        PAD*       bestPad = nullptr;
        int        bestScore = 0;

        for( FOOTPRINT* anchor : anchors )
        {
            int  score = 0;
            PAD* pad = nullptr;

            for( PAD* sp : sat->Pads() )
            {
                if( sp->GetNetCode() <= 0 || isGroundName( sp->GetNetname() ) )
                    continue;

                for( PAD* ap : anchor->Pads() )
                {
                    if( ap->GetNetCode() == sp->GetNetCode() )
                    {
                        ++score;

                        if( !pad )
                            pad = ap;
                    }
                }
            }

            if( score > bestScore )
            {
                bestScore = score;
                bestAnchor = anchor;
                bestPad = pad;
            }
        }

        if( bestAnchor )
            groups[bestAnchor].emplace_back( sat, bestPad );
        else
            others.push_back( sat );
    }

    BOARD_COMMIT commit( m_frame );

    for( FOOTPRINT* fp : movable )
    {
        commit.Modify( fp );

        if( fp->GetLayer() == B_Cu )
            fp->Flip( fp->GetPosition(), m_frame->GetPcbNewSettings()->m_FlipDirection );
    }

    // --- build each group around its anchor at a temporary origin --------------------------
    struct CLUSTER
    {
        std::vector<FOOTPRINT*> members;
        BOX2I                   box;
    };

    std::vector<CLUSTER> clusters;

    for( FOOTPRINT* anchor : anchors )
    {
        CLUSTER cl;
        anchor->SetOrientation( ANGLE_0 );
        anchor->SetPosition( VECTOR2I( 0, 0 ) );
        cl.members.push_back( anchor );

        BOX2I              abox = bodyBox( anchor );
        std::vector<BOX2I> placed = { abox };

        for( auto& [sat, pad] : groups[anchor] )
        {
            VECTOR2I target = pad ? pad->GetPosition() : abox.GetCenter();

            // Which side of the anchor is the pad nearest to?
            int dl = target.x - abox.GetLeft();
            int dr = abox.GetRight() - target.x;
            int dt = target.y - abox.GetTop();
            int db = abox.GetBottom() - target.y;
            int m = std::min( { dl, dr, dt, db } );

            VECTOR2I normal( 0, 0 );

            if( m == dl )
                normal = VECTOR2I( -1, 0 );
            else if( m == dr )
                normal = VECTOR2I( 1, 0 );
            else if( m == dt )
                normal = VECTOR2I( 0, -1 );
            else
                normal = VECTOR2I( 0, 1 );

            // Two-pad parts: put the pad axis perpendicular to the anchor edge so one pad
            // faces the pin.
            sat->SetOrientation( normal.x != 0 ? ANGLE_0 : ANGLE_90 );
            sat->SetPosition( VECTOR2I( 0, 0 ) );

            BOX2I    sbox = bodyBox( sat );
            VECTOR2I half( (int) sbox.GetWidth() / 2, (int) sbox.GetHeight() / 2 );
            VECTOR2I centerOffset = sbox.GetCenter();     // body centre relative to position

            // Start just outside the anchor body, in line with the pad.
            VECTOR2I base = target;

            if( normal.x < 0 )
                base.x = abox.GetLeft() - gap - half.x;
            else if( normal.x > 0 )
                base.x = abox.GetRight() + gap + half.x;
            else if( normal.y < 0 )
                base.y = abox.GetTop() - gap - half.y;
            else
                base.y = abox.GetBottom() + gap + half.y;

            VECTOR2I tangent( normal.y != 0 ? 1 : 0, normal.x != 0 ? 1 : 0 );
            int      step = std::max( ( normal.x != 0 ? (int) sbox.GetHeight() : (int) sbox.GetWidth() ) + gap,
                                      toIU( 0.5 ) );
            bool     done = false;

            // Slide along the edge, then move outward, until there is room.
            for( int ring = 0; ring < 12 && !done; ++ring )
            {
                VECTOR2I ringBase = base + normal * ( ring * ( std::max( (int) sbox.GetWidth(),
                                                                         (int) sbox.GetHeight() ) + gap ) );

                for( int k = 0; k < 40 && !done; ++k )
                {
                    int      offs = ( k % 2 == 0 ? 1 : -1 ) * ( ( k + 1 ) / 2 ) * step;
                    VECTOR2I c = ringBase + tangent * offs;
                    BOX2I    candidate( c - half, VECTOR2I( (int) sbox.GetWidth(), (int) sbox.GetHeight() ) );

                    if( !overlaps( candidate, placed, gap ) )
                    {
                        sat->SetPosition( c - centerOffset );
                        placed.push_back( candidate );
                        done = true;
                    }
                }
            }

            if( !done )
            {
                others.push_back( sat );
                continue;
            }

            cl.members.push_back( sat );
        }

        for( const BOX2I& b : placed )
        {
            if( (int) cl.box.GetWidth() == 0 && (int) cl.box.GetHeight() == 0 )
                cl.box = b;
            else
                cl.box.Merge( b );
        }

        clusters.push_back( cl );
    }

    // Loose parts become single-member clusters.
    for( FOOTPRINT* fp : others )
    {
        fp->SetOrientation( ANGLE_0 );
        fp->SetPosition( VECTOR2I( 0, 0 ) );
        clusters.push_back( { { fp }, bodyBox( fp ) } );
    }

    std::sort( clusters.begin(), clusters.end(),
               []( const CLUSTER& a, const CLUSTER& b )
               {
                   return (long long) a.box.GetWidth() * (int) a.box.GetHeight()
                          > (long long) b.box.GetWidth() * (int) b.box.GetHeight();
               } );

    // --- connectors along the left/right edges ----------------------------------------------
    int      leftY = area.GetTop() + edgeMargin;
    int      rightY = area.GetTop() + edgeMargin;
    int      leftBand = 0, rightBand = 0;
    bool     toLeft = true;
    json     connectorPlacement = json::array();

    for( FOOTPRINT* fp : connectors )
    {
        fp->SetOrientation( toLeft ? ANGLE_0 : ANGLE_180 );
        fp->SetPosition( VECTOR2I( 0, 0 ) );
        BOX2I b = bodyBox( fp );

        int x = toLeft ? area.GetLeft() + edgeMargin - b.GetLeft()
                       : area.GetRight() - edgeMargin - b.GetRight();
        int y = ( toLeft ? leftY : rightY ) - b.GetTop();

        fp->SetPosition( VECTOR2I( x, y ) );

        if( toLeft )
        {
            leftY += (int) b.GetHeight() + gap * 2;
            leftBand = std::max( leftBand, (int) b.GetWidth() );
        }
        else
        {
            rightY += (int) b.GetHeight() + gap * 2;
            rightBand = std::max( rightBand, (int) b.GetWidth() );
        }

        connectorPlacement.push_back( S( fp->GetReference() ) + ( toLeft ? " (left)" : " (right)" ) );
        toLeft = !toLeft;
    }

    // --- shelf-pack clusters in the remaining area ------------------------------------------
    auto pack = [&]( const BOX2I& aRegion, bool aApply, int& aUsedHeight ) -> bool
    {
        int  x = aRegion.GetLeft();
        int  y = aRegion.GetTop();
        int  shelf = 0;
        bool fits = true;

        for( CLUSTER& cl : clusters )
        {
            int w = (int) cl.box.GetWidth();
            int h = (int) cl.box.GetHeight();

            if( x > aRegion.GetLeft() && x + w > aRegion.GetRight() )
            {
                x = aRegion.GetLeft();
                y += shelf + gap * 2;
                shelf = 0;
            }

            if( aApply )
            {
                VECTOR2I delta( x - cl.box.GetLeft(), y - cl.box.GetTop() );

                for( FOOTPRINT* fp : cl.members )
                    fp->Move( delta );
            }

            if( x + w > aRegion.GetRight() || y + h > aRegion.GetBottom() )
                fits = false;

            x += w + gap * 2;
            shelf = std::max( shelf, h );
        }

        aUsedHeight = y + shelf - aRegion.GetTop();
        return fits;
    };

    BOX2I region( VECTOR2I( area.GetLeft() + edgeMargin + ( leftBand ? leftBand + gap * 2 : 0 ),
                            area.GetTop() + edgeMargin ),
                  VECTOR2I( (int) area.GetWidth() - 2 * edgeMargin
                                    - ( leftBand ? leftBand + gap * 2 : 0 )
                                    - ( rightBand ? rightBand + gap * 2 : 0 ),
                            (int) area.GetHeight() - 2 * edgeMargin ) );

    int  used = 0;
    bool fits = (int) region.GetWidth() > 0 && pack( region, false, used );
    json result;

    // Only a plain rectangular outline (as made by set_board_outline) is resized; anything more
    // elaborate (cutouts, mounting holes on Edge.Cuts, curved edges) is left to the user.
    PCB_SHAPE* outlineRect = nullptr;
    int        edgeShapes = 0;

    for( BOARD_ITEM* item : brd->Drawings() )
    {
        if( item->GetLayer() == Edge_Cuts && item->Type() == PCB_SHAPE_T )
        {
            ++edgeShapes;

            if( static_cast<PCB_SHAPE*>( item )->GetShape() == SHAPE_T::RECTANGLE )
                outlineRect = static_cast<PCB_SHAPE*>( item );
        }
    }

    bool canGrow = outlineRect && edgeShapes == 1;

    if( !fits && grow && !canGrow )
    {
        result["outline_not_resized"] = "The board outline is not a single rectangle, so it was "
                                        "not enlarged; enlarge it manually or with "
                                        "set_board_outline (which replaces the whole outline).";
    }

    if( !fits && grow && canGrow )
    {
        // Widen to fit the widest cluster, then grow the height to what the packing needs.
        int widest = 0;

        for( const CLUSTER& cl : clusters )
            widest = std::max( widest, (int) cl.box.GetWidth() );

        if( (int) region.GetWidth() < widest )
            region.SetWidth( widest );

        region.SetHeight( 1000000000 );
        pack( region, false, used );
        region.SetHeight( std::max( used, std::max( leftY, rightY ) - area.GetTop() ) );

        // Never shrink the board.
        int newW = std::max( (int) area.GetWidth(),
                             region.GetRight() + edgeMargin + ( rightBand ? rightBand + gap * 2 : 0 )
                                     - area.GetLeft() );
        int newH = std::max( (int) area.GetHeight(), (int) region.GetHeight() + 2 * edgeMargin );

        region.SetHeight( newH - 2 * edgeMargin );

        commit.Modify( outlineRect );

        VECTOR2I origin( std::min( outlineRect->GetStart().x, outlineRect->GetEnd().x ),
                         std::min( outlineRect->GetStart().y, outlineRect->GetEnd().y ) );
        outlineRect->SetStart( origin );
        outlineRect->SetEnd( origin + VECTOR2I( newW, newH ) );

        // Right-edge connectors must follow the moved edge.
        int shift = area.GetLeft() + newW - area.GetRight();

        for( FOOTPRINT* fp : connectors )
        {
            if( fp->GetOrientation() == ANGLE_180 )
                fp->Move( VECTOR2I( shift, 0 ) );
        }

        area = BOX2I( area.GetOrigin(), VECTOR2I( newW, newH ) );
        result["board_resized_mm"] = { { "width", mm( newW ) }, { "height", mm( newH ) } };
        fits = true;
    }

    pack( region, true, used );

    commit.Push( _( "Copilot: arrange footprints" ) );

    // Show the result.
    m_frame->GetToolManager()->RunAction( ACTIONS::zoomFitScreen );

    json groupsJson = json::array();

    for( const CLUSTER& cl : clusters )
    {
        if( cl.members.size() < 2 )
            continue;

        json members = json::array();

        for( size_t i = 1; i < cl.members.size(); ++i )
            members.push_back( S( cl.members[i]->GetReference() ) );

        groupsJson.push_back( { { "anchor", S( cl.members[0]->GetReference() ) },
                                { "nearby", members } } );
    }

    result["groups"] = groupsJson;
    result["connectors"] = connectorPlacement;
    result["placed"] = (int) movable.size();
    result["fits"] = fits;

    if( !fits )
    {
        result["warning"] = "Parts do not fit inside the outline; enlarge it with "
                            "set_board_outline or pass grow_board=true.";
    }

    result["next"] = "Check with list_footprints / run_drc (courtyard overlaps) and adjust with "
                     "move_footprint. Then set design rules and autoroute.";

    return COPILOT_TOOL_RESULT::Ok( result, std::to_string( movable.size() ) + " parts placed in "
                                                    + std::to_string( clusters.size() )
                                                    + " groups" );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::addZone( const json& aIn )
{
    BOARD*        brd = board();
    std::string   netName = Str( aIn, "net", "GND" );
    NETINFO_ITEM* net = brd->FindNet( W( netName ) );

    if( !net || net->GetNetCode() <= 0 )
        return COPILOT_TOOL_RESULT::Error( "No net named '" + netName + "'." );

    BOX2I area = brd->GetBoardEdgesBoundingBox();

    if( (int) area.GetWidth() <= 0 )
        return COPILOT_TOOL_RESULT::Error( "There is no board outline; call set_board_outline "
                                           "first." );

    std::vector<std::string> layerNames = StrList( aIn, "layers" );

    if( layerNames.empty() )
        layerNames = { "F.Cu", "B.Cu" };

    LSET layers;

    for( const std::string& ln : layerNames )
    {
        PCB_LAYER_ID layer = brd->GetLayerID( W( ln ) );

        if( layer == UNDEFINED_LAYER || !IsCopperLayer( layer ) )
            return COPILOT_TOOL_RESULT::Error( "'" + ln + "' is not a copper layer." );

        layers.set( layer );
    }

    ZONE* zone = new ZONE( brd );
    brd->GetDesignSettings().GetDefaultZoneSettings().ExportSetting( *zone );
    zone->SetLayerSet( layers );
    zone->SetNetCode( net->GetNetCode() );
    zone->SetLocalClearance( toIU( Num( aIn, "clearance_mm" ).value_or( 0.3 ) ) );
    zone->SetMinThickness( toIU( Num( aIn, "min_width_mm" ).value_or( 0.25 ) ) );
    zone->SetAssignedPriority( Int( aIn, "priority", 0, 0, 100 ) );
    zone->SetPadConnection( ZONE_CONNECTION::THERMAL );
    zone->SetIsRuleArea( false );

    // The fill is clipped to the board outline by the copper-to-edge clearance.
    zone->Outline()->NewOutline();
    zone->Outline()->Append( area.GetLeft(), area.GetTop() );
    zone->Outline()->Append( area.GetRight(), area.GetTop() );
    zone->Outline()->Append( area.GetRight(), area.GetBottom() );
    zone->Outline()->Append( area.GetLeft(), area.GetBottom() );
    zone->SetBorderDisplayStyle( ZONE_BORDER_DISPLAY_STYLE::DIAGONAL_EDGE,
                                 ZONE::GetDefaultHatchPitch(), true );

    BOARD_COMMIT commit( m_frame );
    commit.Add( zone );
    commit.Push( _( "Copilot: add zone" ) );

    fillZones( json::object() );

    return COPILOT_TOOL_RESULT::Ok( { { "net", netName }, { "layers", layerNames },
                                      { "filled", true } },
                                    netName + " pour on " + std::to_string( layerNames.size() )
                                            + " layers" );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::fillZones( const json& aIn )
{
    ZONE_FILLER_TOOL* filler = m_frame->GetToolManager()->GetTool<ZONE_FILLER_TOOL>();

    if( !filler )
        return COPILOT_TOOL_RESULT::Error( "Zone filler unavailable." );

    filler->FillAllZones( m_frame );

    return COPILOT_TOOL_RESULT::Ok( { { "zones", (int) board()->Zones().size() } },
                                    std::to_string( board()->Zones().size() ) + " zones filled" );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::autoroute( const json& aIn )
{
    wxString java, how;
    wxString router = findFreerouting( java, how );

    if( router.IsEmpty() )
        return COPILOT_TOOL_RESULT::Error( S( how ) );

    int passes = Int( aIn, "passes", 20, 1, 200 );
    int timeout = Int( aIn, "timeout_s", 900, 30, 7200 );

    wxFileName dir( CopilotDataDir(), wxEmptyString );
    dir.AppendDir( wxS( "autoroute" ) );
    dir.Mkdir( wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );

    wxFileName dsn( dir.GetPath(), wxS( "board.dsn" ) );
    wxFileName ses( dir.GetPath(), wxS( "board.ses" ) );

    if( ses.FileExists() )
        wxRemoveFile( ses.GetFullPath() );

    int tracksBefore = (int) board()->Tracks().size();
    int unroutedBefore = 0;

    if( auto conn = board()->GetConnectivity() )
        unroutedBefore = (int) conn->GetUnconnectedCount( false );

    try
    {
        DSN::ExportBoardToSpecctraFile( board(), dsn.GetFullPath() );
    }
    catch( const IO_ERROR& e )
    {
        return COPILOT_TOOL_RESULT::Error( "Specctra export failed: " + S( e.What() ) );
    }

    std::vector<wxString> argv;

    if( !java.IsEmpty() )
        argv = { java, wxS( "-jar" ), router };
    else
        argv = { router };

    std::vector<wxString> routerArgs = { wxS( "-de" ), dsn.GetFullPath(), wxS( "-do" ),
                                         ses.GetFullPath(), wxS( "-mp" ),
                                         wxString::Format( wxS( "%d" ), passes ),
                                         wxS( "--gui.enabled=false" ) };

    argv.insert( argv.end(), routerArgs.begin(), routerArgs.end() );

    m_frame->SetStatusText( _( "Copilot: autorouting with Freerouting..." ) );

    COPILOT_PROCESS_RESULT res;
    {
        wxBusyCursor     busy;
        COPILOT_ACTIVITY activity( _( "Autorouting the board with Freerouting (this can take a "
                                      "few minutes)..." ) );
        res = CopilotRunProcess( argv, dir.GetPath(), timeout );
    }

    m_frame->SetStatusText( wxEmptyString );

    if( res.cancelled )
        return COPILOT_TOOL_RESULT::Error( "Autorouting was cancelled by the user." );

    std::string outTail = res.output.size() > 3000 ? res.output.substr( res.output.size() - 3000 )
                                                   : res.output;

    if( !res.started )
        return COPILOT_TOOL_RESULT::Error( "Could not start Freerouting (" + S( router ) + ")." );

    if( !ses.FileExists() && res.output.find( "UnsupportedClassVersionError" ) != std::string::npos )
    {
        return COPILOT_TOOL_RESULT::Error( "This Freerouting release needs a newer Java runtime "
                                           "than the one installed (" + S( java ) + "). Ask the "
                                           "user to install Java 25 or newer (e.g. Eclipse "
                                           "Temurin) and try again." );
    }

    if( !ses.FileExists() )
    {
        return COPILOT_TOOL_RESULT::Error( std::string( res.timedOut ? "Freerouting timed out"
                                                                     : "Freerouting failed" )
                                           + " without producing a session file. Output:\n"
                                           + outTail );
    }

    // Avoid dangling selection pointers when tracks are replaced by the import.
    m_frame->GetToolManager()->RunAction( ACTIONS::selectionClear );

    BOARD_COMMIT commit( m_frame );

    try
    {
        if( !DSN::ImportSpecctraSession( board(), ses.GetFullPath(), commit ) )
        {
            commit.Revert();
            return COPILOT_TOOL_RESULT::Error( "Importing the Freerouting session failed." );
        }
    }
    catch( const IO_ERROR& e )
    {
        commit.Revert();
        return COPILOT_TOOL_RESULT::Error( "Importing the Freerouting session failed: "
                                           + S( e.What() ) );
    }

    commit.Push( _( "Copilot: autoroute" ) );

    int unroutedAfter = 0;

    if( auto conn = board()->GetConnectivity() )
    {
        conn->RecalculateRatsnest();
        unroutedAfter = (int) conn->GetUnconnectedCount( false );
    }

    json j = { { "tracks_before", tracksBefore },
               { "tracks_after", (int) board()->Tracks().size() },
               { "unrouted_before", unroutedBefore },
               { "unrouted_after", unroutedAfter },
               { "timed_out", res.timedOut } };

    if( unroutedAfter > 0 )
    {
        j["hint"] = "Some connections are still unrouted: try more passes, a bigger board, "
                    "better placement, or 4 layers; ground connections will be completed by a "
                    "GND pour.";
    }

    return COPILOT_TOOL_RESULT::Ok( j, std::to_string( unroutedAfter ) + " unrouted after "
                                               "autorouting" );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::installFreerouting( const json& aIn )
{
    std::string body, type, error;
    std::string jarUrl, tag;

    wxBusyCursor     busy;
    COPILOT_ACTIVITY activity( _( "Downloading the Freerouting autorouter..." ) );

    // Preferred: ask the GitHub API for the latest release's .jar asset.
    if( CopilotHttpGet( "https://api.github.com/repos/freerouting/freerouting/releases/latest",
                        body, type, error ) )
    {
        json release = json::parse( body, nullptr, false );

        if( !release.is_discarded() && release.contains( "assets" ) )
        {
            tag = release.value( "tag_name", "" );

            for( const json& asset : release["assets"] )
            {
                std::string name = asset.value( "name", "" );

                if( name.size() > 4 && name.substr( name.size() - 4 ) == ".jar"
                    && name.find( "sources" ) == std::string::npos )
                {
                    jarUrl = asset.value( "browser_download_url", "" );
                    break;
                }
            }
        }
    }

    // Fallback when the API is unavailable (rate limits, proxies): a known-good release.
    if( jarUrl.empty() )
    {
        tag = "v2.4.1";
        jarUrl = "https://github.com/freerouting/freerouting/releases/download/v2.4.1/"
                 "freerouting-2.4.1.jar";
    }

    wxFileName target( CopilotDataDir(), wxS( "freerouting.jar" ) );

    if( !CopilotHttpDownload( jarUrl, target.GetFullPath(), error ) )
        return COPILOT_TOOL_RESULT::Error( error );

    wxString java = CopilotFindOnPath( wxS( "java" ) );
    json     j = { { "installed", S( target.GetFullPath() ) },
                   { "release", tag },
                   { "java_found", !java.IsEmpty() } };

    if( java.IsEmpty() )
    {
        j["next"] = "Java was not found. Ask the user to install Java 25 or newer (for example "
                    "Eclipse Temurin from adoptium.net) and restart KiCad.";
    }

    return COPILOT_TOOL_RESULT::Ok( j, "Freerouting " + tag );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::registerFootprintLibrary( const json& aIn )
{
    wxString nickname = W( Str( aIn, "nickname" ) );
    wxString path = W( Str( aIn, "path" ) );
    wxString error;

    if( nickname.IsEmpty() || path.IsEmpty() )
        return COPILOT_TOOL_RESULT::Error( "nickname and path are required." );

    if( CopilotIsRemotePath( path ) )
        return COPILOT_TOOL_RESULT::Error( "Network locations are not allowed." );

    if( !CopilotRegisterProjectLibrary( LIBRARY_TABLE_TYPE::FOOTPRINT, nickname, path, error,
                                        &m_frame->Kiway() ) )
    {
        return COPILOT_TOOL_RESULT::Error( S( error ) );
    }

    return COPILOT_TOOL_RESULT::Ok( { { "registered", S( nickname ) }, { "path", S( path ) } },
                                    S( nickname ) );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::saveBoard( const json& aIn )
{
    if( !m_frame->SaveBoard() )
        return COPILOT_TOOL_RESULT::Error( "Saving the board failed." );

    return COPILOT_TOOL_RESULT::Ok( { { "saved", S( board()->GetFileName() ) } }, "board saved" );
}
