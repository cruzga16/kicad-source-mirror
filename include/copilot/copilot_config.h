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

#include <wx/string.h>

#include <copilot/copilot_provider.h>
#include <settings/common_settings.h>

/**
 * Glue between COMMON_SETTINGS::COPILOT, the OS secret store and COPILOT_CONFIG.
 */
namespace COPILOT_SETTINGS_UTILS
{
    /// The settings block for one provider.
    COMMON_SETTINGS::COPILOT_ENDPOINT& Endpoint( COMMON_SETTINGS::COPILOT& aSettings,
                                                 COPILOT_PROVIDER_TYPE aType );

    /**
     * Load the API key for a provider: first the OS secret store, then the environment
     * (KICAD_COPILOT_API_KEY, then ANTHROPIC_API_KEY / OPENAI_API_KEY as appropriate).
     */
    wxString LoadApiKey( COPILOT_PROVIDER_TYPE aType, bool* aFromEnvironment = nullptr );

    /**
     * Store (or, for an empty key, delete) a provider's API key in the OS secret store.
     * If the secret store is unavailable the key is kept in memory for this session and
     * false is returned.
     */
    bool StoreApiKey( COPILOT_PROVIDER_TYPE aType, const wxString& aKey );

    /// Build a runtime config from the current common settings.
    /// The configured provider.  @a aLoadKey false skips the (possibly slow, possibly
    /// prompting) OS keychain lookup when only the provider and model are needed.
    COPILOT_CONFIG CurrentConfig( bool aLoadKey = true );

    /// Human readable provider name.
    wxString ProviderDisplayName( COPILOT_PROVIDER_TYPE aType );
}
