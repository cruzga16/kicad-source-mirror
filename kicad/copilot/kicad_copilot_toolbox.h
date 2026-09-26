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

#include <frame_type.h>

#include <copilot/copilot_tool_registry.h>

class KICAD_MANAGER_FRAME;
class wxString;


/**
 * The project-level Copilot that lives in the KiCad project manager.  It creates and opens
 * projects, drives the schematic and PCB editors (their tools are offered as sch_* and pcb_*
 * and forwarded through KIWAY mail), imports parts from LCSC/EasyEDA, generates fabrication
 * outputs with kicad-cli and hands them to a board house.
 */
class KICAD_COPILOT_TOOLBOX : public COPILOT_REGISTRY_TOOLBOX
{
public:
    explicit KICAD_COPILOT_TOOLBOX( KICAD_MANAGER_FRAME* aFrame );

    std::string GetSystemPrompt() override;
    std::string GetContextSnapshot() override;
    std::vector<COPILOT_TOOL_DEF> GetTools( bool aAllowEdits ) override;
    COPILOT_TOOL_RESULT ExecuteTool( const std::string& aName,
                                     const nlohmann::json& aInput ) override;
    std::vector<std::pair<std::string, std::string>> GetQuickActions() override;
    std::string DescribeToolCall( const std::string& aName, const nlohmann::json& aInput ) override;
    int MaxToolRounds() override { return 250; }
    bool FindTool( const std::string& aName, COPILOT_TOOL_DEF& aDef ) override;
    bool IsReadyForTools( std::string& aWhy ) override;

private:
    void registerTools();
    void add( const std::string& aName, const std::string& aDescription,
              const nlohmann::json& aProperties, const std::vector<std::string>& aRequired,
              int aFlags, HANDLER aHandler )
    {
        AddTool( aName, aDescription, aProperties, aRequired, aFlags, std::move( aHandler ) );
    }

    bool        projectActive() const;
    bool        closeProject();
    wxString    projectDir() const;
    bool        editorOpen( FRAME_T aFrame ) const;
    bool        openEditor( FRAME_T aFrame );

    /// Call a tool in the schematic or PCB editor; opens the editor if needed.
    COPILOT_TOOL_RESULT callEditor( FRAME_T aFrame, const std::string& aName,
                                    const nlohmann::json& aInput );

    COPILOT_TOOL_RESULT getProjectInfo( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT createProject( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT openProject( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT openEditors( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT importLcscPart( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT exportFabrication( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT sendToBoardHouse( const nlohmann::json& aIn );
    COPILOT_TOOL_RESULT openFolder( const nlohmann::json& aIn );

    KICAD_MANAGER_FRAME* m_frame;
    std::string          m_lastFabZip;      ///< Last export, used by send_to_board_house.
    std::string          m_lastFabHouse;
    nlohmann::json       m_lastBoardInfo;
};
