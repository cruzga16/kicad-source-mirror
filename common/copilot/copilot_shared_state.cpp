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

#include <copilot/copilot_shared_state.h>

#include <map>
#include <mutex>


namespace
{
int                                s_toolDepth = 0;     // GUI thread only
std::mutex                         s_keyMutex;
std::map<std::string, std::string> s_sessionKeys;
}


namespace COPILOT_SHARED
{

bool ToolRunning()
{
    return s_toolDepth > 0;
}


void BeginTool()
{
    ++s_toolDepth;
}


void EndTool()
{
    if( s_toolDepth > 0 )
        --s_toolDepth;
}


void SetSessionKey( const std::string& aProvider, const std::string& aKey )
{
    std::lock_guard<std::mutex> lock( s_keyMutex );

    if( aKey.empty() )
        s_sessionKeys.erase( aProvider );
    else
        s_sessionKeys[aProvider] = aKey;
}


std::string GetSessionKey( const std::string& aProvider )
{
    std::lock_guard<std::mutex> lock( s_keyMutex );
    auto                        it = s_sessionKeys.find( aProvider );
    return it == s_sessionKeys.end() ? std::string() : it->second;
}

}
