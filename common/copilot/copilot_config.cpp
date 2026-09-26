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

#include <copilot/copilot_config.h>
#include <copilot/copilot_shared_state.h>

#include <kiplatform/secrets.h>
#include <pgm_base.h>

#include <wx/translation.h>
#include <wx/utils.h>

#include <map>


static const wxString SECRET_SERVICE = wxS( "KiCad Copilot" );

// Keys the user entered that could not be written to the OS secret store (for example on
// Linux without a running keyring) are kept for the rest of the session in COPILOT_SHARED,
// which every editor can see.


namespace COPILOT_SETTINGS_UTILS
{

COMMON_SETTINGS::COPILOT_ENDPOINT& Endpoint( COMMON_SETTINGS::COPILOT& aSettings,
                                             COPILOT_PROVIDER_TYPE aType )
{
    switch( aType )
    {
    case COPILOT_PROVIDER_TYPE::ANTHROPIC:         return aSettings.anthropic;
    case COPILOT_PROVIDER_TYPE::OPENAI:            return aSettings.openai;
    case COPILOT_PROVIDER_TYPE::OPENAI_COMPATIBLE: return aSettings.openai_compatible;
    case COPILOT_PROVIDER_TYPE::OLLAMA:            return aSettings.ollama;
    }

    return aSettings.anthropic;
}


wxString LoadApiKey( COPILOT_PROVIDER_TYPE aType, bool* aFromEnvironment )
{
    if( aFromEnvironment )
        *aFromEnvironment = false;

    wxString key;
    wxString secretKey = wxString::FromUTF8( CopilotProviderId( aType ) );

    if( KIPLATFORM::SECRETS::GetSecret( SECRET_SERVICE, secretKey, key ) && !key.IsEmpty() )
        return key;

    if( std::string sessionKey = COPILOT_SHARED::GetSessionKey( CopilotProviderId( aType ) );
        !sessionKey.empty() )
    {
        return wxString::FromUTF8( sessionKey );
    }

    std::vector<wxString> envVars = { wxS( "KICAD_COPILOT_API_KEY" ) };

    if( aType == COPILOT_PROVIDER_TYPE::ANTHROPIC )
        envVars.push_back( wxS( "ANTHROPIC_API_KEY" ) );
    else if( aType == COPILOT_PROVIDER_TYPE::OPENAI )
        envVars.push_back( wxS( "OPENAI_API_KEY" ) );

    for( const wxString& var : envVars )
    {
        if( wxGetEnv( var, &key ) && !key.IsEmpty() )
        {
            if( aFromEnvironment )
                *aFromEnvironment = true;

            return key;
        }
    }

    return wxEmptyString;
}


bool StoreApiKey( COPILOT_PROVIDER_TYPE aType, const wxString& aKey )
{
    wxString secretKey = wxString::FromUTF8( CopilotProviderId( aType ) );

    if( aKey.IsEmpty() )
    {
        COPILOT_SHARED::SetSessionKey( CopilotProviderId( aType ), "" );
        return KIPLATFORM::SECRETS::DeleteSecret( SECRET_SERVICE, secretKey );
    }

    if( KIPLATFORM::SECRETS::StoreSecret( SECRET_SERVICE, secretKey, aKey ) )
    {
        COPILOT_SHARED::SetSessionKey( CopilotProviderId( aType ), "" );
        return true;
    }

    COPILOT_SHARED::SetSessionKey( CopilotProviderId( aType ), std::string( aKey.utf8_str() ) );
    return false;
}


COPILOT_CONFIG CurrentConfig( bool aLoadKey )
{
    COPILOT_CONFIG config;

    COMMON_SETTINGS* common = Pgm().GetCommonSettings();

    if( !common )
        return config;

    COMMON_SETTINGS::COPILOT& settings = common->m_Copilot;

    config.provider = CopilotProviderFromId( std::string( settings.provider.utf8_str() ) );

    const COMMON_SETTINGS::COPILOT_ENDPOINT& endpoint = Endpoint( settings, config.provider );

    config.baseUrl = std::string( wxString( endpoint.base_url ).Trim().Trim( false ).utf8_str() );
    config.model = std::string( wxString( endpoint.model ).Trim().Trim( false ).utf8_str() );
    if( aLoadKey )
        config.apiKey = std::string( LoadApiKey( config.provider ).Trim().Trim( false ).utf8_str() );

    config.maxTokens = settings.max_tokens;
    config.timeoutSecs = settings.timeout_secs;
    config.webSearch = settings.web_search && config.provider == COPILOT_PROVIDER_TYPE::ANTHROPIC;

    return config;
}


wxString ProviderDisplayName( COPILOT_PROVIDER_TYPE aType )
{
    switch( aType )
    {
    case COPILOT_PROVIDER_TYPE::ANTHROPIC:         return _( "Anthropic (Claude)" );
    case COPILOT_PROVIDER_TYPE::OPENAI:            return _( "OpenAI" );
    case COPILOT_PROVIDER_TYPE::OPENAI_COMPATIBLE: return _( "OpenAI-compatible server" );
    case COPILOT_PROVIDER_TYPE::OLLAMA:            return _( "Ollama (local)" );
    }

    return wxEmptyString;
}

} // namespace COPILOT_SETTINGS_UTILS
