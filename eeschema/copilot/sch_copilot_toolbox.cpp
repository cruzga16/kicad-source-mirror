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

#include <copilot/sch_copilot_toolbox.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <regex>
#include <set>

#include <wx/filename.h>

#include <base_units.h>
#include <advanced_config.h>
#include <connection_graph.h>
#include <connectivity/conn_facade.h>
#include <copilot/copilot_common_tools.h>
#include <erc/erc.h>
#include <erc/erc_item.h>
#include <kiface_base.h>
#include <lib_symbol.h>
#include <libraries/library_manager.h>
#include <libraries/library_table.h>
#include <libraries/symbol_library_adapter.h>
#include <page_info.h>
#include <pgm_base.h>
#include <project_sch.h>
#include <reporter.h>
#include <sch_commit.h>
#include <sch_edit_frame.h>
#include <sch_field.h>
#include <sch_label.h>
#include <sch_line.h>
#include <sch_marker.h>
#include <sch_no_connect.h>
#include <sch_pin.h>
#include <sch_screen.h>
#include <sch_shape.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>
#include <schematic.h>
#include <string_utils.h>
#include <tool/actions.h>
#include <tool/tool_manager.h>
#include <tools/sch_move_tool.h>
#include <tools/sch_selection_tool.h>
#include <sch_sheet.h>
#include <sch_sheet_pin.h>
#include <view/view.h>
#include <sch_view.h>


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
    return std::round( schIUScale.IUTomm( (int) aIU ) * 100.0 ) / 100.0;
}


int toIU( double aMM )
{
    // Keep model supplied values inside the board/sheet coordinate range.
    if( !std::isfinite( aMM ) )
        aMM = 0.0;

    return schIUScale.mmToIU( std::clamp( aMM, -1500.0, 1500.0 ) );
}


/// Snap to the 50 mil schematic connection grid.
int snap( int aIU )
{
    const int grid = schIUScale.MilsToIU( 50 );
    return (int) std::lround( (double) aIU / grid ) * grid;
}


VECTOR2I snap( const VECTOR2I& aPt )
{
    return VECTOR2I( snap( aPt.x ), snap( aPt.y ) );
}


bool isGroundName( const wxString& aName )
{
    static const std::regex re( R"(^/?(.*[/_-])?(gnd|agnd|dgnd|pgnd|sgnd|gnda|gndd|gndpwr|vss|vssa|0v|earth|chassis)[a-z0-9_]*$)",
                                std::regex::icase );
    return std::regex_match( S( aName ), re );
}


bool isPowerName( const wxString& aName )
{
    if( isGroundName( aName ) )
        return true;

    static const std::regex re( R"(^/?(\+[0-9a-z_.]+|-[0-9]+v[0-9]*|[0-9]+v[0-9]*|v?[0-9]+v[0-9]+|vcc[a-z0-9_]*|vdd[a-z0-9_]*|vee|vbus|vbat|vin|vsys|avcc|avdd|dvdd|vddio|vio)$)",
                                std::regex::icase );
    return std::regex_match( S( aName ), re );
}


wxString refPrefix( const wxString& aRef )
{
    wxString prefix;

    for( wxUniChar c : aRef )
    {
        if( wxIsdigit( c ) || c == '?' )
            break;

        prefix += c;
    }

    return prefix;
}


const char* pinTypeName( ELECTRICAL_PINTYPE aType )
{
    switch( aType )
    {
    case ELECTRICAL_PINTYPE::PT_INPUT:         return "input";
    case ELECTRICAL_PINTYPE::PT_OUTPUT:        return "output";
    case ELECTRICAL_PINTYPE::PT_BIDI:          return "bidirectional";
    case ELECTRICAL_PINTYPE::PT_TRISTATE:      return "tri_state";
    case ELECTRICAL_PINTYPE::PT_PASSIVE:       return "passive";
    case ELECTRICAL_PINTYPE::PT_NIC:           return "not_internally_connected";
    case ELECTRICAL_PINTYPE::PT_UNSPECIFIED:   return "unspecified";
    case ELECTRICAL_PINTYPE::PT_POWER_IN:      return "power_in";
    case ELECTRICAL_PINTYPE::PT_POWER_OUT:     return "power_out";
    case ELECTRICAL_PINTYPE::PT_OPENCOLLECTOR: return "open_collector";
    case ELECTRICAL_PINTYPE::PT_OPENEMITTER:   return "open_emitter";
    case ELECTRICAL_PINTYPE::PT_NC:            return "no_connect";
    default:                                   return "unspecified";
    }
}


ELECTRICAL_PINTYPE pinTypeFromName( const std::string& aName )
{
    static const std::map<std::string, ELECTRICAL_PINTYPE> map = {
        { "input", ELECTRICAL_PINTYPE::PT_INPUT },
        { "output", ELECTRICAL_PINTYPE::PT_OUTPUT },
        { "bidirectional", ELECTRICAL_PINTYPE::PT_BIDI },
        { "bidi", ELECTRICAL_PINTYPE::PT_BIDI },
        { "tri_state", ELECTRICAL_PINTYPE::PT_TRISTATE },
        { "passive", ELECTRICAL_PINTYPE::PT_PASSIVE },
        { "free", ELECTRICAL_PINTYPE::PT_NIC },
        { "unspecified", ELECTRICAL_PINTYPE::PT_UNSPECIFIED },
        { "power_in", ELECTRICAL_PINTYPE::PT_POWER_IN },
        { "power_out", ELECTRICAL_PINTYPE::PT_POWER_OUT },
        { "open_collector", ELECTRICAL_PINTYPE::PT_OPENCOLLECTOR },
        { "open_emitter", ELECTRICAL_PINTYPE::PT_OPENEMITTER },
        { "no_connect", ELECTRICAL_PINTYPE::PT_NC },
    };

    std::string key = aName;

    for( char& c : key )
        c = (char) std::tolower( (unsigned char) c );

    auto it = map.find( key );
    return it == map.end() ? ELECTRICAL_PINTYPE::PT_UNSPECIFIED : it->second;
}


/// Direction in which a wire leaves the pin's connection point (away from the body).
VECTOR2I stubDirection( PIN_ORIENTATION aOrient )
{
    switch( aOrient )
    {
    case PIN_ORIENTATION::PIN_RIGHT: return VECTOR2I( -1, 0 );
    case PIN_ORIENTATION::PIN_LEFT:  return VECTOR2I( 1, 0 );
    case PIN_ORIENTATION::PIN_UP:    return VECTOR2I( 0, 1 );
    case PIN_ORIENTATION::PIN_DOWN:  return VECTOR2I( 0, -1 );
    default:                         return VECTOR2I( -1, 0 );
    }
}


/// Direction a (power) symbol's graphics extend from its pin: the pin's own orientation.
VECTOR2I extensionDirection( PIN_ORIENTATION aOrient )
{
    return -stubDirection( aOrient );
}


const char* sideName( const VECTOR2I& aDir )
{
    if( aDir.x < 0 )
        return "left";

    if( aDir.x > 0 )
        return "right";

    return aDir.y < 0 ? "top" : "bottom";
}

} // namespace


SCH_COPILOT_TOOLBOX::SCH_COPILOT_TOOLBOX( SCH_EDIT_FRAME* aFrame ) :
        m_frame( aFrame )
{
    registerTools();
}


SCH_SCREEN* SCH_COPILOT_TOOLBOX::screen() const
{
    return m_frame->GetScreen();
}


SCH_SHEET_PATH& SCH_COPILOT_TOOLBOX::sheet() const
{
    return m_frame->GetCurrentSheet();
}


wxString SCH_COPILOT_TOOLBOX::refOf( const SCH_SYMBOL* aSymbol ) const
{
    return aSymbol->GetRef( &sheet(), false );
}


std::vector<SCH_SYMBOL*> SCH_COPILOT_TOOLBOX::symbolsOnSheet( bool aIncludePower ) const
{
    std::vector<SCH_SYMBOL*> out;

    if( !screen() )
        return out;

    for( SCH_ITEM* item : screen()->Items().OfType( SCH_SYMBOL_T ) )
    {
        SCH_SYMBOL* sym = static_cast<SCH_SYMBOL*>( item );

        if( aIncludePower || !sym->IsPower() )
            out.push_back( sym );
    }

    std::sort( out.begin(), out.end(),
               [this]( SCH_SYMBOL* a, SCH_SYMBOL* b )
               {
                   int c = StrNumCmp( refOf( a ), refOf( b ), true );
                   return c != 0 ? c < 0 : a->GetUnit() < b->GetUnit();
               } );

    return out;
}


SCH_SYMBOL* SCH_COPILOT_TOOLBOX::findSymbol( const std::string& aRef ) const
{
    wxString ref = W( aRef );

    for( SCH_SYMBOL* sym : symbolsOnSheet( true ) )
    {
        if( refOf( sym ).CmpNoCase( ref ) == 0 )
            return sym;
    }

    return nullptr;
}


SCH_COPILOT_TOOLBOX::PIN_REF SCH_COPILOT_TOOLBOX::findPin( const std::string& aSpec ) const
{
    PIN_REF result;

    // References never contain '.' or ':', but pin names can (e.g. "P0.13"), so split at the
    // first separator.
    size_t dot = aSpec.find_first_of( ".:" );

    if( dot == std::string::npos || dot == 0 || dot + 1 >= aSpec.size() )
    {
        result.error = "Pin '" + aSpec + "' must be written REF.PIN, e.g. U1.3 or U1.VDD";
        return result;
    }

    wxString ref = W( aSpec.substr( 0, dot ) );
    wxString pinId = W( aSpec.substr( dot + 1 ) );

    std::vector<std::pair<SCH_SYMBOL*, SCH_PIN*>> byName;
    bool                                          refFound = false;

    // A multi-unit part has one SCH_SYMBOL per placed unit, all sharing the reference.
    for( SCH_SYMBOL* sym : symbolsOnSheet( true ) )
    {
        if( refOf( sym ).CmpNoCase( ref ) != 0 )
            continue;

        refFound = true;

        for( SCH_PIN* pin : sym->GetPins( &sheet() ) )
        {
            if( pin->GetNumber() == pinId )
            {
                result.symbol = sym;
                result.pin = pin;
                return result;
            }

            if( pin->GetShownName().CmpNoCase( pinId ) == 0 )
                byName.emplace_back( sym, pin );
        }
    }

    if( !refFound )
    {
        result.error = "No symbol '" + S( ref ) + "' on this sheet.";
        return result;
    }

    if( byName.size() == 1 )
    {
        result.symbol = byName[0].first;
        result.pin = byName[0].second;
        return result;
    }

    if( byName.size() > 1 )
    {
        result.error = "Pin name '" + S( pinId ) + "' is ambiguous on " + S( ref )
                       + "; use the pin number.";
        return result;
    }

    result.error = "No pin '" + S( pinId ) + "' on " + S( ref )
                   + " (or that unit is not placed); use get_symbol to list pins.";
    return result;
}


wxString SCH_COPILOT_TOOLBOX::nextReference( const wxString& aPrefix )
{
    int maxNum = 0;

    // References handed out for items that are staged in a commit but not yet on the sheet.
    for( const wxString& ref : m_reservedRefs )
    {
        if( ref.StartsWith( aPrefix ) )
        {
            long n = 0;

            if( ref.Mid( aPrefix.length() ).ToLong( &n ) )
                maxNum = std::max( maxNum, (int) n );
        }
    }

    for( const SCH_SHEET_PATH& path : m_frame->Schematic().Hierarchy() )
    {
        for( SCH_ITEM* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            wxString ref = static_cast<SCH_SYMBOL*>( item )->GetRef( &path, false );

            if( ref.StartsWith( aPrefix ) )
            {
                wxString rest = ref.Mid( aPrefix.length() );
                long     n = 0;

                if( !rest.IsEmpty() && rest.ToLong( &n ) )
                    maxNum = std::max( maxNum, (int) n );
            }
        }
    }

    wxString ref = aPrefix + wxString::Format( wxS( "%d" ), maxNum + 1 );
    m_reservedRefs.insert( ref );
    return ref;
}


bool SCH_COPILOT_TOOLBOX::pinIsWired( const VECTOR2I& aPos ) const
{
    for( SCH_ITEM* item : screen()->Items().Overlapping( aPos ) )
    {
        if( item->Type() == SCH_LINE_T )
        {
            SCH_LINE* line = static_cast<SCH_LINE*>( item );

            if( line->IsWire() && ( line->GetStartPoint() == aPos || line->GetEndPoint() == aPos ) )
                return true;
        }
        else if( item->Type() == SCH_NO_CONNECT_T && item->GetPosition() == aPos )
        {
            return true;
        }
        else if( ( item->Type() == SCH_LABEL_T || item->Type() == SCH_GLOBAL_LABEL_T )
                 && item->GetPosition() == aPos )
        {
            return true;
        }
    }

    return false;
}


std::vector<SCH_ITEM*> SCH_COPILOT_TOOLBOX::attachedItems( SCH_SYMBOL* aSymbol,
                                                         std::vector<WIRE_END>* aOtherWires ) const
{
    // What the Copilot draws on a pin: a short stub wire ending in a label or power symbol,
    // a label or power symbol directly on the pin, or a no-connect flag.  Only such private
    // terminations are returned; a wire that leads on to other items is part of someone else's
    // net and is reported in aOtherWires instead (so a move can stretch it).
    std::vector<SCH_ITEM*> out;
    std::set<SCH_ITEM*>    seen;

    auto addUnique = [&]( SCH_ITEM* aItem )
    {
        if( aItem && aItem != aSymbol && seen.insert( aItem ).second )
            out.push_back( aItem );
    };

    // Label or power symbol sitting exactly on point aPt.
    auto terminalsAt = [&]( const VECTOR2I& aPt, std::vector<SCH_ITEM*>& aTerminals )
    {
        for( SCH_ITEM* item : screen()->Items().Overlapping( aPt ) )
        {
            if( ( item->Type() == SCH_LABEL_T || item->Type() == SCH_GLOBAL_LABEL_T )
                && item->GetPosition() == aPt )
            {
                aTerminals.push_back( item );
            }
            else if( item->Type() == SCH_SYMBOL_T && item != aSymbol
                     && static_cast<SCH_SYMBOL*>( item )->IsPower() )
            {
                SCH_SYMBOL* other = static_cast<SCH_SYMBOL*>( item );
                std::vector<SCH_PIN*> pins = other->GetPins( &sheet() );

                if( pins.size() == 1 && pins[0]->GetPosition() == aPt )
                    aTerminals.push_back( other );
            }
        }
    };

    // Anything else that connects at aPt (other wires, pins of other symbols, junctions,
    // sheet pins, bus entries), ignoring aIgnore.
    auto othersAt = [&]( const VECTOR2I& aPt, const SCH_ITEM* aIgnore ) -> bool
    {
        for( SCH_ITEM* item : screen()->Items().Overlapping( aPt ) )
        {
            if( item == aIgnore || item == aSymbol )
                continue;

            switch( item->Type() )
            {
            case SCH_LINE_T:
            {
                SCH_LINE* line = static_cast<SCH_LINE*>( item );

                if( line->IsWire() && line->HitTest( aPt, 0 ) )
                    return true;

                break;
            }

            case SCH_JUNCTION_T:
            case SCH_BUS_WIRE_ENTRY_T:
                if( item->HitTest( aPt, 0 ) )
                    return true;

                break;

            case SCH_SYMBOL_T:
            {
                SCH_SYMBOL* other = static_cast<SCH_SYMBOL*>( item );

                if( other->IsPower() )
                    break;      // handled as a terminal

                for( SCH_PIN* op : other->GetPins( &sheet() ) )
                {
                    if( op->GetPosition() == aPt )
                        return true;
                }

                break;
            }

            case SCH_SHEET_T:
                for( SCH_SHEET_PIN* sp : static_cast<SCH_SHEET*>( item )->GetPins() )
                {
                    if( sp->GetPosition() == aPt )
                        return true;
                }

                break;

            default:
                break;
            }
        }

        return false;
    };

    for( SCH_PIN* pin : aSymbol->GetPins( &sheet() ) )
    {
        VECTOR2I p = pin->GetPosition();

        std::vector<SCH_ITEM*> direct;
        terminalsAt( p, direct );

        for( SCH_ITEM* t : direct )
            addUnique( t );

        for( SCH_ITEM* item : screen()->Items().Overlapping( p ) )
        {
            if( item->Type() == SCH_NO_CONNECT_T && item->GetPosition() == p )
                addUnique( item );

            if( item->Type() != SCH_LINE_T )
                continue;

            SCH_LINE* wire = static_cast<SCH_LINE*>( item );

            if( !wire->IsWire() || ( wire->GetStartPoint() != p && wire->GetEndPoint() != p ) )
                continue;

            VECTOR2I q = wire->GetStartPoint() == p ? wire->GetEndPoint() : wire->GetStartPoint();

            std::vector<SCH_ITEM*> terminals;
            terminalsAt( q, terminals );

            // A stub: nothing but its label / power symbol at the far end (a label shared by
            // several wires is not private to this pin either).
            if( !othersAt( q, wire ) && terminals.size() <= 1 )
            {
                addUnique( wire );

                for( SCH_ITEM* t : terminals )
                    addUnique( t );
            }
            else if( aOtherWires )
            {
                aOtherWires->push_back( { wire, wire->GetStartPoint() == p } );
            }
        }
    }

    return out;
}


bool SCH_COPILOT_TOOLBOX::pointOccupied( const VECTOR2I& aPos, const SCH_ITEM* aIgnore ) const
{
    for( SCH_ITEM* item : screen()->Items().Overlapping( aPos ) )
    {
        if( item == aIgnore )
            continue;

        switch( item->Type() )
        {
        case SCH_LINE_T:
            if( static_cast<SCH_LINE*>( item )->IsWire() && item->HitTest( aPos, 0 ) )
                return true;

            break;

        case SCH_JUNCTION_T:
        case SCH_NO_CONNECT_T:
        case SCH_BUS_WIRE_ENTRY_T:
        case SCH_LABEL_T:
        case SCH_GLOBAL_LABEL_T:
        case SCH_HIER_LABEL_T:
            if( item->GetPosition() == aPos || item->HitTest( aPos, 0 ) )
                return true;

            break;

        case SCH_SYMBOL_T:
            for( SCH_PIN* pin : static_cast<SCH_SYMBOL*>( item )->GetPins( &sheet() ) )
            {
                if( pin->GetPosition() == aPos )
                    return true;
            }

            break;

        case SCH_SHEET_T:
            for( SCH_SHEET_PIN* sp : static_cast<SCH_SHEET*>( item )->GetPins() )
            {
                if( sp->GetPosition() == aPos )
                    return true;
            }

            break;

        default:
            break;
        }
    }

    return false;
}


VECTOR2I SCH_COPILOT_TOOLBOX::findFreeSpot( const BOX2I& aLocalBox, wxString* aNewPageSize )
{
    // Leave room around every symbol for pin stubs and net labels (each side gets half, so
    // neighbouring symbols end up one full margin apart).
    const int marginX = toIU( 12.7 );
    const int marginY = toIU( 7.62 );
    const int step = toIU( 5.08 );

    std::vector<BOX2I> obstacles;

    for( SCH_ITEM* item : screen()->Items() )
    {
        if( item->Type() == SCH_SYMBOL_T )
        {
            BOX2I b = static_cast<SCH_SYMBOL*>( item )->GetBodyAndPinsBoundingBox();
            b.Inflate( marginX, marginY );
            obstacles.push_back( b );
        }
        else if( item->Type() == SCH_LABEL_T || item->Type() == SCH_GLOBAL_LABEL_T
                 || item->Type() == SCH_LINE_T || item->Type() == SCH_NO_CONNECT_T )
        {
            obstacles.push_back( item->GetBoundingBox() );
        }
    }

    // Symbols placed earlier in the same tool call are not on the sheet yet.
    for( const BOX2I& pending : m_pendingBoxes )
    {
        BOX2I b = pending;
        b.Inflate( marginX, marginY );
        obstacles.push_back( b );
    }

    BOX2I local = aLocalBox;
    local.Inflate( marginX, marginY );

    // If the sheet is full, grow it within its own paper family (never shrink it, never switch
    // between ISO and US sizes).
    const PAGE_INFO current = screen()->GetPageSettings();
    std::vector<wxString> sizes = { current.GetTypeAsString() };

    static const std::vector<wxString> iso = { wxS( "A4" ), wxS( "A3" ), wxS( "A2" ),
                                               wxS( "A1" ), wxS( "A0" ) };
    static const std::vector<wxString> us = { wxS( "USLetter" ), wxS( "USLedger" ), wxS( "C" ),
                                              wxS( "D" ), wxS( "E" ) };

    for( const std::vector<wxString>* family : { &iso, &us } )
    {
        auto it = std::find( family->begin(), family->end(), current.GetTypeAsString() );

        if( it == family->end() && family == &us && current.GetType() == PAGE_SIZE_TYPE::A )
            it = family->begin();       // "A" is the same size as US Letter

        if( it != family->end() )
            sizes.insert( sizes.end(), it + 1, family->end() );
    }

    for( const wxString& size : sizes )
    {
        PAGE_INFO page = current;

        if( size != current.GetTypeAsString() )
        {
            page.SetType( size, current.IsPortrait() );

            if( page.GetWidthIU( schIUScale.IU_PER_MILS ) < current.GetWidthIU( schIUScale.IU_PER_MILS )
                || page.GetHeightIU( schIUScale.IU_PER_MILS )
                           < current.GetHeightIU( schIUScale.IU_PER_MILS ) )
            {
                continue;
            }
        }

        const int left = toIU( 25.4 );
        const int top = toIU( 25.4 );
        const int right = page.GetWidthIU( schIUScale.IU_PER_MILS ) - toIU( 20 );
        const int bottom = page.GetHeightIU( schIUScale.IU_PER_MILS ) - toIU( 45 );   // title block

        for( int y = top; y + (int) local.GetHeight() < bottom; y += step )
        {
            for( int x = left; x + (int) local.GetWidth() < right; x += step )
            {
                VECTOR2I origin = snap( VECTOR2I( x - local.GetLeft(), y - local.GetTop() ) );
                BOX2I    candidate = local;
                candidate.Move( origin );

                bool clear = true;

                for( const BOX2I& o : obstacles )
                {
                    if( candidate.Intersects( o ) )
                    {
                        clear = false;
                        break;
                    }
                }

                if( clear )
                {
                    if( size != current.GetTypeAsString() )
                    {
                        screen()->SetPageSettings( page );
                        m_frame->GetCanvas()->GetView()->MarkDirty();

                        if( aNewPageSize )
                            *aNewPageSize = size;
                    }

                    BOX2I placed = aLocalBox;
                    placed.Move( origin );
                    m_pendingBoxes.push_back( placed );
                    return origin;
                }
            }
        }
    }

    // Everything is full: place below the page and let the user tidy up.
    return snap( VECTOR2I( toIU( 25.4 ), screen()->GetPageSettings().GetHeightIU(
                                                 schIUScale.IU_PER_MILS ) + toIU( 25 ) ) );
}


LIB_SYMBOL* SCH_COPILOT_TOOLBOX::libSymbol( const LIB_ID& aId ) const
{
    // Libraries load in the background after the editor opens.
    CopilotWaitForLibraries( PROJECT_SCH::SymbolLibAdapter( &m_frame->Prj() ) );
    return m_frame->GetLibSymbol( aId );
}


LIB_SYMBOL* SCH_COPILOT_TOOLBOX::powerLibSymbol( const wxString& aNet, LIB_ID& aLibId ) const
{
    // Exact power symbol first (power:GND, power:+3V3, ...), then a generic one whose value
    // (which is the net name for power symbols) we override.
    std::vector<wxString> names = { aNet };
    wxString              stripped = aNet.StartsWith( wxS( "/" ) ) ? aNet.Mid( 1 ) : aNet;

    if( stripped != aNet )
        names.push_back( stripped );

    names.push_back( isGroundName( stripped ) ? wxString( wxS( "GND" ) )
                                              : wxString( wxS( "VCC" ) ) );

    for( const wxString& name : names )
    {
        LIB_ID id( wxS( "power" ), name );

        if( LIB_SYMBOL* lib = libSymbol( id ) )
        {
            if( lib->IsPower() )
            {
                aLibId = id;
                return lib;
            }
        }
    }

    return nullptr;
}


SCH_ITEM* SCH_COPILOT_TOOLBOX::makeTerminal( SCH_COMMIT& aCommit, const wxString& aNet,
                                             const VECTOR2I& aPos, const VECTOR2I& aDir,
                                             const std::string& aStyle, std::string& aKind )
{
    std::string style = aStyle;

    if( style.empty() || style == "auto" )
        style = isPowerName( aNet ) ? "power" : "label";

    if( style == "power" )
    {
        LIB_ID      libId;
        LIB_SYMBOL* lib = powerLibSymbol( aNet, libId );

        if( lib )
        {
            SCH_SYMBOL* pwr = new SCH_SYMBOL( *lib, libId, &sheet(), 1, 1, aPos,
                                              &m_frame->Schematic() );

            // Point the symbol's graphics away from the wire.
            for( int orient : { SYM_ORIENT_0, SYM_ORIENT_90, SYM_ORIENT_180, SYM_ORIENT_270 } )
            {
                pwr->SetOrientation( orient );
                std::vector<SCH_PIN*> pins = pwr->GetPins( &sheet() );

                if( pins.empty() )
                    break;

                if( extensionDirection( pins[0]->PinDrawOrient( pwr->GetTransform() ) ) == aDir )
                    break;
            }

            std::vector<SCH_PIN*> pins = pwr->GetPins( &sheet() );

            if( !pins.empty() )
                pwr->Move( aPos - pins[0]->GetPosition() );

            pwr->SetRef( &sheet(), nextReference( wxS( "#PWR" ) ) );

            if( pwr->GetValue( &sheet(), RAW_VALUE ) != aNet )
                pwr->SetValueFieldText( aNet );

            pwr->AutoplaceFields( screen(), AUTOPLACE_AUTO );
            aCommit.Add( pwr, screen() );
            aKind = "power:" + S( libId.GetUniStringLibItemName() );
            return pwr;
        }

        style = "global";   // No power library available: fall back to a global label.
    }

    SPIN_STYLE spin = aDir.x < 0 ? SPIN_STYLE::LEFT
                    : aDir.x > 0 ? SPIN_STYLE::RIGHT
                    : aDir.y < 0 ? SPIN_STYLE::UP
                                 : SPIN_STYLE::BOTTOM;

    SCH_LABEL_BASE* label = nullptr;

    if( style == "global" )
    {
        label = new SCH_GLOBALLABEL( aPos, aNet );
        aKind = "global_label";
    }
    else
    {
        label = new SCH_LABEL( aPos, aNet );
        aKind = "label";
    }

    label->SetSpinStyle( spin );
    aCommit.Add( label, screen() );
    return label;
}


void SCH_COPILOT_TOOLBOX::refreshConnectivity()
{
    m_frame->RecalculateConnections( nullptr, NO_CLEANUP );
    m_frame->TestDanglingEnds();
    m_frame->GetCanvas()->Refresh();
}


json SCH_COPILOT_TOOLBOX::symbolJson( SCH_SYMBOL* aSymbol, bool aDetailed ) const
{
    json j;
    j["reference"] = S( refOf( aSymbol ) );
    j["value"] = S( aSymbol->GetValue( &sheet(), RAW_VALUE ) );
    j["lib_id"] = S( aSymbol->GetLibId().GetUniStringLibId() );
    j["footprint"] = S( aSymbol->GetFootprintFieldText( &sheet(), RAW_VALUE ) );
    j["x_mm"] = mm( aSymbol->GetPosition().x );
    j["y_mm"] = mm( aSymbol->GetPosition().y );

    if( aSymbol->GetUnitCount() > 1 )
    {
        j["unit"] = aSymbol->GetUnit();
        j["units"] = aSymbol->GetUnitCount();
    }

    if( aSymbol->GetDNP( &sheet() ) )
        j["dnp"] = true;

    json fields = json::object();

    for( const SCH_FIELD& field : aSymbol->GetFields() )
    {
        if( field.IsMandatory() && field.GetId() != FIELD_T::DATASHEET )
            continue;

        wxString text = field.GetText();

        if( text.IsEmpty() || text == wxS( "~" ) || field.GetName().StartsWith( wxS( "Sim." ) ) )
            continue;

        fields[S( field.GetName() )] = S( text );
    }

    if( !fields.empty() )
        j["fields"] = fields;

    if( aDetailed )
    {
        if( const std::unique_ptr<LIB_SYMBOL>& lib = aSymbol->GetLibSymbolRef() )
            j["description"] = S( lib->GetDescription() );

        json pins = json::array();

        for( SCH_PIN* pin : aSymbol->GetPins( &sheet() ) )
        {
            json p;
            p["number"] = S( pin->GetNumber() );
            p["name"] = S( pin->GetShownName() );
            p["type"] = pinTypeName( pin->GetType() );

            if( SCH_CONNECTION* conn = pin->Connection( &sheet() ) )
                p["net"] = S( conn->Name() );

            p["wired"] = pinIsWired( pin->GetPosition() );
            p["side"] = sideName( stubDirection( pin->PinDrawOrient( aSymbol->GetTransform() ) ) );

            if( !pin->IsVisible() )
                p["hidden"] = true;

            pins.push_back( p );
        }

        j["pins"] = pins;
    }

    return j;
}


// ---------------------------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------------------------

void SCH_COPILOT_TOOLBOX::registerTools()
{
    auto bind = [this]( COPILOT_TOOL_RESULT ( SCH_COPILOT_TOOLBOX::*aFn )( const json& ) )
    {
        return [this, aFn]( const json& aIn ) { return ( this->*aFn )( aIn ); };
    };

    add( "get_schematic_summary",
         "Overview of the open schematic: file, page size, symbol counts by prefix, nets, "
         "unannotated symbols, labels and existing ERC markers.",
         json::object(), {}, READ_ONLY, bind( &SCH_COPILOT_TOOLBOX::getSummary ) );

    add( "list_symbols",
         "List the symbols on the current sheet with reference, value, library id, footprint "
         "and fields.",
         { { "filter", String( "Case-insensitive filter on reference, value or library id "
                               "(wildcards * ? allowed)." ) },
           { "include_power", Boolean( "Include power symbols (default false)." ) },
           { "limit", Integer( "Maximum results (default 300)." ) } },
         {}, READ_ONLY, bind( &SCH_COPILOT_TOOLBOX::listSymbols ) );

    add( "get_symbol",
         "Details of one placed symbol, including every pin with number, name, electrical type, "
         "side, net and whether it is already wired.",
         { { "reference", String( "Reference designator, e.g. U1." ) } },
         { "reference" }, READ_ONLY, bind( &SCH_COPILOT_TOOLBOX::getSymbol ) );

    add( "list_nets",
         "List the schematic's nets with the pins on each (REF.PIN).",
         { { "filter", String( "Case-insensitive filter on the net name." ) },
           { "limit", Integer( "Maximum nets (default 200)." ) } },
         {}, READ_ONLY, bind( &SCH_COPILOT_TOOLBOX::listNets ) );

    add( "search_symbol_libraries",
         "Search the installed symbol libraries. Every word must match the library id, "
         "description or keywords (case-insensitive). Examples: 'resistor', 'AMS1117', "
         "'ESP32-S3', 'USB C receptacle', 'LED', 'crystal 2 pin'. Common ids: Device:R, Device:C, "
         "Device:C_Polarized, Device:LED, Device:D_Schottky, Device:Crystal, power:GND, "
         "Connector_Generic:Conn_01x04.",
         { { "query", String( "Space separated words." ) },
           { "library", String( "Restrict to one library nickname." ) },
           { "limit", Integer( "Maximum results (default 25)." ) } },
         { "query" }, READ_ONLY, bind( &SCH_COPILOT_TOOLBOX::searchLibraries ) );

    add( "get_library_symbol",
         "Describe a library symbol before placing it: description, default footprint, "
         "footprint filters, datasheet, units and all pins (number, name, type, unit).",
         { { "lib_id", String( "Library:Symbol, e.g. Regulator_Linear:AMS1117-3.3." ) } },
         { "lib_id" }, READ_ONLY, bind( &SCH_COPILOT_TOOLBOX::getLibrarySymbol ) );

    add( "add_symbol",
         "Place a library symbol. It is annotated immediately (e.g. R3) and placed in free "
         "space unless x_mm/y_mm are given. Always set a footprint ('Library:Footprint', e.g. "
         "Resistor_SMD:R_0603_1608Metric) so the PCB can be built. Returns its pins.",
         { { "lib_id", String( "Library:Symbol." ) },
           { "value", String( "Value, e.g. 10k, 100nF, ESP32-S3-WROOM-1." ) },
           { "footprint", String( "Footprint 'Library:Name'." ) },
           { "reference", String( "Optional reference; auto-numbered if omitted." ) },
           { "unit", Integer( "Unit to place for multi-unit symbols (default 1)." ) },
           { "x_mm", Number( "Optional X position." ) },
           { "y_mm", Number( "Optional Y position." ) },
           { "rotation_deg", Integer( "0, 90, 180 or 270." ) },
           { "fields", { { "type", "object" },
                         { "description", "Extra fields, e.g. {\"MPN\": \"...\", \"LCSC\": "
                                          "\"C1234\", \"Datasheet\": \"https://...\"}." } } } },
         { "lib_id" }, MODIFIES, bind( &SCH_COPILOT_TOOLBOX::addSymbol ) );

    add( "connect",
         "Connect pins to a net. Each pin gets a short wire ending in a net label, or in a "
         "power symbol for power/ground nets (GND, +3V3, VBUS, ...). Pins with the same net name "
         "are connected. Call once per net with all its pins.",
         { { "net", String( "Net name, e.g. SDA, +3V3, GND, USB_D+." ) },
           { "pins", StringArray( "Pins as REF.PIN (number or unique name), e.g. [\"U1.21\", "
                                  "\"R4.1\", \"J1.VBUS\"]." ) },
           { "style", Enum( { "auto", "label", "global", "power" },
                            "Terminal style (default auto)." ) } },
         { "net", "pins" }, MODIFIES, bind( &SCH_COPILOT_TOOLBOX::connect ) );

    add( "no_connect",
         "Mark pins as intentionally unconnected (adds no-connect flags) so ERC does not "
         "report them.",
         { { "pins", StringArray( "Pins as REF.PIN." ) } },
         { "pins" }, MODIFIES, bind( &SCH_COPILOT_TOOLBOX::noConnect ) );

    add( "add_power_flag",
         "Add a PWR_FLAG on a net that is powered from a connector, battery or regulator input "
         "so ERC knows it is driven (fixes 'input power pin not driven').",
         { { "net", String( "Net name, e.g. VBUS or GND." ) } },
         { "net" }, MODIFIES, bind( &SCH_COPILOT_TOOLBOX::addPowerFlag ) );

    add( "set_symbol_field",
         "Set a field on a placed symbol: Value, Footprint, Datasheet, Description, or any "
         "custom field (MPN, Manufacturer, LCSC...; created hidden if missing).",
         { { "reference", String( "Reference designator." ) },
           { "field", String( "Field name." ) },
           { "value", String( "New text." ) } },
         { "reference", "field", "value" }, MODIFIES, bind( &SCH_COPILOT_TOOLBOX::setField ) );

    add( "move_symbol",
         "Move a symbol (with its wire stubs and labels) to x_mm/y_mm or by dx_mm/dy_mm.",
         { { "reference", String( "Reference designator." ) },
           { "x_mm", Number( "Absolute X." ) }, { "y_mm", Number( "Absolute Y." ) },
           { "dx_mm", Number( "Relative X." ) }, { "dy_mm", Number( "Relative Y." ) } },
         { "reference" }, MODIFIES, bind( &SCH_COPILOT_TOOLBOX::moveSymbol ) );

    add( "delete_symbol",
         "Delete a symbol together with its wire stubs, labels and no-connect flags.",
         { { "reference", String( "Reference designator." ) } },
         { "reference" }, MODIFIES, bind( &SCH_COPILOT_TOOLBOX::deleteSymbol ) );

    add( "annotate", "Annotate any symbols that still have '?' references.",
         json::object(), {}, MODIFIES, bind( &SCH_COPILOT_TOOLBOX::annotate ) );

    add( "run_erc",
         "Run the electrical rules check and return violations (markers are shown in the "
         "editor).",
         { { "limit", Integer( "Maximum violations listed (default 60)." ) } },
         {}, READ_ONLY, bind( &SCH_COPILOT_TOOLBOX::runErc ) );

    add( "create_symbol",
         "Create a new library symbol (rectangular body with pins on the sides) in the "
         "project's own symbol library, for parts not in any library. Take pin numbers, names "
         "and types from the datasheet. Returns the lib_id to use with add_symbol.",
         { { "name", String( "Symbol name, e.g. the part number." ) },
           { "reference_prefix", String( "U, J, Q, ... (default U)." ) },
           { "description", String( "Short description." ) },
           { "footprint", String( "Default footprint Library:Name." ) },
           { "datasheet", String( "Datasheet URL." ) },
           { "keywords", String( "Search keywords." ) },
           { "library", String( "Library nickname (default: <project>_copilot)." ) },
           { "pins", { { "type", "array" },
                       { "description", "Pins: [{\"number\":\"1\",\"name\":\"VDD\",\"type\":"
                                        "\"power_in\",\"side\":\"left\"}, ...]; side is left, "
                                        "right, top or bottom; type is input, output, "
                                        "bidirectional, passive, power_in, power_out, "
                                        "open_collector, tri_state, no_connect or "
                                        "unspecified." },
                       { "items", { { "type", "object" } } } } } },
         { "name", "pins" }, MODIFIES, bind( &SCH_COPILOT_TOOLBOX::createSymbol ) );

    add( "register_symbol_library",
         "Add a .kicad_sym library file to the project's symbol library table.",
         { { "nickname", String( "Library nickname." ) },
           { "path", String( "Path to the .kicad_sym file (may use ${KIPRJMOD})." ) } },
         { "nickname", "path" }, MODIFIES, bind( &SCH_COPILOT_TOOLBOX::registerLibrary ) );

    add( "select_symbols", "Select symbols in the editor and zoom to them.",
         { { "references", StringArray( "Reference designators." ) } },
         { "references" }, READ_ONLY, bind( &SCH_COPILOT_TOOLBOX::selectSymbols ) );

    add( "set_page_size", "Change the sheet size.",
         { { "size", Enum( { "A4", "A3", "A2", "A1", "A0", "A", "B", "C", "D" }, "Page size." ) } },
         { "size" }, MODIFIES, bind( &SCH_COPILOT_TOOLBOX::setPageSize ) );

    add( "save_schematic", "Save the schematic.", json::object(), {}, MODIFIES,
         bind( &SCH_COPILOT_TOOLBOX::save ) );

    COPILOT_COMMON_TOOLS::Register(
            [this]( const std::string& n, const std::string& d, const json& p,
                    const std::vector<std::string>& r, int f, HANDLER h )
            {
                AddTool( n, d, p, r, f, std::move( h ) );
            },
            COPILOT_COMMON_TOOLS::WEB, nullptr );
}


std::optional<COPILOT_TOOL_RESULT> SCH_COPILOT_TOOLBOX::preExecute( const std::string& aName )
{
    m_reservedRefs.clear();
    m_pendingBoxes.clear();

    if( !screen() )
        return COPILOT_TOOL_RESULT::Error( "No schematic is open." );

    return std::nullopt;
}


bool SCH_COPILOT_TOOLBOX::IsReadyForTools( std::string& aWhy )
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

        if( SCH_MOVE_TOOL* move = mgr->GetTool<SCH_MOVE_TOOL>() )
            busy |= move->IsToolActive();       // interactive move / drag
    }

    if( busy )
    {
        aWhy = "Waiting for you to finish the current action in the schematic editor (press "
               "Esc to cancel it)...";
        return false;
    }

    return true;
}


std::string SCH_COPILOT_TOOLBOX::GetSystemPrompt()
{
    return "You are working in the KiCad Schematic Editor.\n"
           "Coordinates are in millimetres (+Y down). You rarely need them: add_symbol finds "
           "free space automatically.\n"
           "To draw a circuit: search_symbol_libraries, get_library_symbol to learn the pins, "
           "add_symbol (always with value and footprint; add MPN/LCSC fields when known), then "
           "connect each net once with all of its pins, no_connect unused pins, add_power_flag "
           "on nets fed from connectors or batteries, run_erc and fix every error, then "
           "save_schematic.\n"
           "Standard footprints: Resistor_SMD:R_0603_1608Metric, Capacitor_SMD:C_0603_1608Metric, "
           "Capacitor_SMD:C_0805_2012Metric, LED_SMD:LED_0603_1608Metric, "
           "Package_TO_SOT_SMD:SOT-23, Package_TO_SOT_SMD:SOT-223-3_TabPin2, "
           "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm, Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm, "
           "Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical. Prefer the footprint "
           "that the symbol's footprint filters suggest.\n"
           "If a part is not in any library, create it with create_symbol from its datasheet "
           "pinout (the project Copilot can also import parts from LCSC).\n";
}


std::string SCH_COPILOT_TOOLBOX::GetContextSnapshot()
{
    json ctx;
    ctx["schematic"] = S( wxFileName( m_frame->Schematic().GetFileName() ).GetFullName() );
    ctx["sheet"] = S( sheet().PathHumanReadable() );
    ctx["symbols_on_sheet"] = (int) symbolsOnSheet( false ).size();

    if( SCH_SELECTION_TOOL* selTool = m_frame->GetToolManager()->GetTool<SCH_SELECTION_TOOL>() )
    {
        json sel = json::array();

        for( EDA_ITEM* item : selTool->GetSelection() )
        {
            if( sel.size() >= 15 )
                break;

            sel.push_back( S( item->GetItemDescription( m_frame, false ) ) );
        }

        if( !sel.empty() )
            ctx["selection"] = sel;
    }

    return ctx.dump();
}


std::vector<std::pair<std::string, std::string>> SCH_COPILOT_TOOLBOX::GetQuickActions()
{
    return {
        { S( _( "Review schematic" ) ),
          "Review this schematic: run ERC, check decoupling, pull-ups, power sequencing, "
          "connector protection and missing values/footprints. Give a prioritised list with "
          "concrete fixes." },
        { S( _( "Fix ERC" ) ), "Run ERC and fix every error you safely can, then run it again "
                               "and report what is left." },
        { S( _( "Assign footprints" ) ),
          "Find symbols without a footprint and assign sensible standard SMD footprints "
          "(0603 passives unless a part needs something else). Tell me what you chose." },
    };
}


// ---------------------------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------------------------

COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::getSummary( const json& aIn )
{
    SCHEMATIC& sch = m_frame->Schematic();
    json       j;

    j["file"] = S( wxFileName( sch.GetFileName() ).GetFullName() );
    j["sheets"] = (int) sch.Hierarchy().size();
    j["page_size"] = S( screen()->GetPageSettings().GetTypeAsString() );

    std::map<wxString, int> prefixes;
    int                     symbols = 0, power = 0, unannotated = 0, missingFp = 0;

    for( const SCH_SHEET_PATH& path : sch.Hierarchy() )
    {
        for( SCH_ITEM* item : path.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* sym = static_cast<SCH_SYMBOL*>( item );

            if( sym->IsPower() )
            {
                ++power;
                continue;
            }

            ++symbols;
            wxString ref = sym->GetRef( &path, false );
            prefixes[refPrefix( ref )]++;

            if( ref.EndsWith( wxS( "?" ) ) )
                ++unannotated;

            if( sym->GetFootprintFieldText( &path, RAW_VALUE ).IsEmpty() )
                ++missingFp;
        }
    }

    j["symbols"] = symbols;
    j["power_symbols"] = power;
    j["unannotated"] = unannotated;
    j["without_footprint"] = missingFp;

    json byPrefix = json::object();

    for( const auto& [p, n] : prefixes )
        byPrefix[p.IsEmpty() ? "?" : S( p )] = n;

    j["by_prefix"] = byPrefix;

    int labels = 0, markers = 0;

    for( SCH_ITEM* item : screen()->Items() )
    {
        if( item->Type() == SCH_LABEL_T || item->Type() == SCH_GLOBAL_LABEL_T )
            ++labels;
        else if( item->Type() == SCH_MARKER_T )
            ++markers;
    }

    j["labels_on_sheet"] = labels;
    j["erc_markers_on_sheet"] = markers;

    j["nets"] = (int) netPins( wxEmptyString ).size();

    return COPILOT_TOOL_RESULT::Ok( j, std::to_string( symbols ) + " symbols" );
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::listSymbols( const json& aIn )
{
    wxString filter = W( Str( aIn, "filter" ) ).Lower();
    bool     power = Bool( aIn, "include_power" ).value_or( false );
    int      limit = Int( aIn, "limit", 300, 1, 2000 );

    auto matches = [&]( const wxString& aText )
    {
        if( filter.IsEmpty() )
            return true;

        wxString t = aText.Lower();
        return filter.Contains( wxS( "*" ) ) || filter.Contains( wxS( "?" ) ) ? t.Matches( filter )
                                                                               : t.Contains( filter );
    };

    json list = json::array();
    int  total = 0;

    for( SCH_SYMBOL* sym : symbolsOnSheet( power ) )
    {
        if( !matches( refOf( sym ) ) && !matches( sym->GetValue( &sheet(), RAW_VALUE ) )
            && !matches( sym->GetLibId().GetUniStringLibId() ) )
        {
            continue;
        }

        ++total;

        if( (int) list.size() < limit )
            list.push_back( symbolJson( sym, false ) );
    }

    return COPILOT_TOOL_RESULT::Ok( { { "total", total }, { "symbols", list } },
                                    std::to_string( total ) + " symbols" );
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::getSymbol( const json& aIn )
{
    std::string ref = Str( aIn, "reference" );
    json        units = json::array();
    json        result;

    for( SCH_SYMBOL* sym : symbolsOnSheet( true ) )
    {
        if( refOf( sym ).CmpNoCase( W( ref ) ) == 0 )
            units.push_back( symbolJson( sym, true ) );
    }

    if( units.empty() )
        return COPILOT_TOOL_RESULT::Error( "No symbol '" + ref + "' on this sheet." );

    result = units.size() == 1 ? units[0] : json( { { "units", units } } );
    return COPILOT_TOOL_RESULT::Ok( result, ref );
}


std::map<wxString, std::set<wxString>> SCH_COPILOT_TOOLBOX::netPins( const wxString& aFilter )
{
    std::map<wxString, std::set<wxString>> nets;
    SCHEMATIC&                             sch = m_frame->Schematic();

    auto addPin = [&]( std::set<wxString>& aPins, SCH_ITEM* aItem, const SCH_SHEET_PATH& aPath )
    {
        if( aItem->Type() != SCH_PIN_T )
            return;

        SCH_PIN*    pin = static_cast<SCH_PIN*>( aItem );
        SCH_SYMBOL* sym = dynamic_cast<SCH_SYMBOL*>( pin->GetParentSymbol() );

        if( sym && !sym->IsPower() )
            aPins.insert( sym->GetRef( &aPath, false ) + wxS( "." ) + pin->GetNumber() );
    };

    if( ADVANCED_CFG::GetCfg().m_ConnectivityEngine )
    {
        // KiCad's newer connectivity engine publishes nets through the facade.
        std::map<KIID_PATH, SCH_SHEET_PATH> paths;

        for( const SCH_SHEET_PATH& path : sch.Hierarchy() )
            paths[path.Path()] = path;

        for( const SCH_CONNECTIVITY::NET_GROUP& group : sch.Connectivity().GetNetMap() )
        {
            if( !aFilter.IsEmpty() && !group.name.Lower().Contains( aFilter ) )
                continue;

            std::set<wxString>* pins = nullptr;

            for( const SCH_CONNECTIVITY::NET_VIEW& net : group.instances )
            {
                if( !net.IsNet() )
                    continue;

                if( !pins )
                    pins = &nets[group.name];

                auto path = paths.find( net.Instance() );

                if( path == paths.end() )
                    continue;

                for( SCH_ITEM* item : net.Items() )
                    addPin( *pins, item, path->second );
            }
        }

        return nets;
    }

    if( CONNECTION_GRAPH* graph = sch.ConnectionGraph() )
    {
        for( const auto& [key, subgraphs] : graph->GetNetMap() )
        {
            if( !aFilter.IsEmpty() && !key.Name.Lower().Contains( aFilter ) )
                continue;

            std::set<wxString>& pins = nets[key.Name];

            for( const CONNECTION_SUBGRAPH* sg : subgraphs )
            {
                for( SCH_ITEM* item : sg->GetItems() )
                    addPin( pins, item, sg->GetSheet() );
            }
        }
    }

    return nets;
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::listNets( const json& aIn )
{
    refreshConnectivity();

    wxString filter = W( Str( aIn, "filter" ) ).Lower();
    int      limit = Int( aIn, "limit", 200, 1, 2000 );

    std::map<wxString, std::set<wxString>> nets = netPins( filter );

    json list = json::array();
    int  total = 0;

    for( const auto& [name, pins] : nets )
    {
        ++total;

        if( (int) list.size() >= limit )
            continue;

        json p = json::array();

        for( const wxString& pin : pins )
        {
            if( p.size() >= 60 )
            {
                p.push_back( "..." );
                break;
            }

            p.push_back( S( pin ) );
        }

        json n = { { "net", S( name ) }, { "pins", p } };

        if( pins.size() == 1 )
            n["warning"] = "only one pin";

        list.push_back( n );
    }

    return COPILOT_TOOL_RESULT::Ok( { { "total", total }, { "nets", list } },
                                    std::to_string( total ) + " nets" );
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::searchLibraries( const json& aIn )
{
    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &m_frame->Prj() );

    if( !adapter )
        return COPILOT_TOOL_RESULT::Error( "Symbol libraries are not available." );

    CopilotWaitForLibraries( adapter );

    wxString      query = W( Str( aIn, "query" ) ).Lower();
    wxString      onlyLib = W( Str( aIn, "library" ) );
    int           limit = Int( aIn, "limit", 25, 1, 200 );
    wxArrayString words = wxSplit( query, ' ' );

    words.erase( std::remove_if( words.begin(), words.end(),
                                 []( const wxString& w ) { return w.IsEmpty(); } ),
                 words.end() );

    if( words.IsEmpty() )
        return COPILOT_TOOL_RESULT::Error( "Empty query." );

    struct HIT
    {
        int         score;
        json        info;
    };

    std::vector<HIT> hits;

    wxBusyCursor busy;

    for( const wxString& lib : adapter->GetLibraryNames() )
    {
        if( !onlyLib.IsEmpty() && lib.CmpNoCase( onlyLib ) != 0 )
            continue;

        for( LIB_SYMBOL* sym : adapter->GetSymbols( lib ) )
        {
            if( !sym )
                continue;

            wxString id = ( lib + wxS( ":" ) + sym->GetName() ).Lower();
            wxString hay = id + wxS( " " ) + sym->GetDescription().Lower() + wxS( " " )
                           + sym->GetKeyWords().Lower();
            bool     all = true;
            int      score = 0;

            for( const wxString& w : words )
            {
                if( !hay.Contains( w ) )
                {
                    all = false;
                    break;
                }

                score += id.Contains( w ) ? 3 : 1;
            }

            if( !all )
                continue;

            // Prefer exact names and short (generic) ids.
            if( sym->GetName().Lower() == query )
                score += 20;

            score -= (int) id.length() / 12;

            json info;
            info["lib_id"] = S( lib + wxS( ":" ) + sym->GetName() );
            wxString desc = sym->GetDescription();

            if( desc.length() > 140 )
                desc = desc.Left( 137 ) + wxS( "..." );

            info["description"] = S( desc );

            wxString fp = sym->GetFootprintField().GetText();

            if( !fp.IsEmpty() )
                info["footprint"] = S( fp );

            if( sym->GetUnitCount() > 1 )
                info["units"] = sym->GetUnitCount();

            info["pins"] = (int) sym->GetGraphicalPins( 0, 0 ).size();
            hits.push_back( { score, info } );
        }
    }

    std::sort( hits.begin(), hits.end(),
               []( const HIT& a, const HIT& b ) { return a.score > b.score; } );

    json list = json::array();

    for( const HIT& h : hits )
    {
        if( (int) list.size() >= limit )
            break;

        list.push_back( h.info );
    }

    json j = { { "total_matches", (int) hits.size() }, { "symbols", list } };

    if( hits.empty() )
    {
        j["note"] = "No matches. Try fewer or more generic words, a family name, or create the "
                    "part with create_symbol / import it from LCSC.";
    }

    return COPILOT_TOOL_RESULT::Ok( j, std::to_string( hits.size() ) + " matches" );
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::getLibrarySymbol( const json& aIn )
{
    std::string idStr = Str( aIn, "lib_id" );
    LIB_ID      id;

    if( id.Parse( W( idStr ) ) >= 0 || !id.IsValid() )
        return COPILOT_TOOL_RESULT::Error( "Invalid lib_id '" + idStr + "'; use Library:Symbol." );

    LIB_SYMBOL* sym = libSymbol( id );

    if( !sym )
        return COPILOT_TOOL_RESULT::Error( "Symbol '" + idStr + "' not found; use "
                                           "search_symbol_libraries." );

    json j;
    j["lib_id"] = idStr;
    j["description"] = S( sym->GetDescription() );
    j["keywords"] = S( sym->GetKeyWords() );
    j["reference_prefix"] = S( sym->GetReferenceField().GetText() );
    j["default_value"] = S( sym->GetValueField().GetText() );
    j["footprint"] = S( sym->GetFootprintField().GetText() );
    j["datasheet"] = S( sym->GetDatasheetField().GetText() );
    j["units"] = sym->GetUnitCount();
    j["is_power"] = sym->IsPower();

    json filters = json::array();

    for( const wxString& f : sym->GetFPFilters() )
        filters.push_back( S( f ) );

    j["footprint_filters"] = filters;

    json pins = json::array();

    for( SCH_PIN* pin : sym->GetGraphicalPins( 0, 0 ) )
    {
        if( pins.size() >= 400 )
            break;

        json p = { { "number", S( pin->GetNumber() ) },
                   { "name", S( pin->GetShownName() ) },
                   { "type", pinTypeName( pin->GetType() ) } };

        if( sym->GetUnitCount() > 1 )
            p["unit"] = pin->GetUnit();

        if( !pin->IsVisible() )
            p["hidden"] = true;

        pins.push_back( p );
    }

    j["pins"] = pins;
    return COPILOT_TOOL_RESULT::Ok( j, idStr + ", " + std::to_string( pins.size() ) + " pins" );
}


// ---------------------------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------------------------

COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::addSymbol( const json& aIn )
{
    std::string idStr = Str( aIn, "lib_id" );
    LIB_ID      id;

    if( id.Parse( W( idStr ) ) >= 0 || !id.IsValid() )
        return COPILOT_TOOL_RESULT::Error( "Invalid lib_id '" + idStr + "'; use Library:Symbol." );

    LIB_SYMBOL* lib = libSymbol( id );

    if( !lib )
        return COPILOT_TOOL_RESULT::Error( "Symbol '" + idStr + "' not found; use "
                                           "search_symbol_libraries." );

    int unit = Int( aIn, "unit", 1, 1, std::max( 1, lib->GetUnitCount() ) );

    SCH_SYMBOL* sym = new SCH_SYMBOL( *lib, id, &sheet(), unit, 1, VECTOR2I( 0, 0 ),
                                      &m_frame->Schematic() );

    int rot = Int( aIn, "rotation_deg", 0, 0, 270 );

    switch( ( rot + 45 ) / 90 % 4 )
    {
    case 1: sym->SetOrientation( SYM_ORIENT_90 ); break;
    case 2: sym->SetOrientation( SYM_ORIENT_180 ); break;
    case 3: sym->SetOrientation( SYM_ORIENT_270 ); break;
    default: break;
    }

    // Reference
    std::string wantedRef = Str( aIn, "reference" );
    wxString    ref;

    if( !wantedRef.empty() )
    {
        ref = W( wantedRef );

        // A second unit of an existing multi-unit part may share its reference.
        SCH_SYMBOL* existing = findSymbol( wantedRef );

        if( existing && ( existing->GetLibId() != id || existing->GetUnit() == unit ) )
        {
            delete sym;
            return COPILOT_TOOL_RESULT::Error( "Reference " + wantedRef + " is already used." );
        }
    }
    else
    {
        wxString prefix = lib->GetReferenceField().GetText();

        if( prefix.EndsWith( wxS( "?" ) ) )
            prefix.RemoveLast();

        if( prefix.IsEmpty() )
            prefix = wxS( "U" );

        ref = nextReference( prefix );
    }

    sym->SetRef( &sheet(), ref );

    std::string value = Str( aIn, "value" );

    if( !value.empty() )
        sym->SetValueFieldText( W( value ) );

    std::string footprint = Str( aIn, "footprint" );

    if( !footprint.empty() )
        sym->SetFootprintFieldText( W( footprint ) );

    if( aIn.contains( "fields" ) && aIn["fields"].is_object() )
    {
        for( const auto& [name, val] : aIn["fields"].items() )
        {
            std::string text = val.is_string() ? val.get<std::string>() : val.dump();
            wxString    fname = W( name );

            if( fname.CmpNoCase( wxS( "Value" ) ) == 0 )
            {
                sym->SetValueFieldText( W( text ) );
            }
            else if( fname.CmpNoCase( wxS( "Footprint" ) ) == 0 )
            {
                sym->SetFootprintFieldText( W( text ) );
            }
            else if( SCH_FIELD* field = sym->GetField( fname ) )
            {
                field->SetText( W( text ) );
            }
            else
            {
                SCH_FIELD f( sym, FIELD_T::USER, fname );
                f.SetText( W( text ) );
                f.SetVisible( false );
                f.SetPosition( sym->GetPosition() );
                sym->AddField( f );
            }
        }
    }

    // Position
    std::optional<double> x = Num( aIn, "x_mm" );
    std::optional<double> y = Num( aIn, "y_mm" );
    VECTOR2I              pos;

    wxString newPage;

    if( x && y )
        pos = snap( VECTOR2I( toIU( *x ), toIU( *y ) ) );
    else
        pos = findFreeSpot( sym->GetBodyAndPinsBoundingBox(), &newPage );

    sym->SetPosition( pos );
    sym->AutoplaceFields( screen(), AUTOPLACE_AUTO );

    SCH_COMMIT commit( m_frame );
    commit.Add( sym, screen() );
    commit.Push( wxString::Format( _( "Copilot: add %s" ), ref ) );

    refreshConnectivity();

    json j = symbolJson( sym, true );

    if( !newPage.IsEmpty() )
        j["page_size_changed_to"] = S( newPage );

    if( footprint.empty() && sym->GetFootprintFieldText( &sheet(), RAW_VALUE ).IsEmpty()
        && !sym->IsPower() )
    {
        j["warning"] = "No footprint assigned; set one with set_symbol_field before building "
                       "the PCB.";
    }

    return COPILOT_TOOL_RESULT::Ok( j, S( ref ) + " " + idStr );
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::connect( const json& aIn )
{
    wxString                 net = W( Str( aIn, "net" ) );
    std::vector<std::string> pins = StrList( aIn, "pins" );
    std::string              style = Str( aIn, "style", "auto" );

    net.Trim().Trim( false );

    if( net.IsEmpty() || pins.empty() )
        return COPILOT_TOOL_RESULT::Error( "net and pins are required." );

    if( net.Contains( wxS( " " ) ) )
        net.Replace( wxS( " " ), wxS( "_" ) );

    const int  stub = toIU( 2.54 );
    const int  grid = schIUScale.MilsToIU( 50 );
    SCH_COMMIT commit( m_frame );
    json       done = json::array();
    json       problems = json::array();

    // Grid points used by wires staged in this call (not on the sheet until the push).
    std::set<std::pair<int, int>> staged;

    for( const std::string& spec : pins )
    {
        PIN_REF pr = findPin( spec );

        if( !pr.pin )
        {
            problems.push_back( pr.error );
            continue;
        }

        VECTOR2I p = pr.pin->GetPosition();

        if( pinIsWired( p ) )
        {
            wxString current;

            if( SCH_CONNECTION* conn = pr.pin->Connection( &sheet() ) )
                current = conn->Name();

            problems.push_back( spec + " is already connected" + ( current.IsEmpty() ? ""
                                        : " (net " + S( current ) + ")" )
                                + "; delete or move the symbol to reconnect it." );
            continue;
        }

        VECTOR2I dir = stubDirection( pr.pin->PinDrawOrient( pr.symbol->GetTransform() ) );

        // The stub (and its end, where the label or power symbol goes) must not touch anything
        // else, or it would join an unrelated net.  Try progressively longer stubs.
        auto clearPath = [&]( const VECTOR2I& aEnd ) -> bool
        {
            for( VECTOR2I q = p + dir * grid; ; q += dir * grid )
            {
                if( pointOccupied( q ) || staged.count( { q.x, q.y } ) )
                    return false;

                if( q == aEnd )
                    return true;
            }
        };

        VECTOR2I end;
        bool     found = false;

        for( int len = 1; len <= 4 && !found; ++len )
        {
            end = p + dir * ( stub * len );
            found = clearPath( end );
        }

        if( !found )
        {
            problems.push_back( spec + ": no clear room next to the pin for a wire and label; "
                                "move the symbol (move_symbol) and try again." );
            continue;
        }

        for( VECTOR2I q = p + dir * grid; ; q += dir * grid )
        {
            staged.insert( { q.x, q.y } );

            if( q == end )
                break;
        }

        SCH_LINE* wire = new SCH_LINE( p, LAYER_WIRE );
        wire->SetEndPoint( end );
        commit.Add( wire, screen() );

        std::string kind;
        makeTerminal( commit, net, end, dir, style, kind );
        done.push_back( spec + " (" + kind + ")" );
    }

    if( done.empty() )
    {
        commit.Revert();
        return COPILOT_TOOL_RESULT::Error( "Nothing connected: " + problems.dump() );
    }

    commit.Push( wxString::Format( _( "Copilot: connect %s" ), net ) );
    refreshConnectivity();

    json j = { { "net", S( net ) }, { "connected", done } };

    if( !problems.empty() )
        j["problems"] = problems;

    COPILOT_TOOL_RESULT r = COPILOT_TOOL_RESULT::Ok(
            j, S( net ) + ": " + std::to_string( done.size() ) + " pins" );
    return r;
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::noConnect( const json& aIn )
{
    SCH_COMMIT commit( m_frame );
    json       done = json::array(), problems = json::array();

    for( const std::string& spec : StrList( aIn, "pins" ) )
    {
        PIN_REF pr = findPin( spec );

        if( !pr.pin )
        {
            problems.push_back( pr.error );
            continue;
        }

        if( pinIsWired( pr.pin->GetPosition() ) )
        {
            problems.push_back( spec + " is already connected or flagged." );
            continue;
        }

        commit.Add( new SCH_NO_CONNECT( pr.pin->GetPosition() ), screen() );
        done.push_back( spec );
    }

    if( done.empty() )
    {
        commit.Revert();
        return COPILOT_TOOL_RESULT::Error( "Nothing flagged: " + problems.dump() );
    }

    commit.Push( _( "Copilot: no-connect flags" ) );
    refreshConnectivity();

    json j = { { "flagged", done } };

    if( !problems.empty() )
        j["problems"] = problems;

    return COPILOT_TOOL_RESULT::Ok( j, std::to_string( done.size() ) + " pins flagged" );
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::addPowerFlag( const json& aIn )
{
    wxString net = W( Str( aIn, "net" ) );

    if( net.IsEmpty() )
        return COPILOT_TOOL_RESULT::Error( "net is required." );

    LIB_ID      flagId( wxS( "power" ), wxS( "PWR_FLAG" ) );
    LIB_SYMBOL* lib = libSymbol( flagId );

    if( !lib )
        return COPILOT_TOOL_RESULT::Error( "power:PWR_FLAG is not available in the libraries." );

    SCH_SYMBOL* flag = new SCH_SYMBOL( *lib, flagId, &sheet(), 1, 1, VECTOR2I( 0, 0 ),
                                       &m_frame->Schematic() );

    BOX2I box = flag->GetBodyAndPinsBoundingBox();
    box.Inflate( 0, toIU( 7.62 ) );     // room for the stub and terminal below
    wxString newPage;
    flag->SetPosition( findFreeSpot( box, &newPage ) );
    flag->SetRef( &sheet(), nextReference( wxS( "#FLG" ) ) );
    flag->AutoplaceFields( screen(), AUTOPLACE_AUTO );

    std::vector<SCH_PIN*> pins = flag->GetPins( &sheet() );

    if( pins.empty() )
    {
        delete flag;
        return COPILOT_TOOL_RESULT::Error( "PWR_FLAG has no pin." );
    }

    VECTOR2I p = pins[0]->GetPosition();
    VECTOR2I dir = stubDirection( pins[0]->PinDrawOrient( flag->GetTransform() ) );
    VECTOR2I end = p + dir * toIU( 2.54 );

    SCH_COMMIT commit( m_frame );
    commit.Add( flag, screen() );

    SCH_LINE* wire = new SCH_LINE( p, LAYER_WIRE );
    wire->SetEndPoint( end );
    commit.Add( wire, screen() );

    std::string kind;
    makeTerminal( commit, net, end, dir, "auto", kind );

    commit.Push( wxString::Format( _( "Copilot: power flag on %s" ), net ) );
    refreshConnectivity();

    json j = { { "net", S( net ) }, { "flag", S( refOf( flag ) ) } };

    if( !newPage.IsEmpty() )
        j["page_size_changed_to"] = S( newPage );

    return COPILOT_TOOL_RESULT::Ok( j, "PWR_FLAG on " + S( net ) );
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::setField( const json& aIn )
{
    std::string ref = Str( aIn, "reference" );
    wxString    name = W( Str( aIn, "field" ) );
    wxString    value = W( Str( aIn, "value" ) );

    std::vector<SCH_SYMBOL*> units;

    for( SCH_SYMBOL* sym : symbolsOnSheet( true ) )
    {
        if( refOf( sym ).CmpNoCase( W( ref ) ) == 0 )
            units.push_back( sym );
    }

    if( units.empty() )
        return COPILOT_TOOL_RESULT::Error( "No symbol '" + ref + "' on this sheet." );

    if( name.IsEmpty() )
        return COPILOT_TOOL_RESULT::Error( "field is required." );

    if( name.CmpNoCase( wxS( "Reference" ) ) == 0 )
    {
        if( findSymbol( S( value ) ) )
            return COPILOT_TOOL_RESULT::Error( "Reference " + S( value ) + " is already used." );
    }

    SCH_COMMIT commit( m_frame );

    // Keep all units of a multi-unit part consistent.
    for( SCH_SYMBOL* sym : units )
    {
        commit.Modify( sym, screen() );

        if( name.CmpNoCase( wxS( "Reference" ) ) == 0 )
            sym->SetRef( &sheet(), value );
        else if( name.CmpNoCase( wxS( "Value" ) ) == 0 )
            sym->SetValueFieldText( value );
        else if( name.CmpNoCase( wxS( "Footprint" ) ) == 0 )
            sym->SetFootprintFieldText( value );
        else if( SCH_FIELD* field = sym->GetField( name ) )
            field->SetText( value );
        else if( const SCH_FIELD* ci = sym->FindFieldCaseInsensitive( name ) )
            const_cast<SCH_FIELD*>( ci )->SetText( value );
        else
        {
            SCH_FIELD f( sym, FIELD_T::USER, name );
            f.SetText( value );
            f.SetVisible( false );
            f.SetPosition( sym->GetPosition() );
            sym->AddField( f );
        }
    }

    commit.Push( wxString::Format( _( "Copilot: set %s of %s" ), name, W( ref ) ) );

    return COPILOT_TOOL_RESULT::Ok( { { "reference", ref }, { "field", S( name ) },
                                      { "value", S( value ) } },
                                    ref + " " + S( name ) + " = " + S( value ) );
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::moveSymbol( const json& aIn )
{
    std::string ref = Str( aIn, "reference" );
    SCH_SYMBOL* sym = findSymbol( ref );

    if( !sym )
        return COPILOT_TOOL_RESULT::Error( "No symbol '" + ref + "' on this sheet." );

    VECTOR2I target = sym->GetPosition();

    if( std::optional<double> x = Num( aIn, "x_mm" ) )
        target.x = toIU( *x );

    if( std::optional<double> y = Num( aIn, "y_mm" ) )
        target.y = toIU( *y );

    if( std::optional<double> dx = Num( aIn, "dx_mm" ) )
        target.x += toIU( *dx );

    if( std::optional<double> dy = Num( aIn, "dy_mm" ) )
        target.y += toIU( *dy );

    VECTOR2I delta = snap( target ) - sym->GetPosition();

    if( delta == VECTOR2I( 0, 0 ) )
        return COPILOT_TOOL_RESULT::Error( "Nothing to move." );

    std::vector<WIRE_END>  others;
    std::vector<SCH_ITEM*> items = attachedItems( sym, &others );
    items.push_back( sym );

    SCH_COMMIT commit( m_frame );

    for( SCH_ITEM* item : items )
    {
        commit.Modify( item, screen() );
        item->Move( delta );
    }

    // Wires that continue to other items stay connected: move only their end on the pin.
    for( const WIRE_END& we : others )
    {
        commit.Modify( we.wire, screen() );

        if( we.atStart )
            we.wire->SetStartPoint( we.wire->GetStartPoint() + delta );
        else
            we.wire->SetEndPoint( we.wire->GetEndPoint() + delta );
    }

    commit.Push( wxString::Format( _( "Copilot: move %s" ), W( ref ) ) );
    refreshConnectivity();

    return COPILOT_TOOL_RESULT::Ok( { { "reference", ref },
                                      { "x_mm", mm( sym->GetPosition().x ) },
                                      { "y_mm", mm( sym->GetPosition().y ) },
                                      { "moved_items", (int) items.size() },
                                      { "stretched_wires", (int) others.size() } },
                                    "moved " + ref );
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::deleteSymbol( const json& aIn )
{
    std::string ref = Str( aIn, "reference" );
    std::vector<SCH_SYMBOL*> units;

    for( SCH_SYMBOL* sym : symbolsOnSheet( true ) )
    {
        if( refOf( sym ).CmpNoCase( W( ref ) ) == 0 )
            units.push_back( sym );
    }

    if( units.empty() )
        return COPILOT_TOOL_RESULT::Error( "No symbol '" + ref + "' on this sheet." );

    SCH_COMMIT          commit( m_frame );
    std::set<SCH_ITEM*> removed;

    m_frame->GetToolManager()->RunAction( ACTIONS::selectionClear );

    for( SCH_SYMBOL* sym : units )
    {
        for( SCH_ITEM* item : attachedItems( sym ) )
        {
            if( removed.insert( item ).second )
                commit.Remove( item, screen() );
        }

        if( removed.insert( sym ).second )
            commit.Remove( sym, screen() );
    }

    commit.Push( wxString::Format( _( "Copilot: delete %s" ), W( ref ) ) );
    refreshConnectivity();

    return COPILOT_TOOL_RESULT::Ok( { { "deleted", ref }, { "items_removed", (int) removed.size() } },
                                    "deleted " + ref );
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::annotate( const json& aIn )
{
    SCH_COMMIT         commit( m_frame );
    WX_STRING_REPORTER reporter;

    m_frame->AnnotateSymbols( &commit, ANNOTATE_ALL, SORT_BY_X_POSITION, INCREMENTAL_BY_REF, true, 0,
                              false, false, true, reporter, SYMBOL_FILTER_ALL );
    commit.Push( _( "Copilot: annotate" ) );
    refreshConnectivity();

    std::string log = S( reporter.GetMessages() );

    if( log.size() > 4000 )
        log.resize( 4000 );

    return COPILOT_TOOL_RESULT::Ok( { { "log", log } }, "annotated" );
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::runErc( const json& aIn )
{
    int        limit = Int( aIn, "limit", 60, 1, 500 );
    SCHEMATIC* sch = &m_frame->Schematic();

    // Start from a clean slate, as the ERC dialog does; otherwise old markers are counted too.
    m_frame->ClearErcMarkers();

    // CvPcb provides the footprint pin checks; ERC still runs (without them) if it is missing.
    KIFACE* cvpcb = nullptr;

    try
    {
        cvpcb = m_frame->Kiway().KiFACE( KIWAY::FACE_CVPCB );
    }
    catch( ... )
    {
    }

    {
        wxBusyCursor busy;
        ERC_TESTER   tester( sch, false );
        tester.RunTests( m_frame->GetCanvas()->GetView()->GetDrawingSheet(), m_frame, cvpcb,
                         &m_frame->Prj(), nullptr );
    }

    m_frame->RefreshErcMarkers();

    json                       list = json::array();
    std::map<std::string, int> byType;
    int                        errors = 0, warnings = 0;
    SCH_SCREENS                screens( sch->Root() );

    for( SCH_SCREEN* scr = screens.GetFirst(); scr; scr = screens.GetNext() )
    {
        for( SCH_ITEM* item : scr->Items().OfType( SCH_MARKER_T ) )
        {
            SCH_MARKER* marker = static_cast<SCH_MARKER*>( item );

            if( marker->IsExcluded() || marker->GetMarkerType() != MARKER_BASE::MARKER_ERC )
                continue;

            SEVERITY sev = marker->GetSeverity();

            if( sev == RPT_SEVERITY_ERROR )
                ++errors;
            else if( sev == RPT_SEVERITY_WARNING )
                ++warnings;
            else
                continue;

            std::shared_ptr<RC_ITEM> rc = marker->GetRCItem();
            std::string              key = S( rc->GetSettingsKey() );
            byType[key]++;

            if( (int) list.size() >= limit )
                continue;

            json items = json::array();

            for( const KIID& id : { rc->GetMainItemID(), rc->GetAuxItemID() } )
            {
                if( id == niluuid )
                    continue;

                if( SCH_ITEM* it = sch->ResolveItem( id, nullptr, true ) )
                    items.push_back( S( it->GetItemDescription( m_frame, false ) ) );
            }

            list.push_back( { { "severity", sev == RPT_SEVERITY_ERROR ? "error" : "warning" },
                              { "type", key },
                              { "message", S( rc->GetErrorMessage( false ) ) },
                              { "items", items },
                              { "x_mm", mm( marker->GetPosition().x ) },
                              { "y_mm", mm( marker->GetPosition().y ) } } );
        }
    }

    json j = { { "errors", errors }, { "warnings", warnings }, { "by_type", byType },
               { "violations", list } };

    if( byType.count( "power_pin_not_driven" ) )
    {
        j["hint"] = "power_pin_not_driven: add_power_flag on nets fed from a connector, battery "
                    "or regulator input.";
    }

    return COPILOT_TOOL_RESULT::Ok( j, std::to_string( errors ) + " errors, "
                                               + std::to_string( warnings ) + " warnings" );
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::createSymbol( const json& aIn )
{
    wxString name = W( Str( aIn, "name" ) );

    if( name.IsEmpty() || !aIn.contains( "pins" ) || !aIn["pins"].is_array() || aIn["pins"].empty() )
        return COPILOT_TOOL_RESULT::Error( "name and a non-empty pins array are required." );

    // Library names cannot contain these characters.
    for( wxUniChar bad : { ':', '/', '\\', '"' } )
        name.Replace( wxString( bad ), wxS( "_" ) );

    wxString projectName = m_frame->Prj().GetProjectName();
    wxString nick = W( Str( aIn, "library" ) );

    if( nick.IsEmpty() )
        nick = ( projectName.IsEmpty() ? wxString( wxS( "project" ) ) : projectName )
               + wxS( "_copilot" );

    // The nickname also names the library file in the project folder: keep it a plain name.
    for( size_t i = 0; i < nick.length(); ++i )
    {
        wxUniChar c = nick[i];

        if( !( wxIsalnum( c ) || c == '_' || c == '-' ) )
            nick[i] = '_';
    }

    // Sort pins onto the four sides.
    struct PIN_DEF
    {
        wxString           number, name;
        ELECTRICAL_PINTYPE type;
    };

    std::map<std::string, std::vector<PIN_DEF>> sides;

    for( const json& p : aIn["pins"] )
    {
        PIN_DEF def;
        def.number = W( Str( p, "number" ) );
        def.name = W( Str( p, "name", "~" ) );
        def.type = pinTypeFromName( Str( p, "type", "unspecified" ) );

        if( def.number.IsEmpty() )
            return COPILOT_TOOL_RESULT::Error( "Every pin needs a number." );

        std::string side = Str( p, "side" );

        if( side != "left" && side != "right" && side != "top" && side != "bottom" )
        {
            // Sensible default: inputs & power in on the left, the rest on the right.
            side = ( def.type == ELECTRICAL_PINTYPE::PT_INPUT
                     || def.type == ELECTRICAL_PINTYPE::PT_POWER_IN )
                           ? "left"
                           : "right";
        }

        sides[side].push_back( def );
    }

    const int pitch = schIUScale.MilsToIU( 100 );
    const int pinLen = schIUScale.MilsToIU( 100 );

    size_t maxLR = std::max( sides["left"].size(), sides["right"].size() );
    size_t maxTB = std::max( sides["top"].size(), sides["bottom"].size() );

    // Body size: enough rows for the pins and enough width for names.
    size_t longest = 4;

    for( auto& [side, defs] : sides )
    {
        for( const PIN_DEF& d : defs )
            longest = std::max( longest, (size_t) d.name.length() );
    }

    int halfH = (int) ( ( std::max<size_t>( maxLR, 1 ) + 1 ) * pitch / 2 );
    int halfW = std::max( (int) ( ( std::max<size_t>( maxTB, 1 ) + 1 ) * pitch / 2 ),
                          (int) ( longest * schIUScale.MilsToIU( 50 ) + pitch ) );

    halfH = ( halfH + pitch / 2 ) / pitch * pitch;
    halfW = ( halfW + pitch / 2 ) / pitch * pitch;

    auto lib = std::make_unique<LIB_SYMBOL>( name );
    LIB_ID id( nick, name );
    lib->SetLibId( id );

    wxString prefix = W( Str( aIn, "reference_prefix", "U" ) );
    lib->GetReferenceField().SetText( prefix.IsEmpty() ? wxString( wxS( "U" ) ) : prefix );
    lib->GetValueField().SetText( name );
    lib->GetFootprintField().SetText( W( Str( aIn, "footprint" ) ) );
    lib->GetDatasheetField().SetText( W( Str( aIn, "datasheet", "~" ) ) );
    lib->SetDescription( W( Str( aIn, "description" ) ) );
    lib->SetKeyWords( W( Str( aIn, "keywords" ) ) );
    lib->GetFootprintField().SetVisible( false );
    lib->GetDatasheetField().SetVisible( false );

    lib->GetReferenceField().SetPosition( VECTOR2I( -halfW, -halfH - schIUScale.MilsToIU( 100 ) ) );
    lib->GetValueField().SetPosition( VECTOR2I( -halfW, halfH + schIUScale.MilsToIU( 100 ) ) );

    SCH_SHAPE* body = new SCH_SHAPE( SHAPE_T::RECTANGLE, LAYER_DEVICE );
    body->SetStart( VECTOR2I( -halfW, -halfH ) );
    body->SetEnd( VECTOR2I( halfW, halfH ) );
    body->SetFillMode( FILL_T::FILLED_WITH_BG_BODYCOLOR );
    body->SetStroke( STROKE_PARAMS( schIUScale.MilsToIU( 10 ) ) );
    body->SetUnit( 0 );
    body->SetBodyStyle( 0 );
    lib->AddDrawItem( body );

    auto placeSide = [&]( const std::string& aSide )
    {
        const std::vector<PIN_DEF>& defs = sides[aSide];
        int                         n = (int) defs.size();

        for( int i = 0; i < n; ++i )
        {
            int             offset = ( i - ( n - 1 ) / 2 ) * pitch;
            VECTOR2I        pos;
            PIN_ORIENTATION orient;

            if( aSide == "left" )
            {
                pos = VECTOR2I( -halfW - pinLen, -halfH + pitch + i * pitch );
                orient = PIN_ORIENTATION::PIN_RIGHT;
            }
            else if( aSide == "right" )
            {
                pos = VECTOR2I( halfW + pinLen, -halfH + pitch + i * pitch );
                orient = PIN_ORIENTATION::PIN_LEFT;
            }
            else if( aSide == "top" )
            {
                pos = VECTOR2I( offset, -halfH - pinLen );
                orient = PIN_ORIENTATION::PIN_DOWN;
            }
            else
            {
                pos = VECTOR2I( offset, halfH + pinLen );
                orient = PIN_ORIENTATION::PIN_UP;
            }

            SCH_PIN* pin = new SCH_PIN( lib.get(), defs[i].name, defs[i].number, orient,
                                        defs[i].type, pinLen, schIUScale.MilsToIU( 50 ),
                                        schIUScale.MilsToIU( 50 ), 0, pos, 0 );
            lib->AddDrawItem( pin );
        }
    };

    for( const char* side : { "left", "right", "top", "bottom" } )
        placeSide( side );

    // Register the project library (creating the file if needed) and save the symbol.
    wxFileName libFile( m_frame->Prj().GetProjectPath(), nick + wxS( ".kicad_sym" ) );
    wxString   error;

    if( !CopilotRegisterProjectLibrary( LIBRARY_TABLE_TYPE::SYMBOL, nick,
                                        wxS( "${KIPRJMOD}/" ) + libFile.GetFullName(), error ) )
    {
        return COPILOT_TOOL_RESULT::Error( "Could not register the project library: " + S( error ) );
    }

    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &m_frame->Prj() );

    if( !adapter )
        return COPILOT_TOOL_RESULT::Error( "Symbol libraries are not available." );

    if( !libFile.FileExists() && !adapter->CreateLibrary( nick ) )
        return COPILOT_TOOL_RESULT::Error( "Could not create " + S( libFile.GetFullPath() ) );

    size_t pinCount = aIn["pins"].size();

    if( adapter->SaveSymbol( nick, std::move( lib ), true ) != SYMBOL_LIBRARY_ADAPTER::SAVE_OK )
        return COPILOT_TOOL_RESULT::Error( "Saving the symbol failed." );

    // Reload so the new symbol can be placed right away.
    if( !CopilotRegisterProjectLibrary( LIBRARY_TABLE_TYPE::SYMBOL, nick,
                                        wxS( "${KIPRJMOD}/" ) + libFile.GetFullName(), error ) )
    {
        return COPILOT_TOOL_RESULT::Error( "The symbol was saved but the library could not be "
                                           "reloaded: " + S( error ) );
    }

    return COPILOT_TOOL_RESULT::Ok( { { "lib_id", S( id.GetUniStringLibId() ) },
                                      { "library_file", S( libFile.GetFullPath() ) },
                                      { "pins", pinCount } },
                                    S( id.GetUniStringLibId() ) );
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::registerLibrary( const json& aIn )
{
    wxString nick = W( Str( aIn, "nickname" ) );
    wxString path = W( Str( aIn, "path" ) );
    wxString error;

    if( nick.IsEmpty() || path.IsEmpty() )
        return COPILOT_TOOL_RESULT::Error( "nickname and path are required." );

    if( CopilotIsRemotePath( path ) )
        return COPILOT_TOOL_RESULT::Error( "Network locations are not allowed." );

    if( !CopilotRegisterProjectLibrary( LIBRARY_TABLE_TYPE::SYMBOL, nick, path, error ) )
        return COPILOT_TOOL_RESULT::Error( S( error ) );

    return COPILOT_TOOL_RESULT::Ok( { { "registered", S( nick ) }, { "path", S( path ) } },
                                    S( nick ) );
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::selectSymbols( const json& aIn )
{
    std::vector<EDA_ITEM*> items;
    json                   missing = json::array();

    for( const std::string& ref : StrList( aIn, "references" ) )
    {
        bool found = false;

        for( SCH_SYMBOL* sym : symbolsOnSheet( true ) )
        {
            if( refOf( sym ).CmpNoCase( W( ref ) ) == 0 )
            {
                items.push_back( sym );
                found = true;
            }
        }

        if( !found )
            missing.push_back( ref );
    }

    TOOL_MANAGER* mgr = m_frame->GetToolManager();
    mgr->RunAction( ACTIONS::selectionClear );

    if( !items.empty() )
    {
        mgr->RunAction( ACTIONS::selectItems, &items );
        mgr->RunAction( ACTIONS::zoomFitSelection );
    }

    json j = { { "selected", (int) items.size() } };

    if( !missing.empty() )
        j["not_found"] = missing;

    return COPILOT_TOOL_RESULT::Ok( j, std::to_string( items.size() ) + " selected" );
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::setPageSize( const json& aIn )
{
    PAGE_INFO page = screen()->GetPageSettings();

    if( !page.SetType( W( Str( aIn, "size", "A4" ) ) ) )
        return COPILOT_TOOL_RESULT::Error( "Unknown page size." );

    screen()->SetPageSettings( page );
    m_frame->OnModify();
    m_frame->GetCanvas()->GetView()->MarkDirty();
    m_frame->GetCanvas()->Refresh();

    return COPILOT_TOOL_RESULT::Ok( { { "size", S( page.GetTypeAsString() ) } },
                                    S( page.GetTypeAsString() ) );
}


COPILOT_TOOL_RESULT SCH_COPILOT_TOOLBOX::save( const json& aIn )
{
    if( !m_frame->SaveProject() )
        return COPILOT_TOOL_RESULT::Error( "Saving the schematic failed." );

    return COPILOT_TOOL_RESULT::Ok( { { "saved", S( m_frame->Schematic().GetFileName() ) } },
                                    "schematic saved" );
}
