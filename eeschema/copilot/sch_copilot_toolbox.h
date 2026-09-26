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

#include <map>
#include <set>
#include <vector>

#include <copilot/copilot_tool_registry.h>
#include <lib_id.h>
#include <math/box2.h>
#include <math/vector2d.h>

class SCH_EDIT_FRAME;
class SCH_ITEM;
class SCH_PIN;
class SCH_SCREEN;
class SCH_SYMBOL;
class SCH_SHEET_PATH;
class SCH_COMMIT;
class SCH_LINE;
class LIB_SYMBOL;


/**
 * Exposes the schematic open in the schematic editor to the Copilot: inspection, library
 * search, symbol placement, net connection (with net labels and power symbols), ERC, custom
 * symbol creation and saving.
 *
 * Connections are drawn as a short wire stub on each pin ending in a net label or power
 * symbol, which keeps generated schematics electrically unambiguous and easy to edit.
 */
class SCH_COPILOT_TOOLBOX : public COPILOT_REGISTRY_TOOLBOX
{
public:
    explicit SCH_COPILOT_TOOLBOX( SCH_EDIT_FRAME* aFrame );

    std::string GetSystemPrompt() override;
    std::string GetContextSnapshot() override;
    std::vector<std::pair<std::string, std::string>> GetQuickActions() override;
    int MaxToolRounds() override { return 60; }
    bool IsReadyForTools( std::string& aWhy ) override;

protected:
    std::optional<COPILOT_TOOL_RESULT> preExecute( const std::string& aName ) override;

private:
    struct SYMBOL_REF
    {
        SCH_SYMBOL*     symbol = nullptr;
        SCH_SHEET_PATH* path = nullptr;
    };

    struct PIN_REF
    {
        SCH_SYMBOL* symbol = nullptr;
        SCH_PIN*    pin = nullptr;
        std::string error;
    };

    /// A wire touching a symbol pin that leads on to other items (not a private stub).
    struct WIRE_END
    {
        SCH_LINE* wire = nullptr;
        bool      atStart = false;      ///< The pin is at the wire's start point.
    };

    void registerTools();
    void add( const std::string& aName, const std::string& aDescription,
              const nlohmann::json& aProperties, const std::vector<std::string>& aRequired,
              int aFlags, HANDLER aHandler )
    {
        AddTool( aName, aDescription, aProperties, aRequired, aFlags, std::move( aHandler ) );
    }

    SCH_SCREEN*           screen() const;
    SCH_SHEET_PATH&       sheet() const;
    std::vector<SCH_SYMBOL*> symbolsOnSheet( bool aIncludePower ) const;
    SCH_SYMBOL*           findSymbol( const std::string& aRef ) const;
    PIN_REF               findPin( const std::string& aSpec ) const;
    wxString              refOf( const SCH_SYMBOL* aSymbol ) const;
    wxString              nextReference( const wxString& aPrefix );
    VECTOR2I              findFreeSpot( const BOX2I& aLocalBox, wxString* aNewPageSize = nullptr );
    bool                  pinIsWired( const VECTOR2I& aPos ) const;
    bool                  pointOccupied( const VECTOR2I& aPos, const SCH_ITEM* aIgnore = nullptr ) const;
    std::vector<SCH_ITEM*> attachedItems( SCH_SYMBOL* aSymbol,
                                          std::vector<WIRE_END>* aOtherWires = nullptr ) const;
    SCH_ITEM*             makeTerminal( SCH_COMMIT& aCommit, const wxString& aNet,
                                        const VECTOR2I& aPos, const VECTOR2I& aDir,
                                        const std::string& aStyle, std::string& aKind );
    LIB_SYMBOL*           libSymbol( const LIB_ID& aId ) const;
    LIB_SYMBOL*           powerLibSymbol( const wxString& aNet, LIB_ID& aLibId ) const;
    void                  refreshConnectivity();

    /// Net name -> "REF.PIN" of every non-power symbol pin on it (whole hierarchy).
    std::map<wxString, std::set<wxString>> netPins( const wxString& aFilter );
    nlohmann::json        symbolJson( SCH_SYMBOL* aSymbol, bool aDetailed ) const;

    COPILOT_TOOL_RESULT getSummary( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT listSymbols( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT getSymbol( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT listNets( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT searchLibraries( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT getLibrarySymbol( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT addSymbol( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT connect( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT noConnect( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT addPowerFlag( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT setField( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT moveSymbol( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT deleteSymbol( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT annotate( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT runErc( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT createSymbol( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT registerLibrary( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT selectSymbols( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT setPageSize( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT save( const nlohmann::json& aIn );

    SCH_EDIT_FRAME* m_frame;

    // Per tool call: references and page areas handed out to items staged in a commit that
    // are not on the sheet yet.
    std::set<wxString>  m_reservedRefs;
    std::vector<BOX2I>  m_pendingBoxes;
};
