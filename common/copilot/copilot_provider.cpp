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

#include <copilot/copilot_provider.h>

#include <algorithm>
#include <cctype>


using json = nlohmann::json;


std::string CopilotProviderId( COPILOT_PROVIDER_TYPE aType )
{
    switch( aType )
    {
    case COPILOT_PROVIDER_TYPE::ANTHROPIC:         return "anthropic";
    case COPILOT_PROVIDER_TYPE::OPENAI:            return "openai";
    case COPILOT_PROVIDER_TYPE::OPENAI_COMPATIBLE: return "openai_compatible";
    case COPILOT_PROVIDER_TYPE::OLLAMA:            return "ollama";
    }

    return "anthropic";
}


COPILOT_PROVIDER_TYPE CopilotProviderFromId( const std::string& aId )
{
    if( aId == "openai" )
        return COPILOT_PROVIDER_TYPE::OPENAI;

    if( aId == "openai_compatible" )
        return COPILOT_PROVIDER_TYPE::OPENAI_COMPATIBLE;

    if( aId == "ollama" )
        return COPILOT_PROVIDER_TYPE::OLLAMA;

    return COPILOT_PROVIDER_TYPE::ANTHROPIC;
}


std::string CopilotDefaultBaseUrl( COPILOT_PROVIDER_TYPE aType )
{
    switch( aType )
    {
    case COPILOT_PROVIDER_TYPE::ANTHROPIC:         return "https://api.anthropic.com";
    case COPILOT_PROVIDER_TYPE::OPENAI:            return "https://api.openai.com/v1";
    case COPILOT_PROVIDER_TYPE::OPENAI_COMPATIBLE: return "";
    case COPILOT_PROVIDER_TYPE::OLLAMA:            return "http://localhost:11434/v1";
    }

    return "";
}


std::string CopilotDefaultModel( COPILOT_PROVIDER_TYPE aType )
{
    switch( aType )
    {
    case COPILOT_PROVIDER_TYPE::ANTHROPIC:         return "claude-sonnet-5";
    case COPILOT_PROVIDER_TYPE::OPENAI:            return "gpt-5";
    case COPILOT_PROVIDER_TYPE::OPENAI_COMPATIBLE: return "";
    case COPILOT_PROVIDER_TYPE::OLLAMA:            return "qwen3";
    }

    return "";
}


bool CopilotProviderNeedsKey( COPILOT_PROVIDER_TYPE aType )
{
    return aType == COPILOT_PROVIDER_TYPE::ANTHROPIC || aType == COPILOT_PROVIDER_TYPE::OPENAI;
}


std::string CopilotTruncateUtf8( const std::string& aText, size_t aMaxBytes )
{
    if( aText.size() <= aMaxBytes )
        return aText;

    size_t cut = aMaxBytes;

    // Back up over continuation bytes (10xxxxxx) so the cut lands on a character boundary.
    while( cut > 0 && ( static_cast<unsigned char>( aText[cut] ) & 0xC0 ) == 0x80 )
        --cut;

    return aText.substr( 0, cut );
}


std::string CopilotSanitizeUtf8( const std::string& aText )
{
    std::string out;
    out.reserve( aText.size() );

    const unsigned char* s = reinterpret_cast<const unsigned char*>( aText.data() );
    size_t               n = aText.size();
    size_t               i = 0;

    while( i < n )
    {
        unsigned char c = s[i];
        size_t        len = 0;
        uint32_t      cp = 0;

        if( c < 0x80 )
        {
            out.push_back( static_cast<char>( c ) );
            ++i;
            continue;
        }
        else if( ( c & 0xE0 ) == 0xC0 )
        {
            len = 2;
            cp = c & 0x1F;
        }
        else if( ( c & 0xF0 ) == 0xE0 )
        {
            len = 3;
            cp = c & 0x0F;
        }
        else if( ( c & 0xF8 ) == 0xF0 )
        {
            len = 4;
            cp = c & 0x07;
        }

        bool ok = len > 0 && i + len <= n;

        for( size_t k = 1; ok && k < len; ++k )
        {
            if( ( s[i + k] & 0xC0 ) != 0x80 )
                ok = false;
            else
                cp = ( cp << 6 ) | ( s[i + k] & 0x3F );
        }

        // Reject overlong forms, surrogates and out of range code points.
        if( ok )
        {
            static const uint32_t minCp[5] = { 0, 0, 0x80, 0x800, 0x10000 };

            if( cp < minCp[len] || cp > 0x10FFFF || ( cp >= 0xD800 && cp <= 0xDFFF ) )
                ok = false;
        }

        if( ok )
        {
            out.append( aText, i, len );
            i += len;
        }
        else
        {
            out += "\xEF\xBF\xBD";     // U+FFFD
            ++i;
        }
    }

    return out;
}


std::string COPILOT_PROVIDER::BaseUrl() const
{
    std::string url = m_config.baseUrl.empty() ? CopilotDefaultBaseUrl( m_config.provider )
                                               : m_config.baseUrl;

    while( !url.empty() && url.back() == '/' )
        url.pop_back();

    return url;
}


std::string COPILOT_PROVIDER::Model() const
{
    return m_config.model.empty() ? CopilotDefaultModel( m_config.provider ) : m_config.model;
}


std::unique_ptr<COPILOT_PROVIDER> COPILOT_PROVIDER::Create( const COPILOT_CONFIG& aConfig )
{
    if( aConfig.provider == COPILOT_PROVIDER_TYPE::ANTHROPIC )
        return std::make_unique<COPILOT_ANTHROPIC_PROVIDER>( aConfig );

    return std::make_unique<COPILOT_OPENAI_PROVIDER>( aConfig );
}


std::string COPILOT_PROVIDER::extractError( long aStatus, const std::string& aBody )
{
    std::string detail;

    json payload = json::parse( aBody, nullptr, false );

    if( !payload.is_discarded() && payload.is_object() )
    {
        // Anthropic: {"type":"error","error":{"type":..,"message":..}}
        // OpenAI:    {"error":{"message":..,"type":..}}
        // Ollama:    {"error":"..."}
        if( payload.contains( "error" ) )
        {
            const json& err = payload["error"];

            if( err.is_string() )
                detail = err.get<std::string>();
            else if( err.is_object() && err.contains( "message" ) && err["message"].is_string() )
                detail = err["message"].get<std::string>();
        }
        else if( payload.contains( "message" ) && payload["message"].is_string() )
        {
            detail = payload["message"].get<std::string>();
        }
    }

    if( detail.empty() )
        detail = aBody.substr( 0, 400 );

    std::string prefix;

    switch( aStatus )
    {
    case 401:
    case 403: prefix = "Authentication failed (check your API key). "; break;
    case 404: prefix = "Endpoint or model not found. "; break;
    case 429: prefix = "Rate limited by the provider. "; break;
    case 529:
    case 503: prefix = "The provider is overloaded; try again shortly. "; break;
    default: break;
    }

    return prefix + "HTTP " + std::to_string( aStatus ) + ( detail.empty() ? "" : ": " + detail );
}


// ---------------------------------------------------------------------------------------------
// Anthropic Messages API
// ---------------------------------------------------------------------------------------------

COPILOT_HTTP_REQUEST COPILOT_ANTHROPIC_PROVIDER::BuildHttpRequest( const COPILOT_REQUEST& aReq ) const
{
    json body;
    body["model"] = Model();
    body["max_tokens"] = m_config.maxTokens;

    if( !aReq.system.empty() )
    {
        // Cache the (large, stable) system prompt between turns.
        body["system"] = json::array( { { { "type", "text" },
                                          { "text", aReq.system },
                                          { "cache_control", { { "type", "ephemeral" } } } } } );
    }

    json messages = json::array();

    for( size_t i = 0; i < aReq.messages.size(); ++i )
    {
        const COPILOT_MESSAGE& msg = aReq.messages[i];

        switch( msg.role )
        {
        case COPILOT_MESSAGE::ROLE::USER:
            messages.push_back( { { "role", "user" },
                                  { "content", json::array( { { { "type", "text" },
                                                                { "text", msg.text } } } ) } } );
            break;

        case COPILOT_MESSAGE::ROLE::ASSISTANT:
        {
            if( msg.providerContent.is_array() && !msg.providerContent.empty() )
            {
                messages.push_back( { { "role", "assistant" }, { "content", msg.providerContent } } );
                break;
            }

            json content = json::array();

            if( !msg.text.empty() )
                content.push_back( { { "type", "text" }, { "text", msg.text } } );

            for( const COPILOT_TOOL_CALL& call : msg.toolCalls )
            {
                content.push_back( { { "type", "tool_use" },
                                     { "id", call.id },
                                     { "name", call.name },
                                     { "input", call.input.is_object() ? call.input
                                                                       : json::object() } } );
            }

            if( content.empty() )
                content.push_back( { { "type", "text" }, { "text", "(no response)" } } );

            messages.push_back( { { "role", "assistant" }, { "content", content } } );
            break;
        }

        case COPILOT_MESSAGE::ROLE::TOOL_RESULT:
        {
            // Consecutive tool results must be grouped into a single user message.
            json content = json::array();

            while( i < aReq.messages.size()
                   && aReq.messages[i].role == COPILOT_MESSAGE::ROLE::TOOL_RESULT )
            {
                const COPILOT_MESSAGE& res = aReq.messages[i];
                json block = { { "type", "tool_result" }, { "tool_use_id", res.toolCallId } };

                if( res.attachments.empty() )
                {
                    block["content"] = res.toolResult;
                }
                else
                {
                    json parts = json::array( { { { "type", "text" }, { "text", res.toolResult } } } );

                    for( const COPILOT_ATTACHMENT& att : res.attachments )
                    {
                        json doc = { { "type", "document" },
                                     { "source", { { "type", "base64" },
                                                   { "media_type", att.mediaType },
                                                   { "data", att.base64 } } } };

                        if( !att.title.empty() )
                            doc["title"] = att.title;

                        parts.push_back( doc );
                    }

                    block["content"] = parts;
                }

                if( res.isError )
                    block["is_error"] = true;

                content.push_back( block );
                ++i;
            }

            --i;    // compensate for the for-loop increment

            // A user text message directly following tool results merges into the same turn.
            if( i + 1 < aReq.messages.size()
                && aReq.messages[i + 1].role == COPILOT_MESSAGE::ROLE::USER )
            {
                content.push_back( { { "type", "text" }, { "text", aReq.messages[i + 1].text } } );
                ++i;
            }

            messages.push_back( { { "role", "user" }, { "content", content } } );
            break;
        }
        }
    }

    // Incremental prompt caching: mark the end of the conversation so the next round trip of a
    // long tool loop re-reads everything before it from the cache.
    if( !messages.empty() )
    {
        json& lastContent = messages.back()["content"];

        if( lastContent.is_array() && !lastContent.empty() && lastContent.back().is_object() )
        {
            std::string type = lastContent.back().value( "type", "" );

            if( type == "text" || type == "tool_result" || type == "document" )
                lastContent.back()["cache_control"] = { { "type", "ephemeral" } };
        }
    }

    body["messages"] = messages;

    if( !aReq.tools.empty() || m_config.webSearch )
    {
        json tools = json::array();

        for( const COPILOT_TOOL_DEF& tool : aReq.tools )
        {
            tools.push_back( { { "name", tool.name },
                               { "description", tool.description },
                               { "input_schema", tool.inputSchema } } );
        }

        if( m_config.webSearch )
        {
            tools.push_back( { { "type", "web_search_20250305" },
                               { "name", "web_search" },
                               { "max_uses", 8 } } );
        }

        // Cache breakpoint on the last tool caches the whole tool list.
        tools.back()["cache_control"] = { { "type", "ephemeral" } };
        body["tools"] = tools;
    }

    COPILOT_HTTP_REQUEST http;
    http.url = BaseUrl() + "/v1/messages";
    http.headers = { { "Content-Type", "application/json" },
                     { "anthropic-version", "2023-06-01" } };

    if( !m_config.apiKey.empty() )
        http.headers.emplace_back( "x-api-key", m_config.apiKey );

    http.body = body.dump( -1, ' ', false, json::error_handler_t::replace );
    return http;
}


COPILOT_RESPONSE COPILOT_ANTHROPIC_PROVIDER::ParseHttpResponse( long aStatus,
                                                                const std::string& aBody ) const
{
    COPILOT_RESPONSE resp;

    if( aStatus < 200 || aStatus >= 300 )
    {
        resp.error = extractError( aStatus, aBody );
        return resp;
    }

    json payload = json::parse( aBody, nullptr, false );

    if( payload.is_discarded() || !payload.is_object() || !payload.contains( "content" ) )
    {
        resp.error = "Unexpected response from provider: " + aBody.substr( 0, 400 );
        return resp;
    }

    resp.message.role = COPILOT_MESSAGE::ROLE::ASSISTANT;

    // Server tools (web search) produce blocks that must be echoed back verbatim.
    bool hasServerBlocks = false;

    for( const json& block : payload["content"] )
    {
        std::string type = block.value( "type", "" );

        if( type != "text" && type != "tool_use" )
            hasServerBlocks = true;
    }

    if( hasServerBlocks )
        resp.message.providerContent = payload["content"];

    for( const json& block : payload["content"] )
    {
        std::string type = block.value( "type", "" );

        if( type == "text" )
        {
            if( !resp.message.text.empty() )
                resp.message.text += "\n\n";

            resp.message.text += block.value( "text", "" );
        }
        else if( type == "tool_use" )
        {
            COPILOT_TOOL_CALL call;
            call.id = block.value( "id", "" );
            call.name = block.value( "name", "" );

            if( block.contains( "input" ) && block["input"].is_object() )
                call.input = block["input"];

            resp.message.toolCalls.push_back( call );
        }
    }

    std::string stop = payload.value( "stop_reason", "" );

    if( stop == "tool_use" || !resp.message.toolCalls.empty() )
        resp.stopReason = "tool_use";
    else if( stop == "pause_turn" )
        resp.stopReason = "pause_turn";
    else if( stop == "max_tokens" )
        resp.stopReason = "max_tokens";
    else
        resp.stopReason = "end_turn";

    if( payload.contains( "usage" ) && payload["usage"].is_object() )
    {
        const json& usage = payload["usage"];
        resp.inputTokens = usage.value( "input_tokens", 0 )
                           + usage.value( "cache_read_input_tokens", 0 )
                           + usage.value( "cache_creation_input_tokens", 0 );
        resp.outputTokens = usage.value( "output_tokens", 0 );
    }

    resp.ok = true;
    return resp;
}


// ---------------------------------------------------------------------------------------------
// OpenAI Chat Completions (also OpenAI-compatible servers and Ollama)
// ---------------------------------------------------------------------------------------------

COPILOT_HTTP_REQUEST COPILOT_OPENAI_PROVIDER::BuildHttpRequest( const COPILOT_REQUEST& aReq ) const
{
    json body;
    body["model"] = Model();

    // OpenAI's current models reject max_tokens in favour of max_completion_tokens; most
    // compatible servers only understand the older name.
    if( m_config.provider == COPILOT_PROVIDER_TYPE::OPENAI )
        body["max_completion_tokens"] = m_config.maxTokens;
    else
        body["max_tokens"] = m_config.maxTokens;

    json messages = json::array();

    if( !aReq.system.empty() )
        messages.push_back( { { "role", "system" }, { "content", aReq.system } } );

    for( const COPILOT_MESSAGE& msg : aReq.messages )
    {
        switch( msg.role )
        {
        case COPILOT_MESSAGE::ROLE::USER:
            messages.push_back( { { "role", "user" }, { "content", msg.text } } );
            break;

        case COPILOT_MESSAGE::ROLE::ASSISTANT:
        {
            json m = { { "role", "assistant" } };
            // null content is only valid alongside tool calls.
            if( msg.text.empty() && !msg.toolCalls.empty() )
                m["content"] = nullptr;
            else
                m["content"] = msg.text;

            if( !msg.toolCalls.empty() )
            {
                json calls = json::array();

                for( const COPILOT_TOOL_CALL& call : msg.toolCalls )
                {
                    calls.push_back( { { "id", call.id },
                                       { "type", "function" },
                                       { "function", { { "name", call.name },
                                                       { "arguments", call.input.dump() } } } } );
                }

                m["tool_calls"] = calls;
            }

            messages.push_back( m );
            break;
        }

        case COPILOT_MESSAGE::ROLE::TOOL_RESULT:
        {
            std::string content = msg.isError ? "ERROR: " + msg.toolResult : msg.toolResult;

            if( !msg.attachments.empty() )
                content += "\n[A document was attached, but this provider cannot read attachments.]";

            messages.push_back( { { "role", "tool" },
                                  { "tool_call_id", msg.toolCallId },
                                  { "content", content } } );
            break;
        }
        }
    }

    body["messages"] = messages;

    if( !aReq.tools.empty() )
    {
        json tools = json::array();

        for( const COPILOT_TOOL_DEF& tool : aReq.tools )
        {
            tools.push_back( { { "type", "function" },
                               { "function", { { "name", tool.name },
                                               { "description", tool.description },
                                               { "parameters", tool.inputSchema } } } } );
        }

        body["tools"] = tools;
    }

    COPILOT_HTTP_REQUEST http;
    http.url = BaseUrl() + "/chat/completions";
    http.headers = { { "Content-Type", "application/json" } };

    if( !m_config.apiKey.empty() )
        http.headers.emplace_back( "Authorization", "Bearer " + m_config.apiKey );

    http.body = body.dump( -1, ' ', false, json::error_handler_t::replace );
    return http;
}


COPILOT_RESPONSE COPILOT_OPENAI_PROVIDER::ParseHttpResponse( long aStatus,
                                                             const std::string& aBody ) const
{
    COPILOT_RESPONSE resp;

    if( aStatus < 200 || aStatus >= 300 )
    {
        resp.error = extractError( aStatus, aBody );
        return resp;
    }

    json payload = json::parse( aBody, nullptr, false );

    if( payload.is_discarded() || !payload.is_object() || !payload.contains( "choices" )
        || !payload["choices"].is_array() || payload["choices"].empty() )
    {
        resp.error = "Unexpected response from provider: " + aBody.substr( 0, 400 );
        return resp;
    }

    const json& choice = payload["choices"][0];
    const json  msg = choice.value( "message", json::object() );

    resp.message.role = COPILOT_MESSAGE::ROLE::ASSISTANT;

    if( msg.contains( "content" ) && msg["content"].is_string() )
        resp.message.text = msg["content"].get<std::string>();

    // Some local reasoning models wrap their chain of thought in <think> tags; hide it.
    for( size_t start = resp.message.text.find( "<think>" ); start != std::string::npos;
         start = resp.message.text.find( "<think>" ) )
    {
        size_t end = resp.message.text.find( "</think>", start );

        if( end == std::string::npos )
            break;

        resp.message.text.erase( start, end + 8 - start );
    }

    while( !resp.message.text.empty() && isspace( (unsigned char) resp.message.text.front() ) )
        resp.message.text.erase( resp.message.text.begin() );

    if( msg.contains( "tool_calls" ) && msg["tool_calls"].is_array() )
    {
        int n = 0;

        for( const json& tc : msg["tool_calls"] )
        {
            COPILOT_TOOL_CALL call;
            call.id = tc.value( "id", "" );

            if( call.id.empty() )
                call.id = "call_" + std::to_string( n );

            const json fn = tc.value( "function", json::object() );
            call.name = fn.value( "name", "" );

            if( fn.contains( "arguments" ) )
            {
                const json& args = fn["arguments"];

                if( args.is_object() )
                {
                    call.input = args;      // Ollama sometimes returns a parsed object
                }
                else if( args.is_string() )
                {
                    json parsed = json::parse( args.get<std::string>(), nullptr, false );

                    if( !parsed.is_discarded() && parsed.is_object() )
                        call.input = parsed;
                }
            }

            resp.message.toolCalls.push_back( call );
            ++n;
        }
    }

    std::string finish = choice.contains( "finish_reason" ) && choice["finish_reason"].is_string()
                                 ? choice["finish_reason"].get<std::string>()
                                 : std::string();

    if( !resp.message.toolCalls.empty() || finish == "tool_calls" )
        resp.stopReason = "tool_use";
    else if( finish == "length" )
        resp.stopReason = "max_tokens";
    else
        resp.stopReason = "end_turn";

    if( payload.contains( "usage" ) && payload["usage"].is_object() )
    {
        const json& usage = payload["usage"];
        resp.inputTokens = usage.value( "prompt_tokens", 0 );
        resp.outputTokens = usage.value( "completion_tokens", 0 );
    }

    resp.ok = true;
    return resp;
}
