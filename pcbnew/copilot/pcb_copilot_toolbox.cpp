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

#include <copilot/pcb_copilot_toolbox.h>
#include <copilot/copilot_common_tools.h>

#include <algorithm>
#include <cmath>
#include <regex>
#include <set>

#include <wx/filename.h>

#include <base_units.h>
#include <board.h>
#include <board_commit.h>
#include <board_design_settings.h>
#include <connectivity/connectivity_data.h>
#include <drc/drc_engine.h>
#include <drc/drc_item.h>
#include <footprint.h>
#include <footprint_library_adapter.h>
#include <ki_exception.h>
#include <netinfo.h>
#include <netclass.h>
#include <pad.h>
#include <pcb_draw_panel_gal.h>
#include <pcb_edit_frame.h>
#include <pcb_field.h>
#include <pcb_marker.h>
#include <pcb_text.h>
#include <pcb_track.h>
#include <pcbnew_settings.h>
#include <project/net_settings.h>
#include <project_pcb.h>
#include <ratsnest/ratsnest_data.h>
#include <string_utils.h>
#include <tool/actions.h>
#include <tool/tool_manager.h>
#include <tools/drc_tool.h>
#include <tools/edit_tool.h>
#include <router/router_tool.h>
#include <tools/zone_filler_tool.h>
#include <tools/pcb_actions.h>
#include <tools/pcb_selection_tool.h>
#include <zone.h>


using json = nlohmann::json;


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
    return std::round( pcbIUScale.IUTomm( (int) aIU ) * 10000.0 ) / 10000.0;
}


int toIU( double aMM )
{
    // Keep model supplied values inside the board/sheet coordinate range.
    if( !std::isfinite( aMM ) )
        aMM = 0.0;

    return pcbIUScale.mmToIU( std::clamp( aMM, -1500.0, 1500.0 ) );
}


std::string strArg( const json& aIn, const char* aKey, const std::string& aDefault = "" )
{
    if( aIn.contains( aKey ) && aIn[aKey].is_string() )
        return aIn[aKey].get<std::string>();

    return aDefault;
}


std::optional<double> numArg( const json& aIn, const char* aKey )
{
    // Rejects non-finite values and accepts quoted numbers.
    return COPILOT_ARGS::Num( aIn, aKey );
}


int intArg( const json& aIn, const char* aKey, int aDefault, int aMin, int aMax )
{
    return COPILOT_ARGS::Int( aIn, aKey, aDefault, aMin, aMax );
}


std::optional<bool> boolArg( const json& aIn, const char* aKey )
{
    if( aIn.contains( aKey ) && aIn[aKey].is_boolean() )
        return aIn[aKey].get<bool>();

    return std::nullopt;
}


/// Case-insensitive match; supports * and ? wildcards, otherwise substring.
bool matches( const wxString& aText, const wxString& aFilter )
{
    if( aFilter.IsEmpty() )
        return true;

    wxString text = aText.Lower();
    wxString filter = aFilter.Lower();

    if( filter.Contains( wxS( "*" ) ) || filter.Contains( wxS( "?" ) ) )
        return text.Matches( filter );

    return text.Contains( filter );
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


bool isGroundNet( const wxString& aName )
{
    static const std::regex re( R"(^/?(.*[/_-])?(gnd|agnd|dgnd|pgnd|sgnd|gnda|gndd|gndpwr|vss|vssa|0v|earth|chassis)[a-z0-9_]*$)",
                                std::regex::icase );
    return std::regex_match( S( aName ), re );
}


bool isPowerNet( const wxString& aName )
{
    if( isGroundNet( aName ) )
        return false;

    static const std::regex re( R"(^/?(.*[/_-])?(\+?[0-9]+v[0-9]*|\+?[0-9]+v[0-9]+|v?[0-9]+v[0-9]+|\+[a-z0-9_.]+|vcc[a-z0-9_]*|vdd[a-z0-9_]*|vbus|vbat|vin|vsys|vref[a-z0-9_]*|avcc|avdd|dvdd|vio|vddio|v_[a-z0-9_]+|pwr[a-z0-9_]*)$)",
                                std::regex::icase );
    return std::regex_match( S( aName ), re );
}


const char* padTypeName( PAD_ATTRIB aAttr )
{
    switch( aAttr )
    {
    case PAD_ATTRIB::PTH:  return "pth";
    case PAD_ATTRIB::SMD:  return "smd";
    case PAD_ATTRIB::CONN: return "connector";
    case PAD_ATTRIB::NPTH: return "npth";
    default:               return "other";
    }
}


json schemaString( const std::string& aDescription )
{
    return { { "type", "string" }, { "description", aDescription } };
}


json schemaNumber( const std::string& aDescription )
{
    return { { "type", "number" }, { "description", aDescription } };
}


json schemaInt( const std::string& aDescription )
{
    return { { "type", "integer" }, { "description", aDescription } };
}


json schemaBool( const std::string& aDescription )
{
    return { { "type", "boolean" }, { "description", aDescription } };
}

} // namespace


PCB_COPILOT_TOOLBOX::PCB_COPILOT_TOOLBOX( PCB_EDIT_FRAME* aFrame ) :
        m_frame( aFrame )
{
    registerTools();
    registerLayoutTools();
}


BOARD* PCB_COPILOT_TOOLBOX::board() const
{
    return m_frame ? m_frame->GetBoard() : nullptr;
}


FOOTPRINT* PCB_COPILOT_TOOLBOX::findFootprint( const std::string& aRef ) const
{
    if( !board() || aRef.empty() )
        return nullptr;

    wxString ref = W( aRef );

    for( FOOTPRINT* fp : board()->Footprints() )
    {
        if( fp->GetReference().CmpNoCase( ref ) == 0 )
            return fp;
    }

    return nullptr;
}


void PCB_COPILOT_TOOLBOX::registerTools()
{
    auto bind = [this]( COPILOT_TOOL_RESULT ( PCB_COPILOT_TOOLBOX::*aFn )( const json& ) )
    {
        return [this, aFn]( const json& aIn ) { return ( this->*aFn )( aIn ); };
    };

    // ---- Read-only inspection -----------------------------------------------------------

    add( "get_board_summary",
         "Overview of the open board: file, size, layer stack, item counts, net classes, "
         "unrouted connection count and existing DRC marker count. Call this first.",
         json::object(), {}, READ_ONLY, bind( &PCB_COPILOT_TOOLBOX::getBoardSummary ) );

    add( "get_design_rules",
         "Board-level design constraints (minimum clearance, track width, via size, drill, "
         "edge clearance) and every net class with its clearance/track/via settings.",
         json::object(), {}, READ_ONLY, bind( &PCB_COPILOT_TOOLBOX::getDesignRules ) );

    add( "list_footprints",
         "List footprints (components) on the board with reference, value, library "
         "footprint, position (mm), rotation, side and key fields.",
         { { "filter", schemaString( "Optional case-insensitive filter. A plain word matches "
                                     "reference, value or footprint name as a substring (e.g. "
                                     "'100n', 'LQFP'); a pattern with * or ? wildcards matches "
                                     "reference or footprint name (e.g. 'C*', 'U?', '*0402*')." ) },
           { "limit", schemaInt( "Maximum results (default 200, max 1000)." ) } },
         {}, READ_ONLY, bind( &PCB_COPILOT_TOOLBOX::listFootprints ) );

    add( "get_footprint",
         "Full details of one footprint including every pad with its net and pin function.",
         { { "reference", schemaString( "Reference designator, e.g. U3." ) } },
         { "reference" }, READ_ONLY, bind( &PCB_COPILOT_TOOLBOX::getFootprint ) );

    add( "list_nets",
         "List nets with net class, pad count, routed track length, via count and number of "
         "unrouted connections.",
         { { "filter", schemaString( "Optional case-insensitive filter on the net name "
                                     "(wildcards allowed)." ) },
           { "limit", schemaInt( "Maximum results (default 300, max 2000)." ) } },
         {}, READ_ONLY, bind( &PCB_COPILOT_TOOLBOX::listNets ) );

    add( "get_net",
         "Details of one net: every connected pad (REF.pad and pin function), track length "
         "and widths per layer, vias, zones and unrouted connections.",
         { { "name", schemaString( "Exact net name, e.g. /MCU/SDA or GND." ) } },
         { "name" }, READ_ONLY, bind( &PCB_COPILOT_TOOLBOX::getNet ) );

    add( "get_selection",
         "Describe the items the user currently has selected in the editor.",
         json::object(), {}, READ_ONLY, bind( &PCB_COPILOT_TOOLBOX::getSelection ) );

    add( "run_drc",
         "Run the design rule checker on the current board (it does not add markers) and "
         "return the violations grouped by type. Zones are only refilled first if "
         "refill_zones is true; after moving parts or tracks, refill (or call fill_zones) "
         "so zone results are current.",
         { { "limit", schemaInt( "Maximum violations to list in detail (default 60)." ) },
           { "include_warnings", schemaBool( "Include warnings as well as errors "
                                             "(default true)." ) },
           { "refill_zones", schemaBool( "Refill all zones before checking (default false; "
                                         "changes the zone fills, so it needs edits "
                                         "allowed)." ) } },
         {}, READ_ONLY, bind( &PCB_COPILOT_TOOLBOX::runDrc ) );

    add( "get_review_data",
         "Heuristic design-review checks that DRC does not cover: ICs without a nearby "
         "decoupling capacitor on each power pin, single-pad nets, power nets routed with "
         "thin tracks, tracks narrower than their net class, footprints outside the board "
         "outline, missing values and BOM fields (MPN/manufacturer), DNP parts.",
         { { "decoupling_max_mm", schemaNumber( "Maximum capacitor distance from an IC power "
                                                 "pad to count as decoupling (default 5)." ) },
           { "power_track_min_mm", schemaNumber( "Flag power-net tracks narrower than this "
                                                  "(default 0.3)." ) } },
         {}, READ_ONLY, bind( &PCB_COPILOT_TOOLBOX::getReviewData ) );

    add( "search_footprint_libraries",
         "Search the installed KiCad footprint libraries by name. Every word in the query "
         "must match the 'Library:Footprint' id (case-insensitive). Use for part/footprint "
         "suggestions, e.g. 'SOT-23 5', 'QFN 32 5x5', 'C_0603', 'USB C Receptacle'.",
         { { "query", schemaString( "Space separated search words." ) },
           { "library", schemaString( "Optional library nickname to restrict the search." ) },
           { "limit", schemaInt( "Maximum results (default 40, max 200)." ) } },
         { "query" }, READ_ONLY, bind( &PCB_COPILOT_TOOLBOX::searchFootprintLibraries ) );

    add( "get_library_footprint",
         "Load a footprint from the libraries and describe it: description, keywords, pad "
         "count and types, courtyard/body size.",
         { { "lib_id", schemaString( "Library identifier 'Library:Footprint', e.g. "
                                     "Capacitor_SMD:C_0603_1608Metric." ) } },
         { "lib_id" }, READ_ONLY, bind( &PCB_COPILOT_TOOLBOX::getLibraryFootprint ) );

    // ---- View (does not modify the design) ---------------------------------------------

    add( "select_items",
         "Select footprints and/or all copper items of nets in the editor and zoom to them, "
         "so the user can see what you are talking about.",
         { { "references", { { "type", "array" }, { "items", { { "type", "string" } } },
                             { "description", "Reference designators to select." } } },
           { "nets", { { "type", "array" }, { "items", { { "type", "string" } } },
                       { "description", "Net names whose tracks, vias and pads to select." } } },
           { "zoom", schemaBool( "Zoom to fit the selection (default true)." ) } },
         {}, READ_ONLY, bind( &PCB_COPILOT_TOOLBOX::selectItems ) );

    add( "highlight_net",
         "Highlight a net on the canvas. Pass an empty name to clear highlighting.",
         { { "name", schemaString( "Net name, or empty to clear." ) } },
         {}, READ_ONLY, bind( &PCB_COPILOT_TOOLBOX::highlightNet ) );

    // ---- Editing (each call is one undoable commit) --------------------------------------

    add( "move_footprint",
         "Move, rotate and/or flip a footprint. Give absolute x_mm/y_mm, or relative "
         "dx_mm/dy_mm. Coordinates are board coordinates in mm (+Y is down).",
         { { "reference", schemaString( "Reference designator." ) },
           { "x_mm", schemaNumber( "Absolute X position." ) },
           { "y_mm", schemaNumber( "Absolute Y position." ) },
           { "dx_mm", schemaNumber( "Relative X offset." ) },
           { "dy_mm", schemaNumber( "Relative Y offset." ) },
           { "rotation_deg", schemaNumber( "Absolute orientation in degrees." ) },
           { "side", { { "type", "string" }, { "enum", { "front", "back" } },
                       { "description", "Board side to place the footprint on." } } } },
         { "reference" }, MODIFIES, bind( &PCB_COPILOT_TOOLBOX::moveFootprint ) );

    add( "set_footprint_field",
         "Set a footprint field such as Value, MPN, Manufacturer or any custom field "
         "(created hidden if missing). Note that the schematic is normally the source of "
         "truth for values; mention that the change should be back-annotated.",
         { { "reference", schemaString( "Reference designator." ) },
           { "field", schemaString( "Field name, e.g. Value, MPN, Manufacturer." ) },
           { "value", schemaString( "New field text." ) } },
         { "reference", "field", "value" }, MODIFIES,
         bind( &PCB_COPILOT_TOOLBOX::setFootprintField ) );

    add( "set_footprint_locked",
         "Lock or unlock a footprint.",
         { { "reference", schemaString( "Reference designator." ) },
           { "locked", schemaBool( "True to lock." ) } },
         { "reference", "locked" }, MODIFIES, bind( &PCB_COPILOT_TOOLBOX::setFootprintLocked ) );

    add( "set_track_width",
         "Change the width of every track segment and arc on a net (optionally on one layer "
         "only).",
         { { "net", schemaString( "Net name." ) },
           { "width_mm", schemaNumber( "New track width in mm." ) },
           { "layer", schemaString( "Optional copper layer name, e.g. F.Cu." ) } },
         { "net", "width_mm" }, MODIFIES, bind( &PCB_COPILOT_TOOLBOX::setTrackWidth ) );

    add( "add_text",
         "Add a text item to the board (e.g. a silkscreen label or a fabrication note).",
         { { "text", schemaString( "Text to add." ) },
           { "x_mm", schemaNumber( "X position." ) },
           { "y_mm", schemaNumber( "Y position." ) },
           { "layer", schemaString( "Layer name (default F.SilkS)." ) },
           { "size_mm", schemaNumber( "Text height in mm (default: board default for "
                                      "the layer)." ) } },
         { "text", "x_mm", "y_mm" }, MODIFIES, bind( &PCB_COPILOT_TOOLBOX::addText ) );
}


std::optional<COPILOT_TOOL_RESULT> PCB_COPILOT_TOOLBOX::preExecute( const std::string& aName )
{
    if( !board() )
        return COPILOT_TOOL_RESULT::Error( "No board is open." );

    return std::nullopt;
}


bool PCB_COPILOT_TOOLBOX::IsReadyForTools( std::string& aWhy )
{
    if( !COPILOT_TOOLBOX::IsReadyForTools( aWhy ) )
        return false;

    if( !m_frame )
        return true;

    TOOL_MANAGER* mgr = m_frame->GetToolManager();
    bool          busy = !m_frame->ToolStackIsEmpty();

    if( mgr )
    {
        busy |= mgr->IsContextMenuActive();

        if( EDIT_TOOL* edit = mgr->GetTool<EDIT_TOOL>() )
            busy |= edit->IsToolActive();       // interactive move / drag

        if( ROUTER_TOOL* router = mgr->GetTool<ROUTER_TOOL>() )
            busy |= router->IsToolActive();     // routing, interactive drag

        if( DRC_TOOL* drc = mgr->GetTool<DRC_TOOL>() )
            busy |= drc->IsDRCRunning();

        if( ZONE_FILLER_TOOL* filler = mgr->GetTool<ZONE_FILLER_TOOL>() )
            busy |= filler->IsBusy();
    }

    if( busy )
    {
        aWhy = "Waiting for you to finish the current action in the PCB editor (press Esc to "
               "cancel it)...";
        return false;
    }

    return true;
}


std::string PCB_COPILOT_TOOLBOX::GetSystemPrompt()
{
    return "You are working in the KiCad PCB Editor. The board is the physical layout; "
           "the schematic (edited separately) is normally the source of truth for components, "
           "values and connectivity, so recommend schematic changes where appropriate and "
           "remind the user to update the PCB from the schematic.\n"
           "Coordinates are board coordinates in millimetres with +Y pointing down. Layer names "
           "use KiCad's canonical names (F.Cu, B.Cu, In1.Cu, F.SilkS, F.Fab, Edge.Cuts...).\n"
           "For a design review: call get_board_summary, run_drc and get_review_data, then "
           "inspect specific footprints/nets as needed. Group findings by severity and give "
           "concrete fixes. When you mention specific parts or nets, you may call select_items "
           "so the user can see them.\n"
           "For part suggestions: base them on the circuit around the part (get_footprint and "
           "get_net show what it connects to), propose values and ratings, and use "
           "search_footprint_libraries to name real KiCad footprints.\n"
           "To lay out a board from its schematic: update_from_schematic, set_board_outline, "
           "set_design_rules (board-house limits; optionally a Power net class), "
           "arrange_footprints, then refine placement with move_footprint (connectors at edges, "
           "crystals and decoupling close to their ICs), autoroute, add_zone for GND on both "
           "layers, run_drc, fix issues, and save_board.\n";
}


std::string PCB_COPILOT_TOOLBOX::GetContextSnapshot()
{
    BOARD* brd = board();

    if( !brd )
        return "No board is open.";

    json ctx;
    wxFileName fn( brd->GetFileName() );
    ctx["board_file"] = S( fn.GetFullName() );
    ctx["unsaved_changes"] = m_frame->IsContentModified();

    PCB_SELECTION_TOOL* selTool = m_frame->GetToolManager()->GetTool<PCB_SELECTION_TOOL>();

    if( selTool )
    {
        json sel = json::array();
        int  count = 0;

        for( EDA_ITEM* item : selTool->GetSelection() )
        {
            if( ++count > 15 )
                break;

            sel.push_back( S( item->GetItemDescription( m_frame, false ) ) );
        }

        if( !sel.empty() )
        {
            ctx["selection"] = sel;
            ctx["selection_count"] = (int) selTool->GetSelection().GetSize();
        }
    }

    return ctx.dump();
}


std::vector<std::pair<std::string, std::string>> PCB_COPILOT_TOOLBOX::GetQuickActions()
{
    return {
        { S( _( "Review design" ) ),
          "Review this board for design problems. Run DRC and the review checks, then give me "
          "a prioritised list of issues (problems, warnings, suggestions) with concrete fixes." },
        { S( _( "Explain selection" ) ),
          "Explain what the currently selected items are, what they connect to and what "
          "their role in the circuit probably is." },
        { S( _( "Check BOM" ) ),
          "Check the components for BOM readiness: missing values, missing manufacturer part "
          "numbers, inconsistent footprints for the same value, and DNP parts." },
        { S( _( "Suggest parts" ) ),
          "For the selected component (or the most important ICs if nothing is selected), "
          "suggest suitable parts with values, ratings and matching KiCad footprints." }
    };
}


// ---------------------------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------------------------

json PCB_COPILOT_TOOLBOX::footprintJson( const FOOTPRINT* aFp, bool aDetailed ) const
{
    json j;
    j["reference"] = S( aFp->GetReference() );
    j["value"] = S( aFp->GetValue() );
    j["footprint"] = S( aFp->GetFPID().GetUniStringLibId() );
    j["x_mm"] = mm( aFp->GetPosition().x );
    j["y_mm"] = mm( aFp->GetPosition().y );
    j["rotation_deg"] = std::round( aFp->GetOrientationDegrees() * 100.0 ) / 100.0;
    j["side"] = aFp->GetLayer() == B_Cu ? "back" : "front";
    j["pads"] = (int) aFp->Pads().size();

    if( aFp->IsLocked() )
        j["locked"] = true;

    if( aFp->IsDNP() )
        j["dnp"] = true;

    if( aFp->IsExcludedFromBOM() )
        j["exclude_from_bom"] = true;

    json fields = json::object();

    for( const PCB_FIELD* field : aFp->GetFields() )
    {
        if( !field || field->IsReference() || field->IsValue() )
            continue;

        if( field->GetText().IsEmpty() || field->GetText() == wxS( "~" ) )
            continue;

        if( !aDetailed && field->IsDatasheet() )
            continue;

        fields[S( field->GetName() )] = S( field->GetText() );
    }

    if( !fields.empty() )
        j["fields"] = fields;

    if( aDetailed )
    {
        j["description"] = S( aFp->GetLibDescription() );
        j["keywords"] = S( aFp->GetKeywords() );

        BOX2I bbox = aFp->GetBoundingBox( false );
        j["bbox_mm"] = { { "x", mm( bbox.GetX() ) }, { "y", mm( bbox.GetY() ) },
                         { "w", mm( bbox.GetWidth() ) }, { "h", mm( bbox.GetHeight() ) } };

        json pads = json::array();

        for( const PAD* pad : aFp->Pads() )
        {
            json p;
            p["number"] = S( pad->GetNumber() );
            p["net"] = S( pad->GetNetname() );
            p["type"] = padTypeName( pad->GetAttribute() );

            if( !pad->GetPinFunction().IsEmpty() )
                p["pin_function"] = S( pad->GetPinFunction() );

            if( !pad->GetPinType().IsEmpty() )
                p["pin_type"] = S( pad->GetPinType() );

            p["x_mm"] = mm( pad->GetPosition().x );
            p["y_mm"] = mm( pad->GetPosition().y );
            pads.push_back( p );
        }

        j["pad_list"] = pads;
    }

    return j;
}


// ---------------------------------------------------------------------------------------------
// Read-only tools
// ---------------------------------------------------------------------------------------------

COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::getBoardSummary( const json& aIn )
{
    BOARD*                 brd = board();
    BOARD_DESIGN_SETTINGS& bds = brd->GetDesignSettings();

    json j;
    j["file"] = S( wxFileName( brd->GetFileName() ).GetFullName() );
    j["copper_layers"] = brd->GetCopperLayerCount();
    j["board_thickness_mm"] = mm( bds.GetBoardThickness() );

    BOX2I edges = brd->GetBoardEdgesBoundingBox();

    if( edges.GetWidth() > 0 && edges.GetHeight() > 0 )
    {
        j["outline_bbox_mm"] = { { "x", mm( edges.GetX() ) }, { "y", mm( edges.GetY() ) },
                                 { "w", mm( edges.GetWidth() ) }, { "h", mm( edges.GetHeight() ) } };
    }
    else
    {
        j["outline_bbox_mm"] = "no board outline on Edge.Cuts";
    }

    json layers = json::array();

    for( PCB_LAYER_ID layer : brd->GetEnabledLayers().CuStack() )
        layers.push_back( S( brd->GetLayerName( layer ) ) );

    j["copper_layer_names"] = layers;

    int tracks = 0, arcs = 0, vias = 0;

    for( PCB_TRACK* t : brd->Tracks() )
    {
        switch( t->Type() )
        {
        case PCB_TRACE_T: ++tracks; break;
        case PCB_ARC_T:   ++arcs; break;
        case PCB_VIA_T:   ++vias; break;
        default: break;
        }
    }

    int smd = 0, tht = 0, pads = 0;
    std::map<wxString, int> prefixes;

    for( FOOTPRINT* fp : brd->Footprints() )
    {
        pads += (int) fp->Pads().size();
        prefixes[refPrefix( fp->GetReference() )]++;

        if( fp->GetAttributes() & FP_SMD )
            ++smd;
        else if( fp->GetAttributes() & FP_THROUGH_HOLE )
            ++tht;
    }

    json counts = { { "footprints", (int) brd->Footprints().size() },
                    { "smd_footprints", smd },
                    { "through_hole_footprints", tht },
                    { "pads", pads },
                    { "track_segments", tracks },
                    { "arcs", arcs },
                    { "vias", vias },
                    { "zones", (int) brd->Zones().size() },
                    { "nets", (int) brd->GetNetCount() > 0 ? (int) brd->GetNetCount() - 1 : 0 } };
    j["counts"] = counts;

    json byPrefix = json::object();

    for( const auto& [prefix, count] : prefixes )
        byPrefix[prefix.IsEmpty() ? "?" : S( prefix )] = count;

    j["footprints_by_ref_prefix"] = byPrefix;

    json classes = json::array( { "Default" } );

    for( const auto& [name, nc] : bds.m_NetSettings->GetNetclasses() )
        classes.push_back( S( name ) );

    j["net_classes"] = classes;

    if( std::shared_ptr<CONNECTIVITY_DATA> conn = brd->GetConnectivity() )
        j["unrouted_connections"] = (int) conn->GetUnconnectedCount( false );

    int errors = 0, warnings = 0;

    for( PCB_MARKER* marker : brd->Markers() )
    {
        if( marker->IsExcluded() )
            continue;

        if( marker->GetSeverity() == RPT_SEVERITY_ERROR )
            ++errors;
        else if( marker->GetSeverity() == RPT_SEVERITY_WARNING )
            ++warnings;
    }

    j["existing_drc_markers"] = { { "errors", errors }, { "warnings", warnings } };

    return COPILOT_TOOL_RESULT::Ok( j, std::to_string( brd->Footprints().size() ) + " footprints, "
                                               + std::to_string( brd->GetCopperLayerCount() )
                                               + " copper layers" );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::getDesignRules( const json& aIn )
{
    BOARD_DESIGN_SETTINGS& bds = board()->GetDesignSettings();

    json j;
    j["min_clearance_mm"] = mm( bds.m_MinClearance );
    j["min_track_width_mm"] = mm( bds.m_TrackMinWidth );
    j["min_via_diameter_mm"] = mm( bds.m_ViasMinSize );
    j["min_via_annular_ring_mm"] = mm( bds.m_ViasMinAnnularWidth );
    j["min_through_drill_mm"] = mm( bds.m_MinThroughDrill );
    j["min_microvia_diameter_mm"] = mm( bds.m_MicroViasMinSize );
    j["copper_to_edge_clearance_mm"] = mm( bds.m_CopperEdgeClearance );
    j["hole_clearance_mm"] = mm( bds.m_HoleClearance );

    wxFileName rules( board()->GetDesignRulesPath() );
    j["custom_rules_file"] = rules.FileExists() ? S( rules.GetFullName() ) : "none";

    json classes = json::array();

    auto ncJson = [&]( const NETCLASS* nc )
    {
        json c;
        c["name"] = S( nc->GetName() );

        if( nc->HasClearance() )
            c["clearance_mm"] = mm( nc->GetClearance() );

        if( nc->HasTrackWidth() )
            c["track_width_mm"] = mm( nc->GetTrackWidth() );

        if( nc->HasViaDiameter() )
            c["via_diameter_mm"] = mm( nc->GetViaDiameter() );

        if( nc->HasViaDrill() )
            c["via_drill_mm"] = mm( nc->GetViaDrill() );

        if( nc->HasDiffPairWidth() )
            c["diff_pair_width_mm"] = mm( nc->GetDiffPairWidth() );

        if( nc->HasDiffPairGap() )
            c["diff_pair_gap_mm"] = mm( nc->GetDiffPairGap() );

        return c;
    };

    classes.push_back( ncJson( bds.m_NetSettings->GetDefaultNetclass().get() ) );

    for( const auto& [name, nc] : bds.m_NetSettings->GetNetclasses() )
        classes.push_back( ncJson( nc.get() ) );

    j["net_classes"] = classes;

    return COPILOT_TOOL_RESULT::Ok( j, std::to_string( classes.size() ) + " net classes" );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::listFootprints( const json& aIn )
{
    wxString filter = W( strArg( aIn, "filter" ) );
    int      limit = intArg( aIn, "limit", 200, 1, 1000 );

    std::vector<FOOTPRINT*> fps( board()->Footprints().begin(), board()->Footprints().end() );

    std::sort( fps.begin(), fps.end(),
               []( FOOTPRINT* a, FOOTPRINT* b )
               {
                   return StrNumCmp( a->GetReference(), b->GetReference(), true ) < 0;
               } );

    json list = json::array();
    int  total = 0;

    // Wildcard patterns ('C*', 'U?', '*0402*') are meant for references and footprint names;
    // plain words also search values.
    bool wildcard = filter.Contains( wxS( "*" ) ) || filter.Contains( wxS( "?" ) );

    for( FOOTPRINT* fp : fps )
    {
        if( !matches( fp->GetReference(), filter )
            && !matches( fp->GetFPID().GetUniStringLibItemName(), filter )
            && ( wildcard || !matches( fp->GetValue(), filter ) ) )
        {
            continue;
        }

        ++total;

        if( (int) list.size() < limit )
            list.push_back( footprintJson( fp, false ) );
    }

    json j = { { "total_matches", total }, { "footprints", list } };

    if( total > limit )
        j["note"] = "Results truncated; refine the filter or raise the limit.";

    return COPILOT_TOOL_RESULT::Ok( j, std::to_string( total ) + " footprints" );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::getFootprint( const json& aIn )
{
    std::string ref = strArg( aIn, "reference" );
    FOOTPRINT*  fp = findFootprint( ref );

    if( !fp )
        return COPILOT_TOOL_RESULT::Error( "No footprint with reference '" + ref + "'." );

    return COPILOT_TOOL_RESULT::Ok( footprintJson( fp, true ), S( fp->GetReference() ) );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::listNets( const json& aIn )
{
    BOARD*   brd = board();
    wxString filter = W( strArg( aIn, "filter" ) );
    int      limit = intArg( aIn, "limit", 300, 1, 2000 );

    std::map<int, int>    padCount;
    std::map<int, double> length;
    std::map<int, int>    viaCount;

    for( FOOTPRINT* fp : brd->Footprints() )
    {
        for( PAD* pad : fp->Pads() )
            padCount[pad->GetNetCode()]++;
    }

    for( PCB_TRACK* t : brd->Tracks() )
    {
        if( t->Type() == PCB_VIA_T )
            viaCount[t->GetNetCode()]++;
        else
            length[t->GetNetCode()] += t->GetLength();
    }

    std::shared_ptr<CONNECTIVITY_DATA> conn = brd->GetConnectivity();

    std::vector<NETINFO_ITEM*> nets;

    for( NETINFO_ITEM* net : brd->GetNetInfo() )
    {
        if( net->GetNetCode() > 0 && matches( net->GetNetname(), filter ) )
            nets.push_back( net );
    }

    std::sort( nets.begin(), nets.end(),
               []( NETINFO_ITEM* a, NETINFO_ITEM* b )
               {
                   return StrNumCmp( a->GetNetname(), b->GetNetname(), true ) < 0;
               } );

    json list = json::array();

    for( NETINFO_ITEM* net : nets )
    {
        if( (int) list.size() >= limit )
            break;

        int  code = net->GetNetCode();
        json n;
        n["name"] = S( net->GetNetname() );
        n["class"] = net->GetNetClass() ? S( net->GetNetClass()->GetName() ) : "Default";
        n["pads"] = padCount[code];

        if( length[code] > 0 )
            n["track_length_mm"] = mm( (long long) length[code] );

        if( viaCount[code] )
            n["vias"] = viaCount[code];

        if( conn )
        {
            if( RN_NET* rn = conn->GetRatsnestForNet( code ) )
            {
                if( !rn->GetEdges().empty() )
                    n["unrouted"] = (int) rn->GetEdges().size();
            }
        }

        list.push_back( n );
    }

    json j = { { "total_matches", (int) nets.size() }, { "nets", list } };

    if( (int) nets.size() > limit )
        j["note"] = "Results truncated; refine the filter or raise the limit.";

    return COPILOT_TOOL_RESULT::Ok( j, std::to_string( nets.size() ) + " nets" );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::getNet( const json& aIn )
{
    BOARD*        brd = board();
    std::string   name = strArg( aIn, "name" );
    NETINFO_ITEM* net = brd->FindNet( W( name ) );

    if( !net || net->GetNetCode() <= 0 )
        return COPILOT_TOOL_RESULT::Error( "No net named '" + name + "'. Use list_nets to find "
                                           "the exact name." );

    int  code = net->GetNetCode();
    json j;
    j["name"] = S( net->GetNetname() );
    j["class"] = net->GetNetClass() ? S( net->GetNetClass()->GetName() ) : "Default";

    json pads = json::array();

    for( FOOTPRINT* fp : brd->Footprints() )
    {
        for( PAD* pad : fp->Pads() )
        {
            if( pad->GetNetCode() != code )
                continue;

            std::string entry = S( fp->GetReference() ) + "." + S( pad->GetNumber() );

            if( !pad->GetPinFunction().IsEmpty() )
                entry += " (" + S( pad->GetPinFunction() ) + ")";

            entry += " [" + S( fp->GetValue() ) + "]";

            if( pads.size() < 300 )
                pads.push_back( entry );
        }
    }

    j["pads"] = pads;

    std::map<wxString, double>        lengthByLayer;
    std::map<wxString, std::set<int>> widthsByLayer;
    int                               vias = 0;

    for( PCB_TRACK* t : brd->Tracks() )
    {
        if( t->GetNetCode() != code )
            continue;

        if( t->Type() == PCB_VIA_T )
        {
            ++vias;
            continue;
        }

        wxString layer = brd->GetLayerName( t->GetLayer() );
        lengthByLayer[layer] += t->GetLength();
        widthsByLayer[layer].insert( t->GetWidth() );
    }

    json routing = json::object();

    for( const auto& [layer, len] : lengthByLayer )
    {
        json widths = json::array();

        for( int w : widthsByLayer[layer] )
            widths.push_back( mm( w ) );

        routing[S( layer )] = { { "length_mm", mm( (long long) len ) }, { "widths_mm", widths } };
    }

    j["routing"] = routing;
    j["vias"] = vias;

    json zones = json::array();

    for( ZONE* zone : brd->Zones() )
    {
        if( zone->GetNetCode() != code )
            continue;

        wxString layers;

        for( PCB_LAYER_ID layer : zone->GetLayerSet().Seq() )
            layers += ( layers.IsEmpty() ? wxString() : wxString( wxS( "," ) ) )
                      + brd->GetLayerName( layer );

        zones.push_back( S( ( zone->GetZoneName().IsEmpty() ? wxString( wxS( "zone" ) )
                                                            : zone->GetZoneName() )
                            + wxS( " on " ) + layers ) );
    }

    if( !zones.empty() )
        j["zones"] = zones;

    if( std::shared_ptr<CONNECTIVITY_DATA> conn = brd->GetConnectivity() )
    {
        if( RN_NET* rn = conn->GetRatsnestForNet( code ) )
            j["unrouted_connections"] = (int) rn->GetEdges().size();
    }

    return COPILOT_TOOL_RESULT::Ok( j, name + ": " + std::to_string( pads.size() ) + " pads" );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::getSelection( const json& aIn )
{
    PCB_SELECTION_TOOL* selTool = m_frame->GetToolManager()->GetTool<PCB_SELECTION_TOOL>();

    if( !selTool )
        return COPILOT_TOOL_RESULT::Error( "Selection tool unavailable." );

    json items = json::array();

    for( EDA_ITEM* item : selTool->GetSelection() )
    {
        if( items.size() >= 100 )
            break;

        if( item->Type() == PCB_FOOTPRINT_T )
        {
            json fp = footprintJson( static_cast<FOOTPRINT*>( item ), false );
            fp["kind"] = "footprint";
            items.push_back( fp );
        }
        else
        {
            json it;
            it["kind"] = S( item->GetFriendlyName() );
            it["description"] = S( item->GetItemDescription( m_frame, true ) );

            if( BOARD_CONNECTED_ITEM* bci = dynamic_cast<BOARD_CONNECTED_ITEM*>( item ) )
            {
                if( bci->GetNetCode() > 0 )
                    it["net"] = S( bci->GetNetname() );
            }

            if( item->Type() == PCB_PAD_T )
            {
                PAD* pad = static_cast<PAD*>( item );

                if( pad->GetParentFootprint() )
                    it["footprint"] = S( pad->GetParentFootprint()->GetReference() );

                it["pad"] = S( pad->GetNumber() );
            }

            items.push_back( it );
        }
    }

    json j = { { "count", (int) selTool->GetSelection().GetSize() }, { "items", items } };
    return COPILOT_TOOL_RESULT::Ok( j, std::to_string( selTool->GetSelection().GetSize() )
                                               + " selected" );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::runDrc( const json& aIn )
{
    BOARD*                 brd = board();
    BOARD_DESIGN_SETTINGS& bds = brd->GetDesignSettings();
    int                    limit = intArg( aIn, "limit", 60, 1, 500 );
    bool                   includeWarnings = boolArg( aIn, "include_warnings" ).value_or( true );

    if( DRC_TOOL* drcTool = m_frame->GetToolManager()->GetTool<DRC_TOOL>() )
    {
        if( drcTool->IsDRCRunning() )
            return COPILOT_TOOL_RESULT::Error( "DRC is already running." );
    }

    std::shared_ptr<DRC_ENGINE> engine = bds.m_DRCEngine;

    if( !engine )
        return COPILOT_TOOL_RESULT::Error( "DRC engine unavailable." );

    bool refill = boolArg( aIn, "refill_zones" ).value_or( false ) && EditsAllowed();

    if( refill && !brd->Zones().empty() )
    {
        if( ZONE_FILLER_TOOL* filler = m_frame->GetToolManager()->GetTool<ZONE_FILLER_TOOL>() )
            filler->FillAllZones( m_frame );
    }

    // Always recompile the rules: net classes and constraints may have changed since the last
    // run (for example through set_design_rules).
    try
    {
        engine->InitEngine( brd->GetDesignRulesPath() );
    }
    catch( PARSE_ERROR& e )
    {
        return COPILOT_TOOL_RESULT::Error( "The custom design rules could not be compiled: "
                                           + S( e.What() ) );
    }

    engine->SetDrawingSheet( m_frame->GetCanvas()->GetDrawingSheet() );

    struct VIOLATION
    {
        SEVERITY                   severity;
        std::shared_ptr<DRC_ITEM>  item;
        VECTOR2I                   pos;
    };

    std::vector<VIOLATION> violations;

    // Called from DRC worker threads (serialised by the engine): only record what was found;
    // the board is inspected after the run, on this thread.
    engine->SetViolationHandler(
            [&]( const std::shared_ptr<DRC_ITEM>& aItem, const VECTOR2I& aPos, int aLayer,
                 const std::function<void( PCB_MARKER* )>& aPathGenerator )
            {
                SEVERITY sev = bds.GetSeverity( aItem->GetErrorCode() );

                if( sev == RPT_SEVERITY_IGNORE || sev == RPT_SEVERITY_EXCLUSION )
                    return;

                violations.push_back( { sev, aItem, aPos } );
            } );

    wxBusyCursor busy;

    try
    {
        engine->RunTests( m_frame->GetUserUnits(), false, false );
    }
    catch( ... )
    {
        engine->ClearViolationHandler();
        throw;
    }

    engine->ClearViolationHandler();

    auto describe =
            [&]( const VIOLATION& aV ) -> json
            {
                json items = json::array();

                for( const KIID& id : { aV.item->GetMainItemID(), aV.item->GetAuxItemID() } )
                {
                    if( id == niluuid )
                        continue;

                    if( BOARD_ITEM* item = brd->ResolveItem( id, true ) )
                        items.push_back( S( item->GetItemDescription( m_frame, false ) ) );
                }

                return items;
            };

    std::map<std::string, int> byType;
    json                       list = json::array();
    int                        errors = 0, warnings = 0;

    for( const VIOLATION& v : violations )
    {
        bool isError = v.severity == RPT_SEVERITY_ERROR;

        if( isError )
            ++errors;
        else
            ++warnings;

        if( !isError && !includeWarnings )
            continue;

        std::string key = S( v.item->GetSettingsKey() );
        byType[key]++;

        if( (int) list.size() < limit )
        {
            list.push_back( { { "severity", isError ? "error" : "warning" },
                              { "type", key },
                              { "message", S( v.item->GetErrorMessage( false ) ) },
                              { "items", describe( v ) },
                              { "x_mm", mm( v.pos.x ) },
                              { "y_mm", mm( v.pos.y ) } } );
        }
    }

    json j;
    j["errors"] = errors;
    j["warnings"] = warnings;
    j["by_type"] = byType;
    j["violations"] = list;
    j["note"] = std::string( refill ? "Zones were refilled before the check. "
                                    : "Zones were not refilled; zone results reflect the last "
                                      "fill. " )
                + "Schematic parity was not checked.";

    return COPILOT_TOOL_RESULT::Ok( j, std::to_string( errors ) + " errors, "
                                               + std::to_string( warnings ) + " warnings" );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::getReviewData( const json& aIn )
{
    BOARD* brd = board();
    double decoupleMax = numArg( aIn, "decoupling_max_mm" ).value_or( 5.0 );
    double powerMinW = numArg( aIn, "power_track_min_mm" ).value_or( 0.3 );

    json j;

    // --- Decoupling: every power pad of an IC should have a cap to ground nearby -----------
    struct CAP
    {
        FOOTPRINT* fp;
        int        netA;
        int        netB;
    };

    std::vector<CAP> caps;

    for( FOOTPRINT* fp : brd->Footprints() )
    {
        if( refPrefix( fp->GetReference() ) == wxS( "C" ) && fp->Pads().size() == 2 )
            caps.push_back( { fp, fp->Pads()[0]->GetNetCode(), fp->Pads()[1]->GetNetCode() } );
    }

    auto netName = [&]( int aCode ) -> wxString
    {
        NETINFO_ITEM* n = brd->FindNet( aCode );
        return n ? n->GetNetname() : wxString();
    };

    json decoupling = json::array();

    for( FOOTPRINT* fp : brd->Footprints() )
    {
        wxString prefix = refPrefix( fp->GetReference() );

        if( ( prefix != wxS( "U" ) && prefix != wxS( "IC" ) ) || fp->Pads().size() < 3 )
            continue;

        std::map<int, std::vector<PAD*>> powerPads;

        for( PAD* pad : fp->Pads() )
        {
            if( pad->GetNetCode() > 0 && isPowerNet( pad->GetNetname() ) )
                powerPads[pad->GetNetCode()].push_back( pad );
        }

        for( const auto& [code, padsOnNet] : powerPads )
        {
            double      best = 1e9;
            std::string bestRef;

            for( const CAP& cap : caps )
            {
                bool bridges = ( cap.netA == code && isGroundNet( netName( cap.netB ) ) )
                               || ( cap.netB == code && isGroundNet( netName( cap.netA ) ) );

                if( !bridges )
                    continue;

                for( PAD* pad : padsOnNet )
                {
                    double d = pcbIUScale.IUTomm(
                            (int) ( pad->GetPosition() - cap.fp->GetPosition() ).EuclideanNorm() );

                    if( d < best )
                    {
                        best = d;
                        bestRef = S( cap.fp->GetReference() );
                    }
                }
            }

            if( best > decoupleMax )
            {
                json entry = { { "ic", S( fp->GetReference() ) },
                               { "value", S( fp->GetValue() ) },
                               { "power_net", S( netName( code ) ) },
                               { "power_pads", (int) padsOnNet.size() } };

                if( bestRef.empty() )
                    entry["nearest_decoupling_cap"] = "none on this net";
                else
                    entry["nearest_decoupling_cap"] = bestRef + " at "
                                                      + std::to_string( (int) std::round( best ) )
                                                      + " mm";

                decoupling.push_back( entry );
            }
        }
    }

    j["ic_power_pins_without_close_decoupling"] = decoupling;
    j["decoupling_threshold_mm"] = decoupleMax;

    // --- Single-pad nets (likely unconnected by mistake) -----------------------------------
    std::map<int, std::vector<std::string>> padsByNet;

    for( FOOTPRINT* fp : brd->Footprints() )
    {
        for( PAD* pad : fp->Pads() )
        {
            if( pad->GetNetCode() > 0 )
                padsByNet[pad->GetNetCode()].push_back( S( fp->GetReference() ) + "."
                                                        + S( pad->GetNumber() ) );
        }
    }

    json singlePad = json::array();

    for( const auto& [code, pads] : padsByNet )
    {
        wxString name = netName( code );

        // KiCad names deliberately unconnected pins "unconnected-(...)".
        if( pads.size() == 1 && !name.StartsWith( wxS( "unconnected-" ) )
            && !name.Contains( wxS( "/unconnected-" ) ) && singlePad.size() < 100 )
        {
            singlePad.push_back( { { "net", S( name ) }, { "pad", pads[0] } } );
        }
    }

    j["single_pad_nets"] = singlePad;

    // --- Track widths: below net class, and thin power tracks ------------------------------
    std::map<wxString, std::pair<int, int>> belowClass;    // net -> {count, min width}
    std::map<wxString, int>                 thinPower;     // net -> min width

    for( PCB_TRACK* t : brd->Tracks() )
    {
        if( t->Type() == PCB_VIA_T || t->GetNetCode() <= 0 )
            continue;

        wxString  net = t->GetNetname();
        NETCLASS* nc = t->GetEffectiveNetClass();

        if( nc && nc->HasTrackWidth() && t->GetWidth() < nc->GetTrackWidth() )
        {
            auto& e = belowClass[net];
            e.first++;
            e.second = e.first == 1 ? t->GetWidth() : std::min( e.second, t->GetWidth() );
        }

        if( ( isPowerNet( net ) || isGroundNet( net ) ) && t->GetWidth() < toIU( powerMinW ) )
        {
            auto it = thinPower.find( net );
            thinPower[net] = it == thinPower.end() ? t->GetWidth()
                                                   : std::min( it->second, t->GetWidth() );
        }
    }

    json below = json::array();

    for( const auto& [net, e] : belowClass )
        below.push_back( { { "net", S( net ) }, { "segments", e.first },
                           { "min_width_mm", mm( e.second ) } } );

    j["tracks_narrower_than_netclass"] = below;

    json thin = json::array();

    for( const auto& [net, w] : thinPower )
        thin.push_back( { { "net", S( net ) }, { "min_width_mm", mm( w ) } } );

    j["power_nets_with_thin_tracks"] = thin;
    j["power_track_threshold_mm"] = powerMinW;

    // --- Placement: footprints outside the outline -----------------------------------------
    BOX2I edges = brd->GetBoardEdgesBoundingBox();
    json  outside = json::array();

    if( edges.GetWidth() > 0 )
    {
        for( FOOTPRINT* fp : brd->Footprints() )
        {
            if( !edges.Contains( fp->GetPosition() ) )
                outside.push_back( S( fp->GetReference() ) );
        }
    }

    j["footprints_outside_outline"] = outside;

    // --- BOM readiness ---------------------------------------------------------------------
    static const std::vector<wxString> mpnFields = { wxS( "MPN" ), wxS( "Manufacturer_Part_Number" ),
                                                     wxS( "Manufacturer Part Number" ),
                                                     wxS( "MFR Part" ), wxS( "PartNumber" ),
                                                     wxS( "Part Number" ), wxS( "LCSC" ),
                                                     wxS( "Digikey" ), wxS( "Mouser" ) };
    json missingValue = json::array();
    json missingMpn = json::array();
    json dnp = json::array();
    std::map<wxString, std::set<wxString>> footprintsPerValue;

    for( FOOTPRINT* fp : brd->Footprints() )
    {
        if( fp->IsExcludedFromBOM() || fp->GetReference().StartsWith( wxS( "#" ) ) )
            continue;

        wxString prefix = refPrefix( fp->GetReference() );

        // Mechanical items rarely carry part numbers.
        bool mechanical = prefix == wxS( "H" ) || prefix == wxS( "MH" ) || prefix == wxS( "TP" )
                          || prefix == wxS( "FID" ) || prefix == wxS( "LOGO" ) || prefix == wxS( "G" );

        if( fp->IsDNP() )
            dnp.push_back( S( fp->GetReference() ) );

        wxString value = fp->GetValue();

        if( value.IsEmpty() || value == wxS( "~" ) || value == fp->GetReference() )
            missingValue.push_back( S( fp->GetReference() ) );

        bool hasMpn = false;

        for( const wxString& fieldName : mpnFields )
        {
            for( const PCB_FIELD* field : fp->GetFields() )
            {
                if( field && field->GetName().CmpNoCase( fieldName ) == 0
                    && !field->GetText().IsEmpty() && field->GetText() != wxS( "~" ) )
                {
                    hasMpn = true;
                }
            }
        }

        if( !hasMpn && !mechanical && missingMpn.size() < 200 )
            missingMpn.push_back( S( fp->GetReference() ) );

        if( !mechanical && !value.IsEmpty() )
            footprintsPerValue[prefix + wxS( ":" ) + value].insert( fp->GetFPID().GetUniStringLibItemName() );
    }

    json inconsistent = json::array();

    for( const auto& [key, fps] : footprintsPerValue )
    {
        if( fps.size() > 1 )
        {
            json names = json::array();

            for( const wxString& f : fps )
                names.push_back( S( f ) );

            inconsistent.push_back( { { "prefix_and_value", S( key ) }, { "footprints", names } } );
        }
    }

    j["bom"] = { { "missing_value", missingValue },
                 { "missing_manufacturer_part_number", missingMpn },
                 { "same_value_different_footprints", inconsistent },
                 { "dnp", dnp } };

    j["notes"] = "Heuristics: power/ground nets are detected by name; ICs are references "
                 "starting with U or IC. Verify findings before acting on them.";

    return COPILOT_TOOL_RESULT::Ok( j, std::to_string( decoupling.size() ) + " decoupling, "
                                               + std::to_string( singlePad.size() )
                                               + " single-pad nets" );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::searchFootprintLibraries( const json& aIn )
{
    FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( &m_frame->Prj() );

    if( !adapter )
        return COPILOT_TOOL_RESULT::Error( "Footprint libraries are not available." );

    CopilotWaitForLibraries( adapter );

    wxString query = W( strArg( aIn, "query" ) ).Lower();
    wxString onlyLib = W( strArg( aIn, "library" ) );
    int      limit = intArg( aIn, "limit", 40, 1, 200 );

    wxArrayString words = wxSplit( query, ' ' );
    words.erase( std::remove_if( words.begin(), words.end(),
                                 []( const wxString& w ) { return w.IsEmpty(); } ),
                 words.end() );

    if( words.IsEmpty() )
        return COPILOT_TOOL_RESULT::Error( "Empty query." );

    json results = json::array();
    int  total = 0;

    for( const wxString& lib : adapter->GetLibraryNames() )
    {
        if( !onlyLib.IsEmpty() && lib.CmpNoCase( onlyLib ) != 0 )
            continue;

        for( const wxString& name : adapter->GetFootprintNames( lib, true ) )
        {
            wxString id = lib + wxS( ":" ) + name;
            wxString lower = id.Lower();
            bool     all = true;

            for( const wxString& w : words )
            {
                if( !lower.Contains( w ) )
                {
                    all = false;
                    break;
                }
            }

            if( !all )
                continue;

            ++total;

            if( (int) results.size() < limit )
                results.push_back( S( id ) );
        }
    }

    json j = { { "total_matches", total }, { "footprints", results } };

    if( total == 0 )
        j["note"] = "No matches. Try fewer or more generic words (libraries may still be "
                    "loading if KiCad was just started).";
    else if( total > limit )
        j["note"] = "Results truncated; add words to narrow the search.";

    return COPILOT_TOOL_RESULT::Ok( j, std::to_string( total ) + " matches" );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::getLibraryFootprint( const json& aIn )
{
    FOOTPRINT_LIBRARY_ADAPTER* adapter = PROJECT_PCB::FootprintLibAdapter( &m_frame->Prj() );

    CopilotWaitForLibraries( adapter );

    if( !adapter )
        return COPILOT_TOOL_RESULT::Error( "Footprint libraries are not available." );

    std::string idStr = strArg( aIn, "lib_id" );
    LIB_ID      id;

    if( id.Parse( W( idStr ) ) >= 0 || !id.IsValid() )
        return COPILOT_TOOL_RESULT::Error( "Invalid library id '" + idStr
                                           + "'; expected Library:Footprint." );

    std::unique_ptr<FOOTPRINT> fp( adapter->LoadFootprint( id, false ) );

    if( !fp )
        return COPILOT_TOOL_RESULT::Error( "Footprint '" + idStr + "' not found." );

    json j;
    j["lib_id"] = idStr;
    j["description"] = S( fp->GetLibDescription() );
    j["keywords"] = S( fp->GetKeywords() );
    j["pads"] = (int) fp->Pads().size();

    std::map<std::string, int> padTypes;

    for( const PAD* pad : fp->Pads() )
        padTypes[padTypeName( pad->GetAttribute() )]++;

    j["pad_types"] = padTypes;

    // Library footprints are not on a board, so their courtyard cache is not built yet.
    fp->BuildCourtyardCaches();
    const SHAPE_POLY_SET& court = fp->GetCourtyard( F_CrtYd );
    BOX2I bbox = court.OutlineCount() ? court.BBox() : fp->GetBoundingBox( false );
    j[court.OutlineCount() ? "courtyard_mm" : "bbox_mm"] = { { "w", mm( bbox.GetWidth() ) },
                                                            { "h", mm( bbox.GetHeight() ) } };

    return COPILOT_TOOL_RESULT::Ok( j, idStr );
}


// ---------------------------------------------------------------------------------------------
// View tools
// ---------------------------------------------------------------------------------------------

COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::selectItems( const json& aIn )
{
    BOARD*                 brd = board();
    std::vector<EDA_ITEM*> items;
    json                   missing = json::array();

    if( aIn.contains( "references" ) && aIn["references"].is_array() )
    {
        for( const json& r : aIn["references"] )
        {
            if( !r.is_string() )
                continue;

            if( FOOTPRINT* fp = findFootprint( r.get<std::string>() ) )
                items.push_back( fp );
            else
                missing.push_back( r );
        }
    }

    if( aIn.contains( "nets" ) && aIn["nets"].is_array() )
    {
        for( const json& n : aIn["nets"] )
        {
            if( !n.is_string() )
                continue;

            NETINFO_ITEM* net = brd->FindNet( W( n.get<std::string>() ) );

            if( !net || net->GetNetCode() <= 0 )
            {
                missing.push_back( n );
                continue;
            }

            for( PCB_TRACK* t : brd->Tracks() )
            {
                if( t->GetNetCode() == net->GetNetCode() )
                    items.push_back( t );
            }

            for( FOOTPRINT* fp : brd->Footprints() )
            {
                for( PAD* pad : fp->Pads() )
                {
                    if( pad->GetNetCode() == net->GetNetCode() )
                        items.push_back( pad );
                }
            }
        }
    }

    TOOL_MANAGER* mgr = m_frame->GetToolManager();
    mgr->RunAction( ACTIONS::selectionClear );

    if( !items.empty() )
    {
        mgr->RunAction( ACTIONS::selectItems, &items );

        if( boolArg( aIn, "zoom" ).value_or( true ) )
            mgr->RunAction( ACTIONS::zoomFitSelection );
    }

    m_frame->GetCanvas()->Refresh( false );

    json j = { { "selected", (int) items.size() } };

    if( !missing.empty() )
        j["not_found"] = missing;

    return COPILOT_TOOL_RESULT::Ok( j, std::to_string( items.size() ) + " items selected" );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::highlightNet( const json& aIn )
{
    std::string   name = strArg( aIn, "name" );
    TOOL_MANAGER* mgr = m_frame->GetToolManager();

    if( name.empty() )
    {
        mgr->RunAction( PCB_ACTIONS::clearHighlight );
        return COPILOT_TOOL_RESULT::Ok( { { "highlight", "cleared" } }, "cleared" );
    }

    NETINFO_ITEM* net = board()->FindNet( W( name ) );

    if( !net || net->GetNetCode() <= 0 )
        return COPILOT_TOOL_RESULT::Error( "No net named '" + name + "'." );

    mgr->RunAction( PCB_ACTIONS::highlightNet, net->GetNetCode() );
    return COPILOT_TOOL_RESULT::Ok( { { "highlighted", name } }, name );
}


// ---------------------------------------------------------------------------------------------
// Editing tools
// ---------------------------------------------------------------------------------------------

COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::moveFootprint( const json& aIn )
{
    std::string ref = strArg( aIn, "reference" );
    FOOTPRINT*  fp = findFootprint( ref );

    if( !fp )
        return COPILOT_TOOL_RESULT::Error( "No footprint with reference '" + ref + "'." );

    if( fp->IsLocked() )
        return COPILOT_TOOL_RESULT::Error( ref + " is locked; unlock it first "
                                                 "(set_footprint_locked) if the user agrees." );

    std::optional<double> x = numArg( aIn, "x_mm" );
    std::optional<double> y = numArg( aIn, "y_mm" );
    std::optional<double> dx = numArg( aIn, "dx_mm" );
    std::optional<double> dy = numArg( aIn, "dy_mm" );
    std::optional<double> rot = numArg( aIn, "rotation_deg" );
    std::string           side = strArg( aIn, "side" );

    if( !x && !y && !dx && !dy && !rot && side.empty() )
        return COPILOT_TOOL_RESULT::Error( "Nothing to change: give a position, offset, "
                                           "rotation or side." );

    VECTOR2I pos = fp->GetPosition();

    if( x )
        pos.x = toIU( *x );

    if( y )
        pos.y = toIU( *y );

    if( dx )
        pos.x += toIU( *dx );

    if( dy )
        pos.y += toIU( *dy );

    BOARD_COMMIT commit( m_frame );
    commit.Modify( fp );

    if( !side.empty() )
    {
        bool wantBack = side == "back";

        if( wantBack != ( fp->GetLayer() == B_Cu ) )
            fp->Flip( fp->GetPosition(), m_frame->GetPcbNewSettings()->m_FlipDirection );
    }

    fp->SetPosition( pos );

    // Reduce first: EDA_ANGLE normalises by repeated subtraction, which never ends for huge
    // values.
    if( rot )
        fp->SetOrientation( EDA_ANGLE( std::fmod( *rot, 360.0 ), DEGREES_T ) );

    commit.Push( wxString::Format( _( "Copilot: move %s" ), fp->GetReference() ) );

    return COPILOT_TOOL_RESULT::Ok( footprintJson( fp, false ),
                                    "moved " + ref + " (undo with Ctrl+Z)" );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::setFootprintField( const json& aIn )
{
    std::string ref = strArg( aIn, "reference" );
    std::string fieldName = strArg( aIn, "field" );
    std::string value = strArg( aIn, "value" );
    FOOTPRINT*  fp = findFootprint( ref );

    if( !fp )
        return COPILOT_TOOL_RESULT::Error( "No footprint with reference '" + ref + "'." );

    if( fieldName.empty() )
        return COPILOT_TOOL_RESULT::Error( "A field name is required." );

    wxString name = W( fieldName );

    if( name.CmpNoCase( wxS( "Reference" ) ) == 0 )
        return COPILOT_TOOL_RESULT::Error( "Changing references is not supported; annotate the "
                                           "schematic instead." );

    BOARD_COMMIT commit( m_frame );
    commit.Modify( fp );

    if( name.CmpNoCase( wxS( "Value" ) ) == 0 )
    {
        fp->SetValue( W( value ) );
    }
    else if( PCB_FIELD* field = fp->GetField( name ) )
    {
        field->SetText( W( value ) );
    }
    else
    {
        PCB_FIELD* newField = new PCB_FIELD( fp, FIELD_T::USER, name );
        newField->SetText( W( value ) );
        newField->SetVisible( false );
        newField->SetLayer( fp->GetLayer() == B_Cu ? B_Fab : F_Fab );
        newField->SetPosition( fp->GetPosition() );
        fp->Add( newField );
    }

    commit.Push( wxString::Format( _( "Copilot: set %s field of %s" ), name, fp->GetReference() ) );

    return COPILOT_TOOL_RESULT::Ok( { { "reference", ref }, { "field", fieldName },
                                      { "value", value } },
                                    ref + " " + fieldName + " = " + value );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::setFootprintLocked( const json& aIn )
{
    std::string         ref = strArg( aIn, "reference" );
    std::optional<bool> locked = boolArg( aIn, "locked" );
    FOOTPRINT*          fp = findFootprint( ref );

    if( !fp )
        return COPILOT_TOOL_RESULT::Error( "No footprint with reference '" + ref + "'." );

    if( !locked )
        return COPILOT_TOOL_RESULT::Error( "'locked' must be true or false." );

    BOARD_COMMIT commit( m_frame );
    commit.Modify( fp );
    fp->SetLocked( *locked );
    commit.Push( *locked ? _( "Copilot: lock footprint" ) : _( "Copilot: unlock footprint" ) );

    return COPILOT_TOOL_RESULT::Ok( { { "reference", ref }, { "locked", *locked } },
                                    ref + ( *locked ? " locked" : " unlocked" ) );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::setTrackWidth( const json& aIn )
{
    BOARD*                brd = board();
    std::string           netName = strArg( aIn, "net" );
    std::optional<double> width = numArg( aIn, "width_mm" );
    std::string           layerName = strArg( aIn, "layer" );

    NETINFO_ITEM* net = brd->FindNet( W( netName ) );

    if( !net || net->GetNetCode() <= 0 )
        return COPILOT_TOOL_RESULT::Error( "No net named '" + netName + "'." );

    if( !width || *width <= 0.0 || *width > 25.0 )
        return COPILOT_TOOL_RESULT::Error( "width_mm must be between 0 and 25." );

    PCB_LAYER_ID layer = UNDEFINED_LAYER;

    if( !layerName.empty() )
    {
        layer = brd->GetLayerID( W( layerName ) );

        if( layer == UNDEFINED_LAYER || !IsCopperLayer( layer ) )
            return COPILOT_TOOL_RESULT::Error( "'" + layerName + "' is not a copper layer." );
    }

    int newWidth = toIU( *width );
    int minWidth = brd->GetDesignSettings().m_TrackMinWidth;

    BOARD_COMMIT commit( m_frame );
    int          changed = 0;

    for( PCB_TRACK* t : brd->Tracks() )
    {
        if( t->Type() == PCB_VIA_T || t->GetNetCode() != net->GetNetCode() )
            continue;

        if( layer != UNDEFINED_LAYER && t->GetLayer() != layer )
            continue;

        if( t->IsLocked() || t->GetWidth() == newWidth )
            continue;

        commit.Modify( t );
        t->SetWidth( newWidth );
        ++changed;
    }

    if( changed == 0 )
        return COPILOT_TOOL_RESULT::Ok( { { "changed", 0 } }, "no tracks changed" );

    commit.Push( wxString::Format( _( "Copilot: set track width on %s" ), net->GetNetname() ) );

    json j = { { "net", netName }, { "segments_changed", changed }, { "width_mm", *width } };

    if( newWidth < minWidth )
        j["warning"] = "New width is below the board minimum track width; DRC will flag it.";

    j["note"] = "Wider tracks may now violate clearance; run DRC.";

    return COPILOT_TOOL_RESULT::Ok( j, std::to_string( changed ) + " segments on " + netName );
}


COPILOT_TOOL_RESULT PCB_COPILOT_TOOLBOX::addText( const json& aIn )
{
    BOARD*                brd = board();
    std::string           text = strArg( aIn, "text" );
    std::optional<double> x = numArg( aIn, "x_mm" );
    std::optional<double> y = numArg( aIn, "y_mm" );
    std::string           layerName = strArg( aIn, "layer", "F.SilkS" );
    std::optional<double> size = numArg( aIn, "size_mm" );

    if( text.empty() || !x || !y )
        return COPILOT_TOOL_RESULT::Error( "text, x_mm and y_mm are required." );

    PCB_LAYER_ID layer = brd->GetLayerID( W( layerName ) );

    if( layer == UNDEFINED_LAYER )
        return COPILOT_TOOL_RESULT::Error( "Unknown layer '" + layerName + "'." );

    BOARD_DESIGN_SETTINGS& bds = brd->GetDesignSettings();

    PCB_TEXT* item = new PCB_TEXT( brd );
    item->SetText( W( text ) );
    item->SetLayer( layer );
    item->SetPosition( VECTOR2I( toIU( *x ), toIU( *y ) ) );

    if( size && *size > 0.05 && *size < 100.0 )
    {
        item->SetTextSize( VECTOR2I( toIU( *size ), toIU( *size ) ) );
        item->SetTextThickness( std::max( 1, toIU( *size * 0.15 ) ) );
    }
    else
    {
        item->SetTextSize( bds.GetTextSize( layer ) );
        item->SetTextThickness( bds.GetTextThickness( layer ) );
    }

    item->SetItalic( bds.GetTextItalic( layer ) );
    item->SetKeepUpright( bds.GetTextUpright( layer ) );
    item->SetMirrored( IsBackLayer( layer ) );

    BOARD_COMMIT commit( m_frame );
    commit.Add( item );
    commit.Push( _( "Copilot: add text" ) );

    return COPILOT_TOOL_RESULT::Ok( { { "added", text }, { "layer", layerName },
                                      { "x_mm", *x }, { "y_mm", *y } },
                                    "added text on " + layerName );
}
