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

#include <functional>
#include <map>

#include <copilot/copilot_tool_registry.h>

class BOARD;
class FOOTPRINT;
class PCB_EDIT_FRAME;


/**
 * Exposes the board open in the PCB editor to the Copilot: read-only inspection tools, a
 * design-review helper, DRC, footprint library search, view tools (select/zoom/highlight)
 * and undoable editing tools.
 */
class PCB_COPILOT_TOOLBOX : public COPILOT_REGISTRY_TOOLBOX
{
public:
    explicit PCB_COPILOT_TOOLBOX( PCB_EDIT_FRAME* aFrame );

    std::string GetSystemPrompt() override;
    std::string GetContextSnapshot() override;
    std::vector<std::pair<std::string, std::string>> GetQuickActions() override;
    bool IsReadyForTools( std::string& aWhy ) override;
    int MaxToolRounds() override { return 40; }

protected:
    std::optional<COPILOT_TOOL_RESULT> preExecute( const std::string& aName ) override;

private:
    void registerTools();
    void registerLayoutTools();
    void add( const std::string& aName, const std::string& aDescription,
              const nlohmann::json& aProperties, const std::vector<std::string>& aRequired,
              int aFlags, HANDLER aHandler )
    {
        AddTool( aName, aDescription, aProperties, aRequired, aFlags, std::move( aHandler ) );
    }

    BOARD*     board() const;
    FOOTPRINT* findFootprint( const std::string& aRef ) const;

    // Read-only
    COPILOT_TOOL_RESULT getBoardSummary( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT getDesignRules( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT listFootprints( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT getFootprint( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT listNets( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT getNet( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT getSelection( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT runDrc( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT getReviewData( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT searchFootprintLibraries( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT getLibraryFootprint( const nlohmann::json& aIn );

    // View
    COPILOT_TOOL_RESULT selectItems( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT highlightNet( const nlohmann::json& aIn );

    // Editing (undoable)
    COPILOT_TOOL_RESULT moveFootprint( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT setFootprintField( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT setFootprintLocked( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT setTrackWidth( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT addText( const nlohmann::json& aIn );

    // End-to-end layout (pcb_copilot_layout.cpp)
    COPILOT_TOOL_RESULT updateFromSchematic( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT setBoardOutline( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT setDesignRules( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT arrangeFootprints( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT addZone( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT fillZones( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT autoroute( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT installFreerouting( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT registerFootprintLibrary( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT saveBoard( const nlohmann::json& aIn );

    nlohmann::json footprintJson( const FOOTPRINT* aFp, bool aDetailed ) const;

    PCB_EDIT_FRAME*             m_frame;
};
