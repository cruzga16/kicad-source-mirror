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

#include <dialogs/dialog_copilot_settings.h>

#include <copilot/copilot_common_tools.h>
#include <copilot/copilot_config.h>
#include <copilot/copilot_session.h>
#include <confirm.h>
#include <pgm_base.h>
#include <settings/settings_manager.h>
#include <widgets/ui_common.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>


static const COPILOT_PROVIDER_TYPE PROVIDER_ORDER[] = {
    COPILOT_PROVIDER_TYPE::ANTHROPIC,
    COPILOT_PROVIDER_TYPE::OPENAI,
    COPILOT_PROVIDER_TYPE::OLLAMA,
    COPILOT_PROVIDER_TYPE::OPENAI_COMPATIBLE
};


DIALOG_COPILOT_SETTINGS::DIALOG_COPILOT_SETTINGS( wxWindow* aParent ) :
        DIALOG_SHIM( aParent, wxID_ANY, _( "Copilot Settings" ), wxDefaultPosition,
                     wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER ),
        m_shownProvider( COPILOT_PROVIDER_TYPE::ANTHROPIC )
{
    wxBoxSizer*       mainSizer = new wxBoxSizer( wxVERTICAL );
    wxFlexGridSizer*  grid = new wxFlexGridSizer( 2, FromDIP( 5 ), FromDIP( 8 ) );
    grid->AddGrowableCol( 1 );

    auto label = [&]( const wxString& aText )
    {
        grid->Add( new wxStaticText( this, wxID_ANY, aText ), 0, wxALIGN_CENTER_VERTICAL );
    };

    label( _( "Provider:" ) );
    m_provider = new wxChoice( this, wxID_ANY );

    for( COPILOT_PROVIDER_TYPE type : PROVIDER_ORDER )
        m_provider->Append( COPILOT_SETTINGS_UTILS::ProviderDisplayName( type ) );

    grid->Add( m_provider, 0, wxEXPAND );

    label( _( "Base URL:" ) );
    m_baseUrl = new wxTextCtrl( this, wxID_ANY );
    m_baseUrl->SetMinSize( FromDIP( wxSize( 360, -1 ) ) );
    grid->Add( m_baseUrl, 0, wxEXPAND );

    label( _( "Model:" ) );
    m_model = new wxTextCtrl( this, wxID_ANY );
    grid->Add( m_model, 0, wxEXPAND );

    label( _( "API key:" ) );
    m_apiKey = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                               wxTE_PASSWORD );
    grid->Add( m_apiKey, 0, wxEXPAND );

    grid->AddSpacer( 0 );
    m_keyHelp = new wxStaticText( this, wxID_ANY, wxEmptyString );
    m_keyHelp->SetFont( KIUI::GetInfoFont( this ) );
    grid->Add( m_keyHelp, 0, wxEXPAND );

    label( _( "Max output tokens:" ) );
    m_maxTokens = new wxSpinCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxDefaultSize, wxSP_ARROW_KEYS, 256, 128000, 4096 );
    grid->Add( m_maxTokens, 0 );

    label( _( "Request timeout (s):" ) );
    m_timeout = new wxSpinCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                wxDefaultSize, wxSP_ARROW_KEYS, 10, 1800, 180 );
    grid->Add( m_timeout, 0 );

    grid->AddSpacer( 0 );
    m_webSearch = new wxCheckBox( this, wxID_ANY, _( "Let Claude search the web (parts, datasheets)" ) );
    grid->Add( m_webSearch, 0 );

    label( _( "Freerouting:" ) );
    m_freerouting = new wxTextCtrl( this, wxID_ANY );
    m_freerouting->SetHint( _( "Path to freerouting.jar or executable (auto-detect if empty)" ) );
    grid->Add( m_freerouting, 0, wxEXPAND );

    mainSizer->Add( grid, 1, wxEXPAND | wxALL, 10 );

    wxStaticText* privacy = new wxStaticText(
            this, wxID_ANY,
            _( "The Copilot sends your questions and the parts of the design it inspects "
               "(references, values, nets, positions) to the selected provider. Use a local "
               "provider such as Ollama to keep everything on this computer." ) );
    privacy->SetFont( KIUI::GetInfoFont( this ) );
    privacy->Wrap( FromDIP( 480 ) );
    mainSizer->Add( privacy, 0, wxEXPAND | wxLEFT | wxRIGHT, 10 );

    wxBoxSizer* testSizer = new wxBoxSizer( wxHORIZONTAL );
    wxButton*   testButton = new wxButton( this, wxID_ANY, _( "Test Connection" ) );
    testSizer->Add( testButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8 );
    m_testResult = new wxStaticText( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                     wxDefaultSize, wxST_ELLIPSIZE_END );
    testSizer->Add( m_testResult, 1, wxALIGN_CENTER_VERTICAL );
    mainSizer->Add( testSizer, 0, wxEXPAND | wxALL, 10 );

    wxStdDialogButtonSizer* sdbSizer = new wxStdDialogButtonSizer();
    sdbSizer->AddButton( new wxButton( this, wxID_OK ) );
    sdbSizer->AddButton( new wxButton( this, wxID_CANCEL ) );
    sdbSizer->Realize();
    mainSizer->Add( sdbSizer, 0, wxALL | wxEXPAND, 5 );

    SetSizer( mainSizer );
    SetupStandardButtons();

    m_provider->Bind( wxEVT_CHOICE, &DIALOG_COPILOT_SETTINGS::onProviderChanged, this );
    testButton->Bind( wxEVT_BUTTON, &DIALOG_COPILOT_SETTINGS::onTestConnection, this );

    finishDialogSettings();
}


COPILOT_PROVIDER_TYPE DIALOG_COPILOT_SETTINGS::selectedProvider() const
{
    int sel = m_provider->GetSelection();

    if( sel < 0 || sel >= (int) std::size( PROVIDER_ORDER ) )
        return COPILOT_PROVIDER_TYPE::ANTHROPIC;

    return PROVIDER_ORDER[sel];
}


void DIALOG_COPILOT_SETTINGS::stashFields( COPILOT_PROVIDER_TYPE aType )
{
    COMMON_SETTINGS::COPILOT_ENDPOINT& ep = COPILOT_SETTINGS_UTILS::Endpoint( m_settings, aType );
    ep.base_url = m_baseUrl->GetValue().Trim().Trim( false );
    ep.model = m_model->GetValue().Trim().Trim( false );
    m_keys[aType] = m_apiKey->GetValue().Trim().Trim( false );
}


void DIALOG_COPILOT_SETTINGS::loadFields( COPILOT_PROVIDER_TYPE aType )
{
    const COMMON_SETTINGS::COPILOT_ENDPOINT& ep =
            COPILOT_SETTINGS_UTILS::Endpoint( m_settings, aType );

    m_baseUrl->ChangeValue( ep.base_url );
    m_model->ChangeValue( ep.model );

    wxString defaultUrl = wxString::FromUTF8( CopilotDefaultBaseUrl( aType ) );
    wxString defaultModel = wxString::FromUTF8( CopilotDefaultModel( aType ) );

    m_baseUrl->SetHint( defaultUrl.IsEmpty() ? _( "e.g. http://localhost:1234/v1" ) : defaultUrl );
    m_model->SetHint( defaultModel.IsEmpty() ? _( "Model name as the server expects it" )
                                             : defaultModel );

    if( !m_keys.count( aType ) )
    {
        bool fromEnv = false;
        wxString key = COPILOT_SETTINGS_UTILS::LoadApiKey( aType, &fromEnv );

        // Keys that come from the environment are not shown or re-stored.
        m_keys[aType] = fromEnv ? wxString() : key;
        m_keyFromEnv[aType] = fromEnv;
    }

    m_apiKey->ChangeValue( m_keys[aType] );

    if( m_keyFromEnv[aType] && m_keys[aType].IsEmpty() )
        m_keyHelp->SetLabel( _( "Using the API key from the environment." ) );
    else if( CopilotProviderNeedsKey( aType ) )
        m_keyHelp->SetLabel( _( "Stored in the system keychain, not in KiCad's settings files." ) );
    else
        m_keyHelp->SetLabel( _( "Optional for this provider." ) );

    m_shownProvider = aType;
    m_testResult->SetLabel( wxEmptyString );
    Layout();
}


bool DIALOG_COPILOT_SETTINGS::TransferDataToWindow()
{
    COMMON_SETTINGS* common = Pgm().GetCommonSettings();

    if( !common )
        return false;

    m_settings = common->m_Copilot;

    COPILOT_PROVIDER_TYPE type =
            CopilotProviderFromId( std::string( m_settings.provider.utf8_str() ) );

    for( size_t i = 0; i < std::size( PROVIDER_ORDER ); ++i )
    {
        if( PROVIDER_ORDER[i] == type )
            m_provider->SetSelection( (int) i );
    }

    m_maxTokens->SetValue( m_settings.max_tokens );
    m_timeout->SetValue( m_settings.timeout_secs );
    m_webSearch->SetValue( m_settings.web_search );
    m_freerouting->SetValue( m_settings.freerouting_path );

    loadFields( type );
    return true;
}


bool DIALOG_COPILOT_SETTINGS::TransferDataFromWindow()
{
    COMMON_SETTINGS* common = Pgm().GetCommonSettings();

    if( !common )
        return false;

    COPILOT_PROVIDER_TYPE type = selectedProvider();
    stashFields( type );

    if( type == COPILOT_PROVIDER_TYPE::OPENAI_COMPATIBLE
        && COPILOT_SETTINGS_UTILS::Endpoint( m_settings, type ).base_url.IsEmpty() )
    {
        DisplayErrorMessage( this, _( "Enter the base URL of your OpenAI-compatible server." ) );
        return false;
    }

    m_settings.provider = wxString::FromUTF8( CopilotProviderId( type ) );
    m_settings.max_tokens = m_maxTokens->GetValue();
    m_settings.timeout_secs = m_timeout->GetValue();
    m_settings.web_search = m_webSearch->GetValue();
    m_settings.freerouting_path = m_freerouting->GetValue().Trim().Trim( false );

    for( const auto& [provider, key] : m_keys )
    {
        // Only write keys the user actually typed; leave environment keys alone.
        if( m_keyFromEnv[provider] && key.IsEmpty() )
            continue;

        bool     fromEnv = false;
        wxString existing = COPILOT_SETTINGS_UTILS::LoadApiKey( provider, &fromEnv );

        if( !fromEnv && existing == key )
            continue;

        if( !COPILOT_SETTINGS_UTILS::StoreApiKey( provider, key ) && !key.IsEmpty() )
        {
            DisplayInfoMessage( this, _( "The API key could not be saved to the system keychain, "
                                         "so it will only be remembered until KiCad is closed.\n\n"
                                         "To keep it permanently, set the ANTHROPIC_API_KEY (or "
                                         "KICAD_COPILOT_API_KEY) environment variable." ) );
        }
    }

    common->m_Copilot = m_settings;
    Pgm().GetSettingsManager().Save( common );
    return true;
}


void DIALOG_COPILOT_SETTINGS::onProviderChanged( wxCommandEvent& aEvent )
{
    stashFields( m_shownProvider );
    loadFields( selectedProvider() );
}


COPILOT_CONFIG DIALOG_COPILOT_SETTINGS::configFromFields()
{
    COPILOT_CONFIG cfg;
    cfg.provider = selectedProvider();
    cfg.baseUrl = std::string( m_baseUrl->GetValue().Trim().Trim( false ).utf8_str() );
    cfg.model = std::string( m_model->GetValue().Trim().Trim( false ).utf8_str() );

    wxString key = m_apiKey->GetValue().Trim().Trim( false );

    if( key.IsEmpty() && m_keyFromEnv[cfg.provider] )
        key = COPILOT_SETTINGS_UTILS::LoadApiKey( cfg.provider );

    cfg.apiKey = std::string( key.utf8_str() );
    cfg.maxTokens = 64;
    cfg.timeoutSecs = 30;
    return cfg;
}


void DIALOG_COPILOT_SETTINGS::onTestConnection( wxCommandEvent& aEvent )
{
    COPILOT_CONFIG cfg = configFromFields();

    if( CopilotProviderNeedsKey( cfg.provider ) && cfg.apiKey.empty() )
    {
        m_testResult->SetLabel( _( "Enter an API key first." ) );
        return;
    }

    std::unique_ptr<COPILOT_PROVIDER> provider = COPILOT_PROVIDER::Create( cfg );

    COPILOT_REQUEST request;
    request.messages.push_back( COPILOT_MESSAGE::User( "Reply with the single word OK." ) );

    m_testResult->SetLabel( _( "Connecting..." ) );
    m_testResult->Update();

    COPILOT_RESPONSE response;
    {
        // Off the GUI thread, so the dialog stays responsive and the test can be cancelled.
        wxBusyCursor     busy;
        COPILOT_ACTIVITY activity( _( "Testing the connection..." ) );

        bool finished = CopilotRunInBackground(
                [&]( const std::atomic<bool>& aCancel )
                {
                    response = CopilotHttpTransport( *provider, request,
                                                     [&aCancel]() { return aCancel.load(); } );
                } );

        if( !finished )
        {
            response.ok = false;
            response.error = "Cancelled.";
        }
    }

    if( response.ok )
    {
        m_testResult->SetLabel( wxString::Format( _( "Connected to %s." ),
                                                  wxString::FromUTF8( provider->Model() ) ) );
    }
    else
    {
        m_testResult->SetLabel( wxString::FromUTF8( response.error ) );
        m_testResult->SetToolTip( wxString::FromUTF8( response.error ) );
    }
}
