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

#include <copilot/copilot_session.h>
#include <copilot/copilot_shared_state.h>

#include <algorithm>
#include <map>

#include <wx/app.h>
#include <wx/evtloop.h>
#include <wx/log.h>
#include <wx/timer.h>


namespace
{
/// Sets the session's "executing" flag for the lifetime of the object.
struct EXECUTING_FLAG
{
    EXECUTING_FLAG( bool& aFlag, unsigned& aGenSlot, unsigned aGeneration ) :
            m_flag( aFlag )
    {
        m_flag = true;
        aGenSlot = aGeneration;
    }

    ~EXECUTING_FLAG() { m_flag = false; }

    bool& m_flag;
};


/// One-shot timer that hands its callback to the event loop (so the timer is never on the
/// stack while the callback runs).
class COPILOT_RETRY_TIMER : public wxTimer
{
public:
    std::function<void()> m_callback;

    void Notify() override
    {
        if( m_callback && wxTheApp )
            wxTheApp->CallAfter( m_callback );
    }
};
}


bool CopilotInMainEventLoop()
{
    if( !wxTheApp )
        return true;

    wxEventLoopBase* active = wxEventLoopBase::GetActive();
    wxEventLoopBase* main = wxTheApp->GetMainLoop();

    // Without a main loop (tests, early startup) there is nothing to protect.
    if( !active || !main )
        return true;

    // A nested loop means a modal dialog; a yield means some operation (auto-placement, a
    // progress dialog, ...) is pumping events from the middle of its work.
    return active == main && !active->IsYielding();
}


bool COPILOT_TOOLBOX::IsReadyForTools( std::string& aWhy )
{
    if( !CopilotInMainEventLoop() )
    {
        aWhy = "Waiting for the open dialog or running operation to finish before "
               "continuing...";
        return false;
    }

    return true;
}


bool COPILOT_TOOLBOX::FindTool( const std::string& aName, COPILOT_TOOL_DEF& aDef )
{
    for( const COPILOT_TOOL_DEF& def : GetTools( true ) )
    {
        if( def.name == aName )
        {
            aDef = def;
            return true;
        }
    }

    return false;
}


static const char* const COPILOT_BASE_PROMPT =
        "You are KiCad Copilot, an assistant embedded in the KiCad electronics design suite. "
        "You help an engineer understand, review and edit the design that is open in the "
        "editor.\n\n"
        "Guidelines:\n"
        "- Ground every statement about the design in data from your tools. Never invent "
        "reference designators, net names, values or coordinates; look them up.\n"
        "- Prefer a few targeted tool calls over dumping everything. List tools accept filters "
        "and limits.\n"
        "- All lengths and coordinates exchanged with tools are in millimetres unless a tool "
        "says otherwise. Angles are in degrees.\n"
        "- When reviewing a design, organise findings by severity (problem, warning, "
        "suggestion), cite the exact references/nets involved, and explain the engineering "
        "reason briefly.\n"
        "- When suggesting parts, give concrete values, ratings and package sizes, name an "
        "example manufacturer part number only when you are confident it exists, and mention "
        "matching KiCad library footprints found with the footprint search tool. Remind the "
        "user to verify part numbers against the manufacturer datasheet.\n"
        "- Only modify the design when the user asks you to. Every edit you make is a separate "
        "undoable step; tell the user what you changed. If editing tools are not available, "
        "editing is switched off in the Copilot panel; explain what you would change instead.\n"
        "- Be concise. Use short paragraphs, bullet lists and `code` formatting for references "
        "and net names. Markdown is rendered.\n";


std::shared_ptr<COPILOT_SESSION> COPILOT_SESSION::Create( COPILOT_TOOLBOX* aToolbox,
                                                          CONFIG_PROVIDER aConfigProvider )
{
    return std::shared_ptr<COPILOT_SESSION>( new COPILOT_SESSION( aToolbox,
                                                                  std::move( aConfigProvider ) ) );
}


COPILOT_SESSION::COPILOT_SESSION( COPILOT_TOOLBOX* aToolbox, CONFIG_PROVIDER aConfigProvider ) :
        m_toolbox( aToolbox ),
        m_configProvider( std::move( aConfigProvider ) ),
        m_transport( &CopilotHttpTransport ),
        m_cancel( std::make_shared<std::atomic<bool>>( false ) )
{
    m_dispatcher = []( std::function<void()> aFn )
    {
        if( wxTheApp )
            wxTheApp->CallAfter( std::move( aFn ) );
    };
}


COPILOT_SESSION::~COPILOT_SESSION()
{
    if( m_retryTimer )
        m_retryTimer->Stop();

    m_listener = nullptr;
    m_cancel->store( true );
    joinWorker();
}


void COPILOT_SESSION::joinWorker()
{
    if( m_worker.joinable() )
        m_worker.join();
}


bool COPILOT_SESSION::Send( const std::string& aUserText )
{
    if( m_busy )
        return false;

    joinWorker();
    trimHistory();

    std::string text = aUserText;

    if( m_toolbox )
    {
        std::string ctx = m_toolbox->GetContextSnapshot();

        if( !ctx.empty() )
            text = "<editor_context>\n" + ctx + "\n</editor_context>\n\n" + aUserText;
    }

    m_turnStart = m_history.size();
    m_rounds = 0;
    m_attachmentRetry = false;
    m_pendingCalls.clear();
    m_nextCall = 0;
    m_waitReason.clear();
    m_history.push_back( COPILOT_MESSAGE::User( text ) );

    m_busy = true;

    if( m_listener )
        m_listener->OnCopilotBusy( true );

    startRequest();
    return true;
}


void COPILOT_SESSION::Cancel()
{
    if( !m_busy )
        return;

    m_cancel->store( true );
    ++m_generation;         // drop whatever the worker (or a running tool) eventually delivers

    if( m_retryTimer )
        m_retryTimer->Stop();

    m_pendingCalls.clear();
    m_nextCall = 0;
    m_waitReason.clear();

    closeDanglingToolCalls( "Cancelled by the user." );

    // If the model never answered, forget the question so history stays well formed.
    if( m_history.size() == m_turnStart + 1
        && m_history.back().role == COPILOT_MESSAGE::ROLE::USER )
    {
        m_history.pop_back();
    }

    finishTurn();
}


void COPILOT_SESSION::Reset()
{
    Cancel();
    m_history.clear();
    m_turnStart = 0;
    m_compactUpTo = 0;
    m_alwaysAllowed.clear();
}


void COPILOT_SESSION::finishTurn()
{
    m_busy = false;

    if( m_listener )
        m_listener->OnCopilotBusy( false );
}


void COPILOT_SESSION::closeDanglingToolCalls( const std::string& aReason )
{
    if( m_history.empty() )
        return;

    // Find the last assistant message and make sure each of its tool calls has a result.
    size_t idx = m_history.size();

    while( idx > 0 && m_history[idx - 1].role == COPILOT_MESSAGE::ROLE::TOOL_RESULT )
        --idx;

    if( idx == 0 || m_history[idx - 1].role != COPILOT_MESSAGE::ROLE::ASSISTANT )
        return;

    const COPILOT_MESSAGE assistant = m_history[idx - 1];

    for( const COPILOT_TOOL_CALL& call : assistant.toolCalls )
    {
        bool answered = false;

        for( size_t i = idx; i < m_history.size(); ++i )
        {
            if( m_history[i].toolCallId == call.id )
                answered = true;
        }

        if( !answered )
            m_history.push_back( COPILOT_MESSAGE::ToolResult( call, aReason, true ) );
    }
}


void COPILOT_SESSION::trimHistory()
{
    const size_t maxMessages = 80;
    const size_t keepMessages = 50;

    if( m_history.size() <= maxMessages )
        return;

    // Cut at the start of a user turn so tool calls stay paired with their results.
    for( size_t i = m_history.size() - keepMessages; i < m_history.size(); ++i )
    {
        if( m_history[i].role == COPILOT_MESSAGE::ROLE::USER )
        {
            m_history.erase( m_history.begin(), m_history.begin() + i );
            m_compactUpTo = m_compactUpTo > i ? m_compactUpTo - i : 0;
            return;
        }
    }
}


bool COPILOT_SESSION::approve( const COPILOT_TOOL_CALL& aCall, const COPILOT_TOOL_DEF& aDef,
                               const std::string& aKey )
{
    if( !m_listener )
        return false;

    std::string description = m_toolbox ? m_toolbox->DescribeToolCall( aCall.name, aCall.input )
                                        : aCall.name;
    bool        always = false;
    bool        ok = m_listener->OnCopilotApproval( aCall, description,
                                                    aDef.allowAlways ? &always : nullptr );

    if( ok && always && aDef.allowAlways )
        m_alwaysAllowed.insert( aKey );

    return ok;
}


void COPILOT_SESSION::advanceCompaction()
{
    size_t full = 0;

    for( size_t i = m_compactUpTo; i < m_history.size(); ++i )
    {
        if( m_history[i].role == COPILOT_MESSAGE::ROLE::TOOL_RESULT )
            ++full;
    }

    if( full <= KEEP_FULL_TOOL_RESULTS + COMPACT_BATCH )
        return;

    // Move the watermark so that exactly KEEP_FULL_TOOL_RESULTS full results remain.
    size_t kept = 0;

    for( size_t i = m_history.size(); i > m_compactUpTo; --i )
    {
        if( m_history[i - 1].role == COPILOT_MESSAGE::ROLE::TOOL_RESULT
            && ++kept == KEEP_FULL_TOOL_RESULTS )
        {
            m_compactUpTo = i - 1;
            return;
        }
    }
}


std::vector<COPILOT_MESSAGE> COPILOT_SESSION::compactedHistory() const
{
    std::vector<COPILOT_MESSAGE> msgs = m_history;

    auto shorten =
            []( COPILOT_MESSAGE& aMsg, size_t aLimit )
            {
                aMsg.attachments.clear();

                if( aMsg.toolResult.size() > aLimit )
                {
                    aMsg.toolResult = CopilotTruncateUtf8( aMsg.toolResult, aLimit )
                                      + "...[older result shortened; call the tool again if "
                                        "needed]";
                }
            };

    // Results before the watermark are shortened.  The watermark only moves in batches, so the
    // request prefix stays identical from one round to the next (keeps prompt caching useful).
    for( size_t i = 0; i < msgs.size() && i < m_compactUpTo; ++i )
    {
        if( msgs[i].role == COPILOT_MESSAGE::ROLE::TOOL_RESULT )
            shorten( msgs[i], COPILOT_SESSION::COMPACT_RESULT_CHARS );
    }

    size_t totalChars = 0;

    for( const COPILOT_MESSAGE& m : msgs )
        totalChars += m.text.size() + m.toolResult.size();

    // Emergency measure for enormous conversations: shorten everything but the newest results.
    if( totalChars > 600000 )
    {
        size_t seen = 0;

        for( auto it = msgs.rbegin(); it != msgs.rend(); ++it )
        {
            if( it->role == COPILOT_MESSAGE::ROLE::TOOL_RESULT && ++seen > KEEP_FULL_TOOL_RESULTS )
                shorten( *it, 300 );
        }
    }

    return msgs;
}


bool COPILOT_SESSION::stripAttachments( const std::string& aError )
{
    bool stripped = false;

    for( COPILOT_MESSAGE& msg : m_history )
    {
        if( msg.attachments.empty() )
            continue;

        msg.attachments.clear();
        msg.toolResult += "\n[The attached document was rejected by the model provider ("
                          + CopilotTruncateUtf8( aError, 200 )
                          + "). Use fetch_url to read its text instead.]";
        stripped = true;
    }

    return stripped;
}


void COPILOT_SESSION::startRequest()
{
    if( !m_toolbox )
    {
        if( m_listener )
            m_listener->OnCopilotError( "The editor is no longer available." );

        finishTurn();
        return;
    }

    COPILOT_CONFIG config = m_configProvider ? m_configProvider() : COPILOT_CONFIG();

    if( CopilotProviderNeedsKey( config.provider ) && config.apiKey.empty() )
    {
        if( m_history.size() == m_turnStart + 1 )
            m_history.pop_back();

        if( m_listener )
        {
            m_listener->OnCopilotError( "No API key is configured for "
                                        + CopilotProviderId( config.provider )
                                        + ". Open the Copilot settings (gear button) to add one." );
        }

        finishTurn();
        return;
    }

    COPILOT_REQUEST request;
    request.system = std::string( COPILOT_BASE_PROMPT ) + "\n" + m_toolbox->GetSystemPrompt();
    advanceCompaction();
    request.messages = compactedHistory();
    request.tools = m_toolbox->GetTools( m_allowEdits );

    std::shared_ptr<COPILOT_PROVIDER> provider = COPILOT_PROVIDER::Create( config );

    joinWorker();

    m_cancel = std::make_shared<std::atomic<bool>>( false );
    unsigned                           generation = ++m_generation;
    std::shared_ptr<std::atomic<bool>> cancel = m_cancel;
    std::weak_ptr<COPILOT_SESSION>     weakSelf = weak_from_this();
    TRANSPORT                          transport = m_transport;
    DISPATCHER                         dispatcher = m_dispatcher;

    m_worker = std::thread(
            [=]()
            {
                COPILOT_RESPONSE response;

                try
                {
                    response = transport( *provider, request,
                                          [cancel]() { return cancel->load(); } );
                }
                catch( const std::exception& e )
                {
                    response.ok = false;
                    response.error = std::string( "Request failed: " ) + e.what();
                }
                catch( ... )
                {
                    response.ok = false;
                    response.error = "Request failed with an unexpected error.";
                }

                if( cancel->load() )
                    return;

                dispatcher(
                        [weakSelf, generation, response]()
                        {
                            if( std::shared_ptr<COPILOT_SESSION> self = weakSelf.lock() )
                                self->onResponse( generation, response );
                        } );
            } );
}


void COPILOT_SESSION::onResponse( unsigned aGeneration, const COPILOT_RESPONSE& aResponse )
{
    if( aGeneration != m_generation || !m_busy )
        return;     // stale (cancelled or superseded)

    if( !aResponse.ok )
    {
        // A document attached to a tool result (e.g. an oversized PDF) can make the provider
        // reject the whole request; retry once without it.
        bool badRequest = aResponse.error.find( "HTTP 400" ) != std::string::npos
                          || aResponse.error.find( "HTTP 413" ) != std::string::npos;

        if( badRequest && !m_attachmentRetry && stripAttachments( aResponse.error ) )
        {
            m_attachmentRetry = true;

            if( m_listener )
                m_listener->OnCopilotWaiting( "The provider rejected an attached document; "
                                              "retrying without it." );

            startRequest();
            return;
        }

        // Leave the history well formed: drop an unanswered question so the user can retry.
        if( m_history.size() == m_turnStart + 1
            && m_history.back().role == COPILOT_MESSAGE::ROLE::USER )
        {
            m_history.pop_back();
        }

        if( m_listener )
            m_listener->OnCopilotError( aResponse.error );

        finishTurn();
        return;
    }

    m_history.push_back( aResponse.message );

    if( m_listener )
    {
        m_listener->OnCopilotUsage( aResponse.inputTokens, aResponse.outputTokens );

        if( !aResponse.message.text.empty() )
            m_listener->OnCopilotAssistantText( aResponse.message.text );
    }

    if( aResponse.message.toolCalls.empty() && aResponse.stopReason == "pause_turn" )
    {
        // A long-running server tool (web search) paused; resume by sending the turn again.
        if( ++m_rounds < std::max( 1, m_toolbox ? m_toolbox->MaxToolRounds() : MAX_TOOL_ROUNDS ) )
        {
            startRequest();
            return;
        }
    }

    if( aResponse.message.toolCalls.empty() )
    {
        if( aResponse.stopReason == "max_tokens" && m_listener )
        {
            m_listener->OnCopilotError( "The response was cut off (maximum output tokens "
                                        "reached). Increase the limit in Copilot settings." );
        }

        finishTurn();
        return;
    }

    m_pendingCalls = aResponse.message.toolCalls;
    m_nextCall = 0;
    runPendingTools( aGeneration );
}


bool COPILOT_SESSION::toolsReady( std::string& aWhy )
{
    if( COPILOT_SHARED::ToolRunning() )
    {
        aWhy = "Waiting for another Copilot action to finish...";
        return false;
    }

    if( m_toolbox && !m_toolbox->IsReadyForTools( aWhy ) )
        return false;

    return true;
}


void COPILOT_SESSION::retryLater( unsigned aGeneration, const std::string& aWhy )
{
    if( aWhy != m_waitReason )
    {
        m_waitReason = aWhy;

        if( m_listener && !aWhy.empty() )
            m_listener->OnCopilotWaiting( aWhy );
    }

    std::weak_ptr<COPILOT_SESSION> weakSelf = weak_from_this();

    auto retry = [weakSelf, aGeneration]()
                 {
                     if( std::shared_ptr<COPILOT_SESSION> self = weakSelf.lock() )
                         self->runPendingTools( aGeneration );
                 };

    if( m_scheduler )
    {
        m_scheduler( 300, retry );
        return;
    }

    if( !m_retryTimer )
        m_retryTimer = std::make_unique<COPILOT_RETRY_TIMER>();

    static_cast<COPILOT_RETRY_TIMER*>( m_retryTimer.get() )->m_callback = retry;
    m_retryTimer->StartOnce( 300 );
}


COPILOT_TOOL_RESULT COPILOT_SESSION::executeCall( unsigned aGeneration,
                                                  const COPILOT_TOOL_CALL& aCall )
{
    if( !m_toolbox )
        return COPILOT_TOOL_RESULT::Error( "The editor is no longer available." );

    // Everything below may pump events (tool lookup can open an editor, approval shows a
    // dialog, tools run programs); keep other sessions' tools from running meanwhile.
    COPILOT_TOOL_SCOPE scope;
    EXECUTING_FLAG     executing( m_executing, m_executingGeneration, aGeneration );

    COPILOT_TOOL_DEF def;

    // Looked up per call: an earlier call in the same response may have opened an editor
    // whose tools were not available when the response arrived.
    if( !m_toolbox->FindTool( aCall.name, def ) )
        return COPILOT_TOOL_RESULT::Error( "Unknown tool '" + aCall.name + "'." );

    if( aGeneration != m_generation || !m_busy || !m_toolbox )
        return COPILOT_TOOL_RESULT::Error( "Cancelled by the user." );

    if( def.modifiesDesign && !m_allowEdits )
    {
        return COPILOT_TOOL_RESULT::Error( "Editing is disabled. Ask the user to enable "
                                           "'Allow edits' in the Copilot panel." );
    }

    if( def.requiresApproval )
    {
        std::string key = m_toolbox->ApprovalKey( aCall.name, aCall.input );
        bool        preApproved = def.allowAlways && m_alwaysAllowed.count( key );

        if( !preApproved && !approve( aCall, def, key ) )
        {
            return COPILOT_TOOL_RESULT::Error( "The user declined to run this action. Ask what "
                                               "they would like to do instead." );
        }

        // The approval dialog runs a nested event loop; the turn may have ended meanwhile.
        if( aGeneration != m_generation || !m_busy || !m_toolbox )
            return COPILOT_TOOL_RESULT::Error( "Cancelled by the user." );
    }

    m_toolbox->SetEditsAllowed( m_allowEdits );

    try
    {
        return m_toolbox->ExecuteTool( aCall.name, aCall.input );
    }
    catch( const std::exception& e )
    {
        return COPILOT_TOOL_RESULT::Error( std::string( "Tool failed: " ) + e.what() );
    }
    catch( ... )
    {
        // Never let an exception escape into the event loop.
        return COPILOT_TOOL_RESULT::Error( "Tool failed with an unexpected error." );
    }
}


void COPILOT_SESSION::runPendingTools( unsigned aGeneration )
{
    // Nothing to do if a stale retry fires after the calls were processed.
    if( m_pendingCalls.empty() )
        return;

    if( m_executing )
    {
        // Re-entered while one of our tools pumps events.  The frame running that tool
        // carries on with this turn; a newer turn (started after Stop) has to wait for it.
        if( aGeneration != m_executingGeneration )
            retryLater( aGeneration, "Waiting for the previous action to finish..." );

        return;
    }

    while( m_nextCall < m_pendingCalls.size() )
    {
        if( aGeneration != m_generation || !m_busy )
            return;     // cancelled or superseded

        // While one of our own tools runs (and pumps events), toolsReady() is false because
        // COPILOT_SHARED::ToolRunning() is set, so re-entrant calls just reschedule.
        std::string why;

        if( !toolsReady( why ) )
        {
            retryLater( aGeneration, why );
            return;
        }

        m_waitReason.clear();

        COPILOT_TOOL_CALL   call = m_pendingCalls[m_nextCall++];
        COPILOT_TOOL_RESULT result = executeCall( aGeneration, call );

        // The user may have stopped the turn (or started a new one) while the tool ran.
        if( aGeneration != m_generation || !m_busy )
            return;

        std::string payload = CopilotSanitizeUtf8( result.content );

        if( payload.size() > MAX_TOOL_RESULT_CHARS )
        {
            payload = CopilotTruncateUtf8( payload, MAX_TOOL_RESULT_CHARS )
                      + "\n...[truncated; use filters or limits to narrow the query]";
        }

        result.summary = CopilotSanitizeUtf8( result.summary );

        m_history.push_back( COPILOT_MESSAGE::ToolResult( call, payload, result.isError ) );
        m_history.back().attachments = result.attachments;

        if( m_listener )
            m_listener->OnCopilotToolUsed( call, result );
    }

    if( aGeneration != m_generation || !m_busy )
        return;

    m_pendingCalls.clear();
    m_nextCall = 0;

    int maxRounds = m_toolbox ? m_toolbox->MaxToolRounds() : MAX_TOOL_ROUNDS;

    if( ++m_rounds >= maxRounds )
    {
        if( m_listener )
        {
            m_listener->OnCopilotError( "Paused after " + std::to_string( maxRounds )
                                        + " tool rounds. Say \"continue\" to keep going." );
        }

        finishTurn();
        return;
    }

    startRequest();
}
