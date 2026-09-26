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

#include <boost/test/unit_test.hpp>

#include <copilot/copilot_markdown.h>
#include <copilot/copilot_provider.h>
#include <copilot/copilot_session.h>
#include <copilot/copilot_shared_state.h>
#include <copilot/copilot_tool_registry.h>

#include <deque>
#include <mutex>
#include <thread>

using json = nlohmann::json;


namespace
{

COPILOT_REQUEST sampleRequest()
{
    COPILOT_REQUEST req;
    req.system = "sys";
    req.messages.push_back( COPILOT_MESSAGE::User( "hi" ) );

    COPILOT_MESSAGE a;
    a.role = COPILOT_MESSAGE::ROLE::ASSISTANT;
    a.text = "checking";
    a.toolCalls.push_back( { "t1", "get_board_summary", json::object() } );
    a.toolCalls.push_back( { "t2", "list_nets", { { "filter", "GND" } } } );
    req.messages.push_back( a );

    req.messages.push_back( COPILOT_MESSAGE::ToolResult( a.toolCalls[0], "{}", false ) );
    req.messages.push_back( COPILOT_MESSAGE::ToolResult( a.toolCalls[1], "bad", true ) );
    req.tools.push_back( { "get_board_summary", "d", { { "type", "object" } }, false } );
    return req;
}


struct TEST_TOOLBOX : public COPILOT_TOOLBOX
{
    int calls = 0;

    std::string GetSystemPrompt() override { return "test"; }

    std::vector<COPILOT_TOOL_DEF> GetTools( bool aAllowEdits ) override
    {
        std::vector<COPILOT_TOOL_DEF> v = { { "read", "d", { { "type", "object" } }, false } };

        if( aAllowEdits )
            v.push_back( { "edit", "d", { { "type", "object" } }, true } );

        return v;
    }

    std::string GetContextSnapshot() override { return "ctx"; }

    COPILOT_TOOL_RESULT ExecuteTool( const std::string& aName, const json& ) override
    {
        ++calls;
        return COPILOT_TOOL_RESULT::Ok( { { "tool", aName } } );
    }
};


struct TEST_LISTENER : public COPILOT_SESSION_LISTENER
{
    std::vector<std::string> texts, errors, tools, waits;
    std::vector<bool>        approvalOfferedAlways;
    bool                     approve = true;
    bool                     approveAlways = false;

    void OnCopilotBusy( bool ) override {}
    void OnCopilotWaiting( const std::string& aWhy ) override { waits.push_back( aWhy ); }

    bool OnCopilotApproval( const COPILOT_TOOL_CALL&, const std::string&, bool* aAlways ) override
    {
        approvalOfferedAlways.push_back( aAlways != nullptr );

        if( aAlways && approveAlways )
            *aAlways = true;

        return approve;
    }

    void OnCopilotAssistantText( const std::string& aText ) override { texts.push_back( aText ); }
    void OnCopilotError( const std::string& aErr ) override { errors.push_back( aErr ); }

    void OnCopilotToolUsed( const COPILOT_TOOL_CALL& aCall, const COPILOT_TOOL_RESULT& aRes ) override
    {
        tools.push_back( aCall.name + ( aRes.isError ? ":error" : "" ) );
    }
};


/// A registry toolbox with a few tools exercising flags and arguments.
struct REG_TOOLBOX : public COPILOT_REGISTRY_TOOLBOX
{
    int  readyAfter = 0;         ///< IsReadyForTools() fails this many times first.
    int  readyChecks = 0;
    bool sawEditsAllowed = false;

    REG_TOOLBOX()
    {
        AddTool( "fetch_url", "d", { { "url", COPILOT_ARGS::String( "u" ) } }, { "url" }, APPROVAL,
                 []( const json& aIn )
                 {
                     return COPILOT_TOOL_RESULT::Ok( { { "url", COPILOT_ARGS::Str( aIn, "url" ) } } );
                 } );
        AddTool( "run_command", "d", json::object(), {}, ASK_EVERY_TIME,
                 []( const json& ) { return COPILOT_TOOL_RESULT::Ok( { { "ran", true } } ); } );
        AddTool( "big", "d", json::object(), {}, READ_ONLY,
                 []( const json& )
                 {
                     COPILOT_TOOL_RESULT r;
                     r.content = std::string( 3000, 'x' );
                     return r;
                 } );
        AddTool( "count", "d", json::object(), {}, READ_ONLY,
                 [this]( const json& aIn )
                 {
                     sawEditsAllowed = EditsAllowed();
                     return COPILOT_TOOL_RESULT::Ok(
                             { { "n", COPILOT_ARGS::Int( aIn, "n", 0, -5, 5 ) } } );
                 } );
    }

    std::string GetSystemPrompt() override { return "reg"; }
    std::string GetContextSnapshot() override { return ""; }
    int         MaxToolRounds() override { return 100; }

    bool IsReadyForTools( std::string& aWhy ) override
    {
        if( readyChecks++ < readyAfter )
        {
            aWhy = "busy";
            return false;
        }

        return true;
    }
};


/// Drive a session with a scripted transport; runs dispatched and scheduled callbacks inline.
struct SCRIPTED_SESSION
{
    std::shared_ptr<COPILOT_SESSION>                  session;
    std::deque<std::function<void()>>                 queue;
    std::mutex                                        mutex;
    std::function<COPILOT_RESPONSE( const COPILOT_REQUEST& )> script;
    std::vector<COPILOT_REQUEST>                      requests;
    int                                               scheduled = 0;

    SCRIPTED_SESSION( COPILOT_TOOLBOX* aToolbox, COPILOT_SESSION_LISTENER* aListener )
    {
        session = COPILOT_SESSION::Create( aToolbox,
                                           []()
                                           {
                                               COPILOT_CONFIG c;
                                               c.apiKey = "k";
                                               return c;
                                           } );
        session->SetListener( aListener );
        session->SetDispatcher(
                [this]( std::function<void()> aFn )
                {
                    std::lock_guard<std::mutex> lock( mutex );
                    queue.push_back( std::move( aFn ) );
                } );
        session->SetScheduler(
                [this]( int, std::function<void()> aFn )
                {
                    std::lock_guard<std::mutex> lock( mutex );
                    ++scheduled;
                    queue.push_back( std::move( aFn ) );
                } );
        session->SetTransport(
                [this]( const COPILOT_PROVIDER&, const COPILOT_REQUEST& aReq,
                        const std::function<bool()>& )
                {
                    {
                        std::lock_guard<std::mutex> lock( mutex );
                        requests.push_back( aReq );
                    }

                    return script( aReq );
                } );
    }

    void Run()
    {
        for( int spin = 0; session->IsBusy() && spin < 4000; ++spin )
        {
            std::function<void()> fn;
            {
                std::lock_guard<std::mutex> lock( mutex );

                if( !queue.empty() )
                {
                    fn = queue.front();
                    queue.pop_front();
                }
            }

            if( fn )
                fn();
            else
                std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
        }
    }
};


COPILOT_RESPONSE toolUse( std::vector<COPILOT_TOOL_CALL> aCalls )
{
    COPILOT_RESPONSE r;
    r.ok = true;
    r.message.role = COPILOT_MESSAGE::ROLE::ASSISTANT;
    r.message.toolCalls = std::move( aCalls );
    r.stopReason = "tool_use";
    return r;
}


COPILOT_RESPONSE finalText( const std::string& aText )
{
    COPILOT_RESPONSE r;
    r.ok = true;
    r.message.role = COPILOT_MESSAGE::ROLE::ASSISTANT;
    r.message.text = aText;
    r.stopReason = "end_turn";
    return r;
}

} // namespace


BOOST_AUTO_TEST_SUITE( Copilot )


BOOST_AUTO_TEST_CASE( AnthropicRequestGroupsToolResults )
{
    COPILOT_CONFIG cfg;
    cfg.apiKey = "k";
    COPILOT_ANTHROPIC_PROVIDER provider( cfg );

    COPILOT_REQUEST req = sampleRequest();
    req.messages.push_back( COPILOT_MESSAGE::User( "follow up" ) );

    COPILOT_HTTP_REQUEST http = provider.BuildHttpRequest( req );
    json                 body = json::parse( http.body );

    BOOST_CHECK_EQUAL( http.url, "https://api.anthropic.com/v1/messages" );
    BOOST_REQUIRE_EQUAL( body["messages"].size(), 3 );

    // Both tool results plus the follow-up text land in one user message.
    const json& results = body["messages"][2]["content"];
    BOOST_REQUIRE_EQUAL( results.size(), 3 );
    BOOST_CHECK_EQUAL( results[0]["type"], "tool_result" );
    BOOST_CHECK_EQUAL( results[1]["is_error"], true );
    BOOST_CHECK_EQUAL( results[2]["type"], "text" );
    BOOST_CHECK_EQUAL( body["messages"][1]["content"][2]["input"]["filter"], "GND" );
}


BOOST_AUTO_TEST_CASE( AnthropicResponseParsing )
{
    COPILOT_ANTHROPIC_PROVIDER provider( COPILOT_CONFIG{} );

    COPILOT_RESPONSE r = provider.ParseHttpResponse(
            200, R"({"content":[{"type":"text","text":"a"},{"type":"tool_use","id":"x","name":"n",
                     "input":{"q":1}}],"stop_reason":"tool_use","usage":{"input_tokens":5,
                     "output_tokens":7}})" );

    BOOST_CHECK( r.ok );
    BOOST_CHECK_EQUAL( r.stopReason, "tool_use" );
    BOOST_REQUIRE_EQUAL( r.message.toolCalls.size(), 1 );
    BOOST_CHECK_EQUAL( r.message.toolCalls[0].input["q"], 1 );
    BOOST_CHECK_EQUAL( r.outputTokens, 7 );

    COPILOT_RESPONSE err = provider.ParseHttpResponse(
            401, R"({"type":"error","error":{"message":"invalid x-api-key"}})" );
    BOOST_CHECK( !err.ok );
    BOOST_CHECK( err.error.find( "invalid x-api-key" ) != std::string::npos );
}


BOOST_AUTO_TEST_CASE( OpenAiRoundTrip )
{
    COPILOT_CONFIG cfg;
    cfg.provider = COPILOT_PROVIDER_TYPE::OLLAMA;
    COPILOT_OPENAI_PROVIDER provider( cfg );

    COPILOT_HTTP_REQUEST http = provider.BuildHttpRequest( sampleRequest() );
    json                 body = json::parse( http.body );

    BOOST_CHECK_EQUAL( http.url, "http://localhost:11434/v1/chat/completions" );
    BOOST_CHECK_EQUAL( body["messages"][0]["role"], "system" );
    BOOST_CHECK_EQUAL( body["messages"][2]["tool_calls"][1]["function"]["arguments"],
                       "{\"filter\":\"GND\"}" );
    BOOST_CHECK_EQUAL( body["messages"][4]["role"], "tool" );

    COPILOT_RESPONSE r = provider.ParseHttpResponse(
            200, R"({"choices":[{"message":{"content":"<think>x</think>Hello","tool_calls":[
                     {"function":{"name":"f","arguments":{"a":2}}}]},"finish_reason":"tool_calls"}]})" );

    BOOST_CHECK( r.ok );
    BOOST_CHECK_EQUAL( r.message.text, "Hello" );
    BOOST_REQUIRE_EQUAL( r.message.toolCalls.size(), 1 );
    BOOST_CHECK_EQUAL( r.message.toolCalls[0].id, "call_0" );
    BOOST_CHECK_EQUAL( r.message.toolCalls[0].input["a"], 2 );
}


BOOST_AUTO_TEST_CASE( MarkdownConversion )
{
    std::string html = CopilotMarkdownToHtml( "# T\n**b** `U1<x>`\n\n- a\n- b\n\n```\n<c>\n```" );

    BOOST_CHECK( html.find( "<h3>T</h3>" ) != std::string::npos );
    BOOST_CHECK( html.find( "<b>b</b>" ) != std::string::npos );
    BOOST_CHECK( html.find( "<code>U1&lt;x&gt;</code>" ) != std::string::npos );
    BOOST_CHECK( html.find( "<ul><li>a</li><li>b</li></ul>" ) != std::string::npos );
    BOOST_CHECK( html.find( "<pre>&lt;c&gt;</pre>" ) != std::string::npos );
    BOOST_CHECK( CopilotMarkdownToHtml( "<script>" ).find( "<script>" ) == std::string::npos );
}


BOOST_AUTO_TEST_CASE( SessionToolLoop )
{
    TEST_TOOLBOX  toolbox;
    TEST_LISTENER listener;
    int           round = 0;

    std::deque<std::function<void()>> queue;
    std::mutex                        mutex;

    auto session = COPILOT_SESSION::Create( &toolbox,
                                            []()
                                            {
                                                COPILOT_CONFIG c;
                                                c.apiKey = "k";
                                                return c;
                                            } );

    session->SetListener( &listener );
    session->SetDispatcher(
            [&]( std::function<void()> aFn )
            {
                std::lock_guard<std::mutex> lock( mutex );
                queue.push_back( std::move( aFn ) );
            } );

    session->SetTransport(
            [&]( const COPILOT_PROVIDER&, const COPILOT_REQUEST&, const std::function<bool()>& )
            {
                COPILOT_RESPONSE r;
                r.ok = true;
                r.message.role = COPILOT_MESSAGE::ROLE::ASSISTANT;

                if( round++ == 0 )
                {
                    r.message.toolCalls.push_back( { "c1", "read", json::object() } );
                    r.message.toolCalls.push_back( { "c2", "edit", json::object() } );
                    r.stopReason = "tool_use";
                }
                else
                {
                    r.message.text = "done";
                    r.stopReason = "end_turn";
                }

                return r;
            } );

    BOOST_REQUIRE( session->Send( "go" ) );

    for( int spin = 0; session->IsBusy() && spin < 2000; ++spin )
    {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lock( mutex );

            if( !queue.empty() )
            {
                fn = queue.front();
                queue.pop_front();
            }
        }

        if( fn )
            fn();
        else
            std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    }

    BOOST_CHECK( !session->IsBusy() );
    BOOST_REQUIRE_EQUAL( listener.tools.size(), 2 );
    BOOST_CHECK_EQUAL( listener.tools[0], "read" );
    BOOST_CHECK_EQUAL( listener.tools[1], "edit:error" );    // edits are off by default
    BOOST_CHECK_EQUAL( toolbox.calls, 1 );
    BOOST_REQUIRE_EQUAL( listener.texts.size(), 1 );
    BOOST_CHECK_EQUAL( listener.texts[0], "done" );
    BOOST_CHECK_EQUAL( session->History().size(), 5 );
}


BOOST_AUTO_TEST_CASE( SessionMissingKey )
{
    TEST_TOOLBOX  toolbox;
    TEST_LISTENER listener;

    auto session = COPILOT_SESSION::Create( &toolbox, []() { return COPILOT_CONFIG(); } );
    session->SetListener( &listener );
    session->Send( "hi" );

    BOOST_CHECK( !session->IsBusy() );
    BOOST_CHECK_EQUAL( listener.errors.size(), 1 );
    BOOST_CHECK( session->History().empty() );
}


BOOST_AUTO_TEST_CASE( Utf8Helpers )
{
    std::string mu = "ab\xC2\xB5" "cd";        // "abµcd"

    BOOST_CHECK_EQUAL( CopilotTruncateUtf8( mu, 3 ), "ab" );          // never splits µ
    BOOST_CHECK_EQUAL( CopilotTruncateUtf8( mu, 4 ), "ab\xC2\xB5" );
    BOOST_CHECK_EQUAL( CopilotTruncateUtf8( mu, 99 ), mu );

    BOOST_CHECK_EQUAL( CopilotSanitizeUtf8( mu ), mu );
    BOOST_CHECK_EQUAL( CopilotSanitizeUtf8( "a\xFF" "b" ), "a\xEF\xBF\xBD" "b" );
    BOOST_CHECK_EQUAL( CopilotSanitizeUtf8( "x\xC2" ), "x\xEF\xBF\xBD" );         // truncated
    BOOST_CHECK_EQUAL( CopilotSanitizeUtf8( "\xC0\xAF" ),                         // overlong
                       "\xEF\xBF\xBD\xEF\xBF\xBD" );
    BOOST_CHECK_EQUAL( CopilotSanitizeUtf8( "\xED\xA0\x80" ).substr( 0, 3 ),      // surrogate
                       "\xEF\xBF\xBD" );
}


BOOST_AUTO_TEST_CASE( RegistryArgumentsAndFlags )
{
    json in = { { "big", 1e300 }, { "neg", -1e300 }, { "s", "12.6" }, { "bad", "nan" },
                { "inf", "inf" } };

    BOOST_CHECK_EQUAL( COPILOT_ARGS::Int( in, "big", 0, 0, 100 ), 100 );
    BOOST_CHECK_EQUAL( COPILOT_ARGS::Int( in, "neg", 0, -3, 100 ), -3 );
    BOOST_CHECK_EQUAL( COPILOT_ARGS::Int( in, "s", 0, 0, 100 ), 13 );
    BOOST_CHECK( !COPILOT_ARGS::Num( in, "bad" ) );
    BOOST_CHECK( !COPILOT_ARGS::Num( in, "inf" ) );

    REG_TOOLBOX tb;
    COPILOT_TOOL_DEF def;

    BOOST_REQUIRE( tb.FindTool( "run_command", def ) );
    BOOST_CHECK( def.requiresApproval );
    BOOST_CHECK( !def.allowAlways );

    BOOST_REQUIRE( tb.FindTool( "fetch_url", def ) );
    BOOST_CHECK( def.requiresApproval && def.allowAlways );

    BOOST_CHECK_EQUAL( tb.ApprovalKey( "fetch_url", { { "url", "https://User@WWW.TI.com:443/x?y" } } ),
                       "fetch_url@www.ti.com" );
    BOOST_CHECK_EQUAL( tb.ApprovalKey( "count", json::object() ), "count" );
    BOOST_CHECK_EQUAL( CopilotUrlHost( "http://[::1]:8080/x" ), "[::1]" );
    BOOST_CHECK_EQUAL( CopilotUrlHost( "not a url" ), "" );

    // Approval prompts show hidden characters instead of letting them disguise a command.
    std::string shown = tb.DescribeToolCall(
            "run_command", { { "command", "echo hi\n\xE2\x80\xAE" "rm -rf ~\x07" } } );
    BOOST_CHECK( shown.find( "[direction mark]" ) != std::string::npos );
    BOOST_CHECK( shown.find( "\xE2\x8F\x8E" ) != std::string::npos );       // visible line break
    BOOST_CHECK( shown.find( '\x07' ) == std::string::npos );
    BOOST_CHECK( shown.find( '\n' + std::string( "rm" ) ) == std::string::npos );
    BOOST_CHECK_LT( CopilotVisibleText( std::string( 5000, 'x' ), 100 ).size(), 140u );

    // The flags survive the KIWAY mail round trip used by the project manager.
    std::vector<COPILOT_TOOL_DEF> defs =
            COPILOT_REGISTRY_TOOLBOX::ParseToolList( tb.HandleMail( R"({"op":"list"})" ) );
    BOOST_REQUIRE_EQUAL( defs.size(), 4 );
    BOOST_CHECK( !defs[1].allowAlways );

    // Forwarded calls carry the caller's edit permission.
    tb.HandleMail( COPILOT_REGISTRY_TOOLBOX::MakeCallRequest( "count", json::object(), true ) );
    BOOST_CHECK( tb.sawEditsAllowed );
    BOOST_CHECK( !tb.EditsAllowed() );      // restored afterwards

    std::string why;
    tb.readyAfter = 1;
    BOOST_CHECK( !COPILOT_REGISTRY_TOOLBOX::ParseReadyReply( tb.HandleMail( R"({"op":"ready"})" ),
                                                             why ) );
    BOOST_CHECK_EQUAL( why, "busy" );
    BOOST_CHECK( COPILOT_REGISTRY_TOOLBOX::ParseReadyReply( tb.HandleMail( R"({"op":"ready"})" ),
                                                            why ) );
}


BOOST_AUTO_TEST_CASE( SessionApprovals )
{
    REG_TOOLBOX   tb;
    TEST_LISTENER listener;
    listener.approveAlways = true;

    SCRIPTED_SESSION s( &tb, &listener );
    int              round = 0;

    s.script = [&]( const COPILOT_REQUEST& ) -> COPILOT_RESPONSE
    {
        switch( round++ )
        {
        case 0:
            return toolUse( { { "a", "fetch_url", { { "url", "https://a.com/1" } } },
                              { "b", "fetch_url", { { "url", "https://a.com/2" } } },
                              { "c", "fetch_url", { { "url", "https://b.com/1" } } },
                              { "d", "run_command", json::object() },
                              { "e", "run_command", json::object() } } );
        default:
            return finalText( "ok" );
        }
    };

    BOOST_REQUIRE( s.session->Send( "go" ) );
    s.Run();

    // a.com once (then remembered per host), b.com once, run_command every time and never
    // with an "always" option.
    BOOST_REQUIRE_EQUAL( listener.approvalOfferedAlways.size(), 4 );
    BOOST_CHECK( listener.approvalOfferedAlways[0] );
    BOOST_CHECK( listener.approvalOfferedAlways[1] );
    BOOST_CHECK( !listener.approvalOfferedAlways[2] );
    BOOST_CHECK( !listener.approvalOfferedAlways[3] );
    BOOST_CHECK_EQUAL( listener.tools.size(), 5 );

    // A new conversation forgets "always allow".
    s.session->Reset();
    listener.approvalOfferedAlways.clear();
    round = 0;
    BOOST_REQUIRE( s.session->Send( "again" ) );
    s.Run();
    BOOST_CHECK_EQUAL( listener.approvalOfferedAlways.size(), 4 );

    // Declined approvals become error results; the tool does not run.
    s.session->Reset();
    listener.approve = false;
    listener.tools.clear();
    round = 0;
    BOOST_REQUIRE( s.session->Send( "no" ) );
    s.Run();
    BOOST_REQUIRE_EQUAL( listener.tools.size(), 5 );
    BOOST_CHECK_EQUAL( listener.tools[0], "fetch_url:error" );
}


BOOST_AUTO_TEST_CASE( SessionWaitsUntilReady )
{
    REG_TOOLBOX   tb;
    TEST_LISTENER listener;
    tb.readyAfter = 3;

    SCRIPTED_SESSION s( &tb, &listener );
    int              round = 0;

    s.script = [&]( const COPILOT_REQUEST& ) -> COPILOT_RESPONSE
    {
        return round++ == 0 ? toolUse( { { "a", "count", json::object() } } ) : finalText( "ok" );
    };

    BOOST_REQUIRE( s.session->Send( "go" ) );
    s.Run();

    BOOST_CHECK_EQUAL( s.scheduled, 3 );
    BOOST_REQUIRE_EQUAL( listener.waits.size(), 1 );    // reported once, not on every retry
    BOOST_CHECK_EQUAL( listener.waits[0], "busy" );
    BOOST_REQUIRE_EQUAL( listener.tools.size(), 1 );
    BOOST_CHECK_EQUAL( s.requests.size(), 2 );

    // Another editor's tool running holds everything back too.
    tb.readyAfter = 0;
    round = 0;
    s.scheduled = 0;
    COPILOT_SHARED::BeginTool();

    BOOST_REQUIRE( s.session->Send( "go" ) );

    for( int i = 0; i < 200 && s.scheduled < 2; ++i )
    {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lock( s.mutex );

            if( !s.queue.empty() )
            {
                fn = s.queue.front();
                s.queue.pop_front();
            }
        }

        if( fn )
            fn();
        else
            std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
    }

    BOOST_CHECK_EQUAL( listener.tools.size(), 1 );      // nothing ran yet
    COPILOT_SHARED::EndTool();
    s.Run();
    BOOST_CHECK_EQUAL( listener.tools.size(), 2 );
    BOOST_CHECK( !COPILOT_SHARED::ToolRunning() );
}


BOOST_AUTO_TEST_CASE( SessionStopDuringWaitDropsResults )
{
    REG_TOOLBOX   tb;
    TEST_LISTENER listener;
    tb.readyAfter = 1000;       // never ready

    SCRIPTED_SESSION s( &tb, &listener );
    s.script = []( const COPILOT_REQUEST& ) { return toolUse( { { "a", "count", json::object() } } ); };

    BOOST_REQUIRE( s.session->Send( "go" ) );

    for( int i = 0; i < 400 && s.scheduled < 2; ++i )
    {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lock( s.mutex );

            if( !s.queue.empty() )
            {
                fn = s.queue.front();
                s.queue.pop_front();
            }
        }

        if( fn )
            fn();
        else
            std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
    }

    s.session->Cancel();
    BOOST_CHECK( !s.session->IsBusy() );

    // Pending retries fire after the cancel and must do nothing.
    tb.readyAfter = 0;

    while( !s.queue.empty() )
    {
        auto fn = s.queue.front();
        s.queue.pop_front();
        fn();
    }

    BOOST_CHECK( listener.tools.empty() );

    // History stays well formed: every tool call has a result.
    const std::vector<COPILOT_MESSAGE>& h = s.session->History();
    BOOST_REQUIRE_EQUAL( h.size(), 3 );
    BOOST_CHECK( h[2].role == COPILOT_MESSAGE::ROLE::TOOL_RESULT );
    BOOST_CHECK( h[2].isError );
}


BOOST_AUTO_TEST_CASE( SessionCompactionKeepsPrefixStable )
{
    REG_TOOLBOX   tb;
    TEST_LISTENER listener;

    SCRIPTED_SESSION s( &tb, &listener );
    int              round = 0;

    s.script = [&]( const COPILOT_REQUEST& ) -> COPILOT_RESPONSE
    {
        if( round++ < 40 )
            return toolUse( { { "c" + std::to_string( round ), "big", json::object() } } );

        return finalText( "ok" );
    };

    BOOST_REQUIRE( s.session->Send( "go" ) );
    s.Run();

    BOOST_REQUIRE_EQUAL( s.requests.size(), 41 );

    // Between two consecutive requests, earlier messages either stay identical or the whole
    // prefix changes at a batch boundary; count how often the prefix changed.
    int changes = 0;

    for( size_t r = 1; r < s.requests.size(); ++r )
    {
        const auto& prev = s.requests[r - 1].messages;
        const auto& cur = s.requests[r].messages;

        for( size_t i = 0; i < prev.size(); ++i )
        {
            if( prev[i].toolResult != cur[i].toolResult )
            {
                ++changes;
                break;
            }
        }
    }

    BOOST_CHECK_GE( changes, 1 );      // compaction did happen ...
    BOOST_CHECK_LE( changes, 3 );      // ... but only in a few batches

    const COPILOT_REQUEST& last = s.requests.back();
    BOOST_CHECK_LT( last.messages[2].toolResult.size(), 1400u );                 // oldest: short
    BOOST_CHECK_EQUAL( last.messages[last.messages.size() - 1].toolResult.size(),
                       s.session->History()[last.messages.size() - 1].toolResult.size() );
}


BOOST_AUTO_TEST_CASE( SessionRetriesWithoutRejectedAttachment )
{
    struct PDF_TOOLBOX : public REG_TOOLBOX
    {
        PDF_TOOLBOX()
        {
            AddTool( "read_pdf", "d", json::object(), {}, READ_ONLY,
                     []( const json& )
                     {
                         COPILOT_TOOL_RESULT r = COPILOT_TOOL_RESULT::Ok( { { "pdf", true } } );
                         r.attachments.push_back( { "application/pdf", "JVBERi0=", "x.pdf" } );
                         return r;
                     } );
        }
    };

    PDF_TOOLBOX   tb;
    TEST_LISTENER listener;
    SCRIPTED_SESSION s( &tb, &listener );
    int              round = 0;

    s.script = [&]( const COPILOT_REQUEST& aReq ) -> COPILOT_RESPONSE
    {
        bool hasAttachment = false;

        for( const COPILOT_MESSAGE& m : aReq.messages )
            hasAttachment |= !m.attachments.empty();

        if( round++ == 0 )
            return toolUse( { { "a", "read_pdf", json::object() } } );

        if( hasAttachment )
        {
            COPILOT_RESPONSE bad;
            bad.error = "HTTP 400: document too large";
            return bad;
        }

        return finalText( "read it" );
    };

    BOOST_REQUIRE( s.session->Send( "go" ) );
    s.Run();

    BOOST_CHECK( listener.errors.empty() );
    BOOST_REQUIRE_EQUAL( listener.texts.size(), 1 );
    BOOST_CHECK_EQUAL( listener.texts[0], "read it" );
    BOOST_CHECK( s.session->History()[2].toolResult.find( "rejected" ) != std::string::npos );
}


BOOST_AUTO_TEST_CASE( OpenAiContentNullOnlyWithToolCalls )
{
    COPILOT_CONFIG cfg;
    cfg.provider = COPILOT_PROVIDER_TYPE::OPENAI;
    COPILOT_OPENAI_PROVIDER provider( cfg );

    COPILOT_REQUEST req;
    COPILOT_MESSAGE empty;
    empty.role = COPILOT_MESSAGE::ROLE::ASSISTANT;
    req.messages.push_back( COPILOT_MESSAGE::User( "hi" ) );
    req.messages.push_back( empty );

    json body = json::parse( provider.BuildHttpRequest( req ).body );
    BOOST_CHECK( body["messages"][1]["content"].is_string() );

    req.messages.back().toolCalls.push_back( { "t", "n", json::object() } );
    body = json::parse( provider.BuildHttpRequest( req ).body );
    BOOST_CHECK( body["messages"][1]["content"].is_null() );
}


BOOST_AUTO_TEST_SUITE_END()
