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

#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <copilot/copilot_provider.h>

class wxTimer;


/**
 * Result of executing a copilot tool.  The payload is fed back to the model verbatim, so
 * it should be compact JSON (or plain text for errors).
 */
struct COPILOT_TOOL_RESULT
{
    std::string                     content;
    bool                            isError = false;
    std::string                     summary;    ///< Short line shown in the chat transcript.
    std::vector<COPILOT_ATTACHMENT> attachments;

    static COPILOT_TOOL_RESULT Ok( const nlohmann::json& aJson, const std::string& aSummary = "" )
    {
        COPILOT_TOOL_RESULT r;
        r.content = aJson.dump( -1, ' ', false, nlohmann::json::error_handler_t::replace );
        r.summary = aSummary;
        return r;
    }

    static COPILOT_TOOL_RESULT Error( const std::string& aMessage )
    {
        COPILOT_TOOL_RESULT r;
        r.content = aMessage;
        r.isError = true;
        r.summary = aMessage.size() > 160 ? CopilotTruncateUtf8( aMessage, 157 ) + "..."
                                          : aMessage;
        return r;
    }
};


/**
 * Editor specific half of the copilot: exposes the open design to the model.
 *
 * All methods are called on the GUI thread, so implementations may freely touch the
 * document model, the tool manager and the view.
 */
class COPILOT_TOOLBOX
{
public:
    virtual ~COPILOT_TOOLBOX() = default;

    /// Editor-specific part of the system prompt (what the tools do, conventions, units...).
    virtual std::string GetSystemPrompt() = 0;

    /// Tools to advertise.  Tools with modifiesDesign should be omitted if !aAllowEdits.
    virtual std::vector<COPILOT_TOOL_DEF> GetTools( bool aAllowEdits ) = 0;

    /// A short snapshot of volatile context (file, selection) prepended to each user message.
    virtual std::string GetContextSnapshot() = 0;

    virtual COPILOT_TOOL_RESULT ExecuteTool( const std::string& aName,
                                             const nlohmann::json& aInput ) = 0;

    /// Canned prompts offered as quick-action buttons: {label, prompt}.
    virtual std::vector<std::pair<std::string, std::string>> GetQuickActions() { return {}; }

    /// Human readable description of a call, shown when asking the user for approval.
    virtual std::string DescribeToolCall( const std::string& aName, const nlohmann::json& aInput )
    {
        return aName + " " + aInput.dump( 2 );
    }

    /// Maximum model round trips per user turn.  Orchestrators that build whole projects
    /// need many more than an editor-local assistant.
    virtual int MaxToolRounds() { return 24; }

    /**
     * Whether tools may run right now.  Tools must not run while the user is in the middle of
     * something (a modal dialog, an interactive tool such as the router); the session waits
     * and retries.  The default checks that no nested event loop (modal dialog) is running.
     *
     * @param aWhy set to a short explanation shown to the user while waiting.
     */
    virtual bool IsReadyForTools( std::string& aWhy );

    /// Look up a tool definition by name (default: search GetTools( true )).
    virtual bool FindTool( const std::string& aName, COPILOT_TOOL_DEF& aDef );

    /// Key under which "always allow" is remembered for a call needing approval.  Tools that
    /// reach different targets (e.g. web hosts) can make the approval per target.
    virtual std::string ApprovalKey( const std::string& aName, const nlohmann::json& aInput )
    {
        return aName;
    }

    /// Whether the user currently allows edits; set before each tool runs so read-only
    /// tools with optional side effects can respect it.
    void SetEditsAllowed( bool aAllowed ) { m_editsAllowed = aAllowed; }
    bool EditsAllowed() const { return m_editsAllowed; }

protected:
    bool m_editsAllowed = false;
};


/// True when called from the application's main event loop with nothing else in progress (no
/// modal dialog, no operation yielding to the event loop).
bool CopilotInMainEventLoop();


/**
 * Receives progress from a COPILOT_SESSION.  Always called on the GUI thread.
 */
class COPILOT_SESSION_LISTENER
{
public:
    virtual ~COPILOT_SESSION_LISTENER() = default;

    virtual void OnCopilotBusy( bool aBusy ) = 0;
    virtual void OnCopilotAssistantText( const std::string& aMarkdown ) = 0;
    virtual void OnCopilotToolUsed( const COPILOT_TOOL_CALL& aCall,
                                    const COPILOT_TOOL_RESULT& aResult ) = 0;
    virtual void OnCopilotError( const std::string& aMessage ) = 0;
    virtual void OnCopilotUsage( int aInputTokens, int aOutputTokens ) {}

    /**
     * Ask the user whether a tool that needs approval may run.
     *
     * @param aAlwaysAllow set to true if the user allowed this tool for the rest of the session;
     *                     null if the tool must be approved every time.
     * @return true to run the tool.
     */
    virtual bool OnCopilotApproval( const COPILOT_TOOL_CALL& aCall, const std::string& aDescription,
                                    bool* aAlwaysAllow )
    {
        return false;
    }

    /// The session is waiting before running tools (e.g. for a dialog to close).
    virtual void OnCopilotWaiting( const std::string& aReason ) {}
};


/**
 * A single conversation with the model, including the tool-use loop.
 *
 * Network requests run on a private worker thread; responses are marshalled back to the GUI
 * thread where tools are executed against the design.  The session must be owned by a
 * std::shared_ptr (the worker holds a weak reference to it).
 */
class COPILOT_SESSION : public std::enable_shared_from_this<COPILOT_SESSION>
{
public:
    using CONFIG_PROVIDER = std::function<COPILOT_CONFIG()>;

    /// Hook used by tests to replace the network layer.
    using TRANSPORT = std::function<COPILOT_RESPONSE( const COPILOT_PROVIDER&,
                                                      const COPILOT_REQUEST&,
                                                      const std::function<bool()>& aCancelled )>;

    /// Hook used by tests to replace posting to the GUI thread.
    using DISPATCHER = std::function<void( std::function<void()> )>;

    /// Hook used by tests to replace the retry timer: run the callback after aMs milliseconds.
    using SCHEDULER = std::function<void( int aMs, std::function<void()> )>;

    static std::shared_ptr<COPILOT_SESSION> Create( COPILOT_TOOLBOX* aToolbox,
                                                    CONFIG_PROVIDER aConfigProvider );

    ~COPILOT_SESSION();

    void SetListener( COPILOT_SESSION_LISTENER* aListener ) { m_listener = aListener; }

    /// Detach from the toolbox (e.g. when the owning editor is being destroyed).
    void DetachToolbox() { m_toolbox = nullptr; }

    void SetAllowEdits( bool aAllow ) { m_allowEdits = aAllow; }
    bool GetAllowEdits() const { return m_allowEdits; }

    void SetTransport( TRANSPORT aTransport ) { m_transport = std::move( aTransport ); }
    void SetDispatcher( DISPATCHER aDispatcher ) { m_dispatcher = std::move( aDispatcher ); }
    void SetScheduler( SCHEDULER aScheduler ) { m_scheduler = std::move( aScheduler ); }

    /// Start a new user turn.  Returns false if a turn is already running.
    bool Send( const std::string& aUserText );

    /// Abort the running turn (the in-flight request is dropped).
    void Cancel();

    /// Forget the conversation (and any "always allow" approvals).
    void Reset();

    bool IsBusy() const { return m_busy; }

    /// True while one of this session's tools is executing.
    bool IsExecutingTool() const { return m_executing; }

    const std::vector<COPILOT_MESSAGE>& History() const { return m_history; }

    /// Default maximum model round trips per user turn (see COPILOT_TOOLBOX::MaxToolRounds()).
    static constexpr int MAX_TOOL_ROUNDS = 24;

    /// The most recent tool results sent in full; older ones are shortened to save context.
    static constexpr size_t KEEP_FULL_TOOL_RESULTS = 10;

    /// Older results are shortened in batches of this many so the request prefix (and with it
    /// the provider's prompt cache) stays stable between batches.
    static constexpr size_t COMPACT_BATCH = 12;

    /// Length older tool results are shortened to.
    static constexpr size_t COMPACT_RESULT_CHARS = 1200;

    /// Tool results longer than this are truncated before being sent to the model.
    static constexpr size_t MAX_TOOL_RESULT_CHARS = 24000;

private:
    COPILOT_SESSION( COPILOT_TOOLBOX* aToolbox, CONFIG_PROVIDER aConfigProvider );

    void startRequest();
    bool approve( const COPILOT_TOOL_CALL& aCall, const COPILOT_TOOL_DEF& aDef,
                  const std::string& aKey );
    void onResponse( unsigned aGeneration, const COPILOT_RESPONSE& aResponse );
    void runPendingTools( unsigned aGeneration );
    COPILOT_TOOL_RESULT executeCall( unsigned aGeneration, const COPILOT_TOOL_CALL& aCall );
    bool toolsReady( std::string& aWhy );
    void retryLater( unsigned aGeneration, const std::string& aWhy );
    void finishTurn();
    void closeDanglingToolCalls( const std::string& aReason );
    void trimHistory();
    void advanceCompaction();
    bool stripAttachments( const std::string& aError );
    std::vector<COPILOT_MESSAGE> compactedHistory() const;
    void joinWorker();

    COPILOT_TOOLBOX*             m_toolbox;
    COPILOT_SESSION_LISTENER*    m_listener = nullptr;
    CONFIG_PROVIDER              m_configProvider;
    TRANSPORT                    m_transport;
    DISPATCHER                   m_dispatcher;
    SCHEDULER                    m_scheduler;

    std::vector<COPILOT_MESSAGE> m_history;
    size_t                       m_turnStart = 0;   ///< History index where this turn began.
    int                          m_rounds = 0;
    bool                         m_busy = false;
    bool                         m_allowEdits = false;
    std::set<std::string>        m_alwaysAllowed;   ///< Approval keys allowed for the session.

    std::vector<COPILOT_TOOL_CALL> m_pendingCalls;  ///< Tool calls of the current response.
    size_t                         m_nextCall = 0;
    bool                           m_executing = false;
    unsigned                       m_executingGeneration = 0;
    std::string                    m_waitReason;
    std::unique_ptr<wxTimer>       m_retryTimer;
    size_t                         m_compactUpTo = 0;   ///< Results before this are shortened.
    bool                           m_attachmentRetry = false;

    unsigned                           m_generation = 0;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::thread                        m_worker;
};


/**
 * Perform the HTTP round trip for a request using KiCad's curl wrapper.  Blocking; call from
 * a worker thread.  @a aCancelled is polled during the transfer.
 */
COPILOT_RESPONSE CopilotHttpTransport( const COPILOT_PROVIDER& aProvider,
                                       const COPILOT_REQUEST& aRequest,
                                       const std::function<bool()>& aCancelled );
