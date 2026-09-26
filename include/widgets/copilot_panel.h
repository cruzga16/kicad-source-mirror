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

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <wx/panel.h>

#include <copilot/copilot_session.h>

class HTML_WINDOW;
class wxButton;
class wxCheckBox;
class wxStaticText;
class wxTextCtrl;
class wxHtmlLinkEvent;


/**
 * Dockable chat pane for the KiCad Copilot.  Editor independent: the editor supplies a
 * COPILOT_TOOLBOX that exposes its document.
 */
class COPILOT_PANEL : public wxPanel, public COPILOT_SESSION_LISTENER
{
public:
    COPILOT_PANEL( wxWindow* aParent, COPILOT_TOOLBOX* aToolbox );
    ~COPILOT_PANEL() override;

    /// Put keyboard focus in the prompt box.
    void FocusInput();

    /// Send a prompt as though the user typed it (used by editor actions).
    void SendPrompt( const wxString& aPrompt );

    /// Forget the conversation (e.g. when a different document is loaded).
    void ResetConversation();

    /// Detach from the editor; call before the toolbox is destroyed.
    void DetachToolbox();

    // COPILOT_SESSION_LISTENER
    void OnCopilotBusy( bool aBusy ) override;
    void OnCopilotAssistantText( const std::string& aMarkdown ) override;
    void OnCopilotToolUsed( const COPILOT_TOOL_CALL& aCall,
                            const COPILOT_TOOL_RESULT& aResult ) override;
    void OnCopilotError( const std::string& aMessage ) override;
    void OnCopilotUsage( int aInputTokens, int aOutputTokens ) override;
    bool OnCopilotApproval( const COPILOT_TOOL_CALL& aCall, const std::string& aDescription,
                            bool* aAlwaysAllow ) override;
    void OnCopilotWaiting( const std::string& aReason ) override;

    /// Title shown in the welcome text (e.g. "KiCad Copilot — Project").
    void SetWelcomeText( const wxString& aHtml ) { m_welcomeHtml = aHtml; showWelcome(); }

private:
    void onSend( wxCommandEvent& aEvent );
    void onInputKey( wxKeyEvent& aEvent );
    void onNewChat( wxCommandEvent& aEvent );
    void onSettings( wxCommandEvent& aEvent );
    void onAllowEdits( wxCommandEvent& aEvent );
    void onLinkClicked( wxHtmlLinkEvent& aEvent );
    void onThemeChanged( wxSysColourChangedEvent& aEvent );

    enum class CHUNK_KIND
    {
        USER,
        ASSISTANT,
        TOOL,
        ERROR_MSG,
        INFO
    };

    struct CHUNK
    {
        CHUNK_KIND  kind;
        std::string html;   ///< Inner HTML (already escaped/converted).
    };

    void submit();
    void append( CHUNK_KIND aKind, const std::string& aHtml );
    void render();
    void updateStatus();
    void showWelcome();

    COPILOT_CONFIG currentConfig();

    COPILOT_TOOLBOX*                 m_toolbox;
    std::shared_ptr<COPILOT_SESSION> m_session;
    std::optional<COPILOT_CONFIG>    m_config;      ///< Cached; cleared when settings change.

    HTML_WINDOW*                     m_transcript;
    wxTextCtrl*                      m_input;
    wxButton*                        m_sendButton;
    wxButton*                        m_newChatButton;
    wxButton*                        m_settingsButton;
    wxCheckBox*                      m_allowEdits;
    wxStaticText*                    m_status;
    wxStaticText*                    m_usage;
    std::vector<wxButton*>           m_quickButtons;

    std::vector<CHUNK>               m_chunks;      ///< Transcript, rendered on demand.
    bool                             m_busy = false;
    long long                        m_totalIn = 0;
    long long                        m_totalOut = 0;
    wxString                         m_lastPrompt;
    wxString                         m_welcomeHtml;
};
