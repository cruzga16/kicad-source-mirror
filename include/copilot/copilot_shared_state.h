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

#include <string>

#include <kicommon.h>

/**
 * Process-wide Copilot state.
 *
 * The Copilot core lives in the static "common" library, which is linked separately into the
 * project manager and into every kiface, so plain statics there exist once per module.  State
 * that must be shared by all editors lives here, in kicommon.
 */
namespace COPILOT_SHARED
{
/// True while any Copilot tool is executing in any editor.  Sessions wait for it to clear
/// before running their own tools, so tools never nest (e.g. while a tool pumps events).
KICOMMON_API bool ToolRunning();

KICOMMON_API void BeginTool();
KICOMMON_API void EndTool();

/// API keys entered this session that could not be stored in the system keychain.
KICOMMON_API void        SetSessionKey( const std::string& aProvider, const std::string& aKey );
KICOMMON_API std::string GetSessionKey( const std::string& aProvider );
}


/// Marks a Copilot tool as running for the lifetime of the object.
class COPILOT_TOOL_SCOPE
{
public:
    COPILOT_TOOL_SCOPE() { COPILOT_SHARED::BeginTool(); }
    ~COPILOT_TOOL_SCOPE() { COPILOT_SHARED::EndTool(); }

    COPILOT_TOOL_SCOPE( const COPILOT_TOOL_SCOPE& ) = delete;
    COPILOT_TOOL_SCOPE& operator=( const COPILOT_TOOL_SCOPE& ) = delete;
};
