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
#include <optional>
#include <string>
#include <vector>

#include <copilot/copilot_session.h>


/**
 * Helpers for reading tool arguments leniently (models sometimes quote numbers).
 */
namespace COPILOT_ARGS
{
    std::string           Str( const nlohmann::json& aIn, const char* aKey,
                               const std::string& aDefault = "" );
    std::optional<double> Num( const nlohmann::json& aIn, const char* aKey );
    int                   Int( const nlohmann::json& aIn, const char* aKey, int aDefault, int aMin,
                               int aMax );
    std::optional<bool>   Bool( const nlohmann::json& aIn, const char* aKey );
    std::vector<std::string> StrList( const nlohmann::json& aIn, const char* aKey );

    nlohmann::json String( const std::string& aDescription );
    nlohmann::json Number( const std::string& aDescription );
    nlohmann::json Integer( const std::string& aDescription );
    nlohmann::json Boolean( const std::string& aDescription );
    nlohmann::json StringArray( const std::string& aDescription );
    nlohmann::json Enum( const std::vector<std::string>& aValues, const std::string& aDescription );
}


/**
 * A COPILOT_TOOLBOX that keeps its tools in a table of {definition, handler}.
 *
 * It also implements the cross-application protocol used by the KiCad project manager to call
 * the tools of the schematic and PCB editors through KIWAY mail (see HandleMail()).
 */
/// Host part of an http(s) URL, lower case, without credentials or port ("" if none).
std::string CopilotUrlHost( const std::string& aUrl );

/// @a aText with control characters, line breaks and Unicode direction marks made visible and
/// its length capped, for showing untrusted text (e.g. a command line) in approval prompts.
std::string CopilotVisibleText( const std::string& aText, size_t aMaxBytes );


class COPILOT_REGISTRY_TOOLBOX : public COPILOT_TOOLBOX
{
public:
    using HANDLER = std::function<COPILOT_TOOL_RESULT( const nlohmann::json& )>;

    enum FLAGS
    {
        READ_ONLY = 0,
        MODIFIES = 1,       ///< Changes the design; only offered when edits are allowed.
        APPROVAL = 2,       ///< Runs programs, downloads or uploads: ask the user first (the
                            ///< user may allow it for the rest of the session).
        ASK_EVERY_TIME = 4  ///< Like APPROVAL, but "always allow" is not offered.
    };

    std::vector<COPILOT_TOOL_DEF> GetTools( bool aAllowEdits ) override;
    COPILOT_TOOL_RESULT ExecuteTool( const std::string& aName,
                                     const nlohmann::json& aInput ) override;

    /// Calls that reach a URL are approved per web host.
    std::string ApprovalKey( const std::string& aName, const nlohmann::json& aInput ) override;

    std::string DescribeToolCall( const std::string& aName, const nlohmann::json& aInput ) override;

    /**
     * Serve a KIWAY_MAIL request.  The payload is JSON:
     *   {"op":"list"}                               -> {"tools":[...definitions...]}
     *   {"op":"call","name":"...","input":{...},"allow_edits":bool}
     *                                               -> {"content":"...","is_error":bool,"summary":"..."}
     *   {"op":"context"}                            -> {"context":"..."}
     *   {"op":"ready"}                              -> {"ready":bool,"why":"..."}
     */
    std::string HandleMail( const std::string& aPayload );

    /// Parse a tool definition list produced by HandleMail( "list" ).
    static std::vector<COPILOT_TOOL_DEF> ParseToolList( const std::string& aReply );

    /// Parse a call result produced by HandleMail( "call" ).
    static COPILOT_TOOL_RESULT ParseCallResult( const std::string& aReply );

    static std::string MakeListRequest();
    static std::string MakeReadyRequest();
    static std::string MakeCallRequest( const std::string& aName, const nlohmann::json& aInput,
                                        bool aAllowEdits = false );

    /// Parse a reply to MakeReadyRequest().
    static bool ParseReadyReply( const std::string& aReply, std::string& aWhy );

    /// True while one of this toolbox's tools is running (directly or forwarded by mail).
    /// The owning window must not be destroyed then.
    bool IsExecuting() const { return m_executing > 0; }

    bool FindTool( const std::string& aName, COPILOT_TOOL_DEF& aDef ) override
    {
        if( const COPILOT_TOOL_DEF* def = findDef( aName ) )
        {
            aDef = *def;
            return true;
        }

        return false;
    }

protected:
    void AddTool( const std::string& aName, const std::string& aDescription,
                  const nlohmann::json& aProperties, const std::vector<std::string>& aRequired,
                  int aFlags, HANDLER aHandler );

    /// Called before every tool; return an error to refuse (e.g. "no document open").
    virtual std::optional<COPILOT_TOOL_RESULT> preExecute( const std::string& aName )
    {
        return std::nullopt;
    }

    const COPILOT_TOOL_DEF* findDef( const std::string& aName ) const;

private:
    struct TOOL
    {
        COPILOT_TOOL_DEF def;
        HANDLER          handler;
    };

    std::vector<TOOL>             m_tools;
    std::map<std::string, size_t> m_index;
    int                           m_executing = 0;
};
