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

#include <widgets/copilot_panel.h>

#include <copilot/copilot_config.h>
#include <copilot/copilot_markdown.h>
#include <dialogs/dialog_copilot_settings.h>
#include <kiplatform/ui.h>
#include <pgm_base.h>
#include <settings/common_settings.h>
#include <widgets/html_window.h>
#include <widgets/ui_common.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>
#include <wx/wrapsizer.h>


static std::string toUtf8( const wxString& aStr )
{
    return std::string( aStr.utf8_str() );
}


COPILOT_PANEL::COPILOT_PANEL( wxWindow* aParent, COPILOT_TOOLBOX* aToolbox ) :
        wxPanel( aParent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL ),
        m_toolbox( aToolbox )
{
    wxBoxSizer* mainSizer = new wxBoxSizer( wxVERTICAL );

    // Header: provider/model status + buttons
    wxBoxSizer* header = new wxBoxSizer( wxHORIZONTAL );
    m_status = new wxStaticText( this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                 wxST_ELLIPSIZE_END );
    m_status->SetFont( KIUI::GetInfoFont( this ) );
    header->Add( m_status, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4 );

    m_newChatButton = new wxButton( this, wxID_ANY, _( "New Chat" ), wxDefaultPosition,
                                    wxDefaultSize, wxBU_EXACTFIT );
    m_newChatButton->SetToolTip( _( "Start a new conversation" ) );
    header->Add( m_newChatButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2 );

    m_settingsButton = new wxButton( this, wxID_ANY, _( "Settings..." ), wxDefaultPosition,
                                     wxDefaultSize, wxBU_EXACTFIT );
    m_settingsButton->SetToolTip( _( "Choose the AI provider, model and API key" ) );
    header->Add( m_settingsButton, 0, wxALIGN_CENTER_VERTICAL );

    mainSizer->Add( header, 0, wxEXPAND | wxALL, 3 );

    // Transcript
    m_transcript = new HTML_WINDOW( this, wxID_ANY, wxDefaultPosition, wxSize( 260, 200 ),
                                    wxHW_SCROLLBAR_AUTO );
    mainSizer->Add( m_transcript, 1, wxEXPAND | wxLEFT | wxRIGHT, 3 );

    // Quick actions
    if( m_toolbox )
    {
        wxWrapSizer* quick = new wxWrapSizer( wxHORIZONTAL );

        for( const auto& [label, prompt] : m_toolbox->GetQuickActions() )
        {
            wxButton* btn = new wxButton( this, wxID_ANY, wxString::FromUTF8( label ),
                                          wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT );
            wxString  promptText = wxString::FromUTF8( prompt );

            btn->SetToolTip( promptText );
            btn->Bind( wxEVT_BUTTON,
                       [this, promptText]( wxCommandEvent& )
                       {
                           SendPrompt( promptText );
                       } );

            quick->Add( btn, 0, wxRIGHT | wxTOP, 2 );
            m_quickButtons.push_back( btn );
        }

        mainSizer->Add( quick, 0, wxEXPAND | wxLEFT | wxRIGHT, 3 );
    }

    // Prompt input
    m_input = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              wxSize( -1, FromDIP( 56 ) ), wxTE_MULTILINE | wxTE_PROCESS_ENTER );
    m_input->SetHint( _( "Ask about or edit this design (Enter to send, Shift+Enter for a new line)" ) );
    mainSizer->Add( m_input, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 3 );

    // Footer
    wxBoxSizer* footer = new wxBoxSizer( wxHORIZONTAL );
    m_allowEdits = new wxCheckBox( this, wxID_ANY, _( "Allow edits" ) );
    m_allowEdits->SetToolTip( _( "Let the Copilot modify the design. Every change is a separate "
                                 "undoable step." ) );
    footer->Add( m_allowEdits, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6 );

    m_usage = new wxStaticText( this, wxID_ANY, wxEmptyString );
    m_usage->SetFont( KIUI::GetInfoFont( this ) );
    footer->Add( m_usage, 1, wxALIGN_CENTER_VERTICAL );

    m_sendButton = new wxButton( this, wxID_ANY, _( "Send" ) );
    footer->Add( m_sendButton, 0, wxALIGN_CENTER_VERTICAL );

    mainSizer->Add( footer, 0, wxEXPAND | wxALL, 3 );

    SetSizer( mainSizer );
    Layout();

    m_session = COPILOT_SESSION::Create( m_toolbox,
                                         [this]()
                                         {
                                             return currentConfig();
                                         } );
    m_session->SetListener( this );

    if( COMMON_SETTINGS* common = Pgm().GetCommonSettings() )
    {
        m_allowEdits->SetValue( common->m_Copilot.allow_edits );
        m_session->SetAllowEdits( common->m_Copilot.allow_edits );
    }

    m_sendButton->Bind( wxEVT_BUTTON, &COPILOT_PANEL::onSend, this );
    m_newChatButton->Bind( wxEVT_BUTTON, &COPILOT_PANEL::onNewChat, this );
    m_settingsButton->Bind( wxEVT_BUTTON, &COPILOT_PANEL::onSettings, this );
    m_allowEdits->Bind( wxEVT_CHECKBOX, &COPILOT_PANEL::onAllowEdits, this );
    m_input->Bind( wxEVT_KEY_DOWN, &COPILOT_PANEL::onInputKey, this );
    m_transcript->Bind( wxEVT_HTML_LINK_CLICKED, &COPILOT_PANEL::onLinkClicked, this );
    Bind( wxEVT_SYS_COLOUR_CHANGED, &COPILOT_PANEL::onThemeChanged, this );

    // The API key lives in the OS keychain; only look it up (for the "no key" hint) once the
    // pane is actually visible, not for every hidden pane at editor start-up.
    Bind( wxEVT_SHOW,
          [this]( wxShowEvent& aEvent )
          {
              if( aEvent.IsShown() && !m_busy && m_session && m_session->History().empty() )
                  CallAfter( [this]() { showWelcome(); } );

              aEvent.Skip();
          } );

    updateStatus();
    showWelcome();

    CallAfter(
            [this]()
            {
                if( IsShownOnScreen() && !m_busy && m_session && m_session->History().empty() )
                    showWelcome();
            } );
}


COPILOT_PANEL::~COPILOT_PANEL()
{
    DetachToolbox();
}


void COPILOT_PANEL::DetachToolbox()
{
    m_toolbox = nullptr;

    if( m_session )
    {
        m_session->SetListener( nullptr );
        m_session->DetachToolbox();
        m_session->Cancel();
    }
}


COPILOT_CONFIG COPILOT_PANEL::currentConfig()
{
    if( !m_config )
        m_config = COPILOT_SETTINGS_UTILS::CurrentConfig();

    return *m_config;
}


void COPILOT_PANEL::FocusInput()
{
    m_input->SetFocus();
}


void COPILOT_PANEL::SendPrompt( const wxString& aPrompt )
{
    if( m_busy || aPrompt.IsEmpty() )
        return;

    m_input->SetValue( aPrompt );
    submit();
}


void COPILOT_PANEL::ResetConversation()
{
    if( !m_session )
        return;

    m_session->Reset();
    m_chunks.clear();
    m_totalIn = m_totalOut = 0;
    m_usage->SetLabel( wxEmptyString );
    showWelcome();
}


void COPILOT_PANEL::showWelcome()
{
    bool           visible = IsShownOnScreen();
    COPILOT_CONFIG cfg = visible ? currentConfig() : COPILOT_SETTINGS_UTILS::CurrentConfig( false );

    std::string html = m_welcomeHtml.IsEmpty()
            ? std::string( "<b>KiCad Copilot</b><br>"
                           "Ask questions about the open design, request a design review, get "
                           "part and footprint suggestions, or (with <i>Allow edits</i> on) have "
                           "it make changes." )
            : toUtf8( m_welcomeHtml );

    if( visible && CopilotProviderNeedsKey( cfg.provider ) && cfg.apiKey.empty() )
    {
        html += "<br><br><font color=\"#d08000\">No API key is set for "
                + CopilotHtmlEscape( toUtf8( COPILOT_SETTINGS_UTILS::ProviderDisplayName(
                        cfg.provider ) ) )
                + ". Click <b>Settings...</b> to add one, or choose a local provider such as "
                  "Ollama.</font>";
    }

    m_chunks.clear();
    append( CHUNK_KIND::INFO, html );
}


void COPILOT_PANEL::updateStatus()
{
    COPILOT_CONFIG                    cfg = COPILOT_SETTINGS_UTILS::CurrentConfig( false );
    std::unique_ptr<COPILOT_PROVIDER> provider = COPILOT_PROVIDER::Create( cfg );

    wxString model = wxString::FromUTF8( provider->Model() );

    if( model.IsEmpty() )
        model = _( "(no model set)" );

    m_status->SetLabel( COPILOT_SETTINGS_UTILS::ProviderDisplayName( cfg.provider ) + wxS( " · " )
                        + model );
    m_status->SetToolTip( wxString::FromUTF8( provider->BaseUrl() ) );
}


void COPILOT_PANEL::append( CHUNK_KIND aKind, const std::string& aHtml )
{
    m_chunks.push_back( { aKind, aHtml } );
    render();
}


void COPILOT_PANEL::render()
{
    bool dark = KIPLATFORM::UI::IsDarkTheme();

    const char* userBg = dark ? "#2d3b4e" : "#e6eef8";
    const char* errFg = dark ? "#ff7b72" : "#b00020";
    const char* toolFg = dark ? "#9da5b0" : "#6a737d";

    std::string page;
    bool        inToolGroup = false;

    for( const CHUNK& chunk : m_chunks )
    {
        // Consecutive tool lines share one compact paragraph.
        if( chunk.kind == CHUNK_KIND::TOOL && !inToolGroup )
        {
            page += "<p>";
            inToolGroup = true;
        }
        else if( chunk.kind != CHUNK_KIND::TOOL && inToolGroup )
        {
            page += "</p>";
            inToolGroup = false;
        }

        switch( chunk.kind )
        {
        case CHUNK_KIND::USER:
            page += std::string( "<table width=\"100%\" cellpadding=\"5\" cellspacing=\"0\"><tr>"
                                 "<td bgcolor=\"" ) + userBg + "\">" + chunk.html
                    + "</td></tr></table>";
            break;

        case CHUNK_KIND::ASSISTANT:
            page += "<div>" + chunk.html + "</div>";
            break;

        case CHUNK_KIND::TOOL:
            page += std::string( "<font size=\"-1\" color=\"" ) + toolFg + "\">" + chunk.html
                    + "</font><br>";
            break;

        case CHUNK_KIND::ERROR_MSG:
            page += std::string( "<p><font color=\"" ) + errFg + "\">" + chunk.html
                    + "</font></p>";
            break;

        case CHUNK_KIND::INFO:
            page += "<p>" + chunk.html + "</p>";
            break;
        }
    }

    if( inToolGroup )
        page += "</p>";

    if( m_busy )
        page += std::string( "<p><font color=\"" ) + toolFg + "\"><i>"
                + toUtf8( _( "Thinking..." ) ) + "</i></font></p>";

    page += "<a name=\"end\"></a>";

    m_transcript->Freeze();
    m_transcript->SetPage( wxString::FromUTF8( CopilotSanitizeUtf8( page ) ) );

    // Scroll to the bottom.
    int x = 0, y = 0;
    m_transcript->GetVirtualSize( &x, &y );
    int ppuX = 1, ppuY = 1;
    m_transcript->GetScrollPixelsPerUnit( &ppuX, &ppuY );

    if( ppuY > 0 )
        m_transcript->Scroll( -1, y / ppuY );

    m_transcript->Thaw();
}


void COPILOT_PANEL::submit()
{
    wxString text = m_input->GetValue();
    text.Trim().Trim( false );

    if( text.IsEmpty() || m_busy || !m_session )
        return;

    // Pick up settings changes made elsewhere (e.g. in another editor's Copilot pane).
    m_config.reset();
    updateStatus();

    m_lastPrompt = text;

    if( m_chunks.size() == 1 && m_chunks[0].kind == CHUNK_KIND::INFO )
        m_chunks.clear();

    std::string escaped = CopilotHtmlEscape( toUtf8( text ) );
    std::string withBreaks;

    for( char c : escaped )
        withBreaks += ( c == '\n' ) ? std::string( "<br>" ) : std::string( 1, c );

    append( CHUNK_KIND::USER, withBreaks );
    m_input->Clear();

    m_session->Send( toUtf8( text ) );
}


void COPILOT_PANEL::onSend( wxCommandEvent& aEvent )
{
    if( m_busy )
    {
        if( m_session )
            m_session->Cancel();

        append( CHUNK_KIND::INFO, "<i>" + toUtf8( _( "Stopped." ) ) + "</i>" );
        return;
    }

    submit();
}


void COPILOT_PANEL::onInputKey( wxKeyEvent& aEvent )
{
    int key = aEvent.GetKeyCode();

    if( ( key == WXK_RETURN || key == WXK_NUMPAD_ENTER ) && !aEvent.ShiftDown() )
    {
        submit();
        return;
    }

    if( key == WXK_UP && m_input->GetValue().IsEmpty() && !m_lastPrompt.IsEmpty() )
    {
        m_input->SetValue( m_lastPrompt );
        m_input->SetInsertionPointEnd();
        return;
    }

    if( key == WXK_ESCAPE && m_busy && m_session )
    {
        m_session->Cancel();
        append( CHUNK_KIND::INFO, "<i>" + toUtf8( _( "Stopped." ) ) + "</i>" );
        return;
    }

    aEvent.Skip();
}


void COPILOT_PANEL::onNewChat( wxCommandEvent& aEvent )
{
    ResetConversation();
}


void COPILOT_PANEL::onSettings( wxCommandEvent& aEvent )
{
    DIALOG_COPILOT_SETTINGS dlg( this );

    if( dlg.ShowModal() == wxID_OK )
    {
        m_config.reset();
        updateStatus();

        if( !m_busy && m_session && m_session->History().empty() )
            showWelcome();
    }
}


void COPILOT_PANEL::onAllowEdits( wxCommandEvent& aEvent )
{
    bool allow = m_allowEdits->GetValue();

    if( m_session )
        m_session->SetAllowEdits( allow );

    if( COMMON_SETTINGS* common = Pgm().GetCommonSettings() )
        common->m_Copilot.allow_edits = allow;
}


void COPILOT_PANEL::onLinkClicked( wxHtmlLinkEvent& aEvent )
{
    wxString href = aEvent.GetLinkInfo().GetHref();

    if( href.StartsWith( wxS( "http://" ) ) || href.StartsWith( wxS( "https://" ) ) )
        wxLaunchDefaultBrowser( href );
}


void COPILOT_PANEL::onThemeChanged( wxSysColourChangedEvent& aEvent )
{
    render();
    aEvent.Skip();
}


void COPILOT_PANEL::OnCopilotBusy( bool aBusy )
{
    m_busy = aBusy;
    m_sendButton->SetLabel( aBusy ? _( "Stop" ) : _( "Send" ) );

    for( wxButton* btn : m_quickButtons )
        btn->Enable( !aBusy );

    m_newChatButton->Enable( !aBusy );
    render();
}


void COPILOT_PANEL::OnCopilotAssistantText( const std::string& aMarkdown )
{
    append( CHUNK_KIND::ASSISTANT, CopilotMarkdownToHtml( aMarkdown ) );
}


void COPILOT_PANEL::OnCopilotToolUsed( const COPILOT_TOOL_CALL& aCall,
                                       const COPILOT_TOOL_RESULT& aResult )
{
    std::string line = "&#8226; <b>" + CopilotHtmlEscape( aCall.name ) + "</b>";

    if( !aResult.summary.empty() )
        line += " &mdash; " + CopilotHtmlEscape( aResult.summary );

    if( aResult.isError )
        line += " <i>(" + toUtf8( _( "failed" ) ) + ")</i>";

    append( CHUNK_KIND::TOOL, line );
}


void COPILOT_PANEL::OnCopilotError( const std::string& aMessage )
{
    append( CHUNK_KIND::ERROR_MSG, CopilotHtmlEscape( aMessage ) );
}


bool COPILOT_PANEL::OnCopilotApproval( const COPILOT_TOOL_CALL& aCall,
                                       const std::string& aDescription, bool* aAlwaysAllow )
{
    wxString msg = wxString::Format( _( "The Copilot wants to run '%s':" ),
                                     wxString::FromUTF8( aCall.name ) );

    // Some actions (running arbitrary commands) must be approved every time.
    long style = wxICON_QUESTION | wxCENTRE | wxYES_NO | ( aAlwaysAllow ? wxCANCEL : 0 );

    wxMessageDialog dlg( wxGetTopLevelParent( this ), msg, _( "Copilot Approval" ), style );
    dlg.SetExtendedMessage( wxString::FromUTF8( CopilotSanitizeUtf8( aDescription ) ) );

    if( aAlwaysAllow )
        dlg.SetYesNoCancelLabels( _( "Allow" ), _( "Always Allow This Session" ), _( "Deny" ) );
    else
        dlg.SetYesNoLabels( _( "Allow" ), _( "Deny" ) );

    int rc = dlg.ShowModal();

    if( aAlwaysAllow && rc == wxID_NO )
    {
        *aAlwaysAllow = true;
        append( CHUNK_KIND::INFO, "<i>" + CopilotHtmlEscape( toUtf8(
                wxString::Format( _( "Allowed '%s' for this session." ),
                                  wxString::FromUTF8( aCall.name ) ) ) ) + "</i>" );
        return true;
    }

    return rc == wxID_YES;
}


void COPILOT_PANEL::OnCopilotWaiting( const std::string& aReason )
{
    append( CHUNK_KIND::INFO, "<i>" + CopilotHtmlEscape( aReason ) + "</i>" );
}


void COPILOT_PANEL::OnCopilotUsage( int aInputTokens, int aOutputTokens )
{
    m_totalIn += aInputTokens;
    m_totalOut += aOutputTokens;

    if( m_totalIn || m_totalOut )
    {
        m_usage->SetLabel( wxString::Format( _( "%lld in / %lld out tokens" ), m_totalIn,
                                             m_totalOut ) );
    }
}
