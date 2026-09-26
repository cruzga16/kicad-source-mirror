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

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

/**
 * @file copilot_provider.h
 *
 * Provider-neutral data model for the KiCad Copilot and the wire adapters that translate it
 * to and from the HTTP APIs of the supported model back ends.
 *
 * Nothing in this file touches wxWidgets or the network so it can be unit tested in isolation;
 * the HTTP round trip lives in COPILOT_TRANSPORT (copilot_transport.h).
 */

enum class COPILOT_PROVIDER_TYPE
{
    ANTHROPIC = 0,       ///< Anthropic Messages API (Claude models).
    OPENAI,              ///< OpenAI Chat Completions API.
    OPENAI_COMPATIBLE,   ///< Any server speaking the OpenAI Chat Completions dialect
                         ///< (OpenRouter, LM Studio, vLLM, llama.cpp server, ...).
    OLLAMA               ///< Local Ollama server via its OpenAI-compatible endpoint.
};


/// Stable string id used in settings files and the secret store.
std::string CopilotProviderId( COPILOT_PROVIDER_TYPE aType );

/// Parse a stable id back to a provider type; unknown ids map to ANTHROPIC.
COPILOT_PROVIDER_TYPE CopilotProviderFromId( const std::string& aId );

/// Default endpoint base URL for a provider (empty for OPENAI_COMPATIBLE).
std::string CopilotDefaultBaseUrl( COPILOT_PROVIDER_TYPE aType );

/// A reasonable default model name for a provider.
std::string CopilotDefaultModel( COPILOT_PROVIDER_TYPE aType );

/// True if the provider normally requires an API key.
bool CopilotProviderNeedsKey( COPILOT_PROVIDER_TYPE aType );

/// Shorten @a aText to at most @a aMaxBytes without splitting a UTF-8 sequence.
std::string CopilotTruncateUtf8( const std::string& aText, size_t aMaxBytes );

/// Replace invalid UTF-8 sequences with U+FFFD so the text can be displayed and serialised.
std::string CopilotSanitizeUtf8( const std::string& aText );


struct COPILOT_CONFIG
{
    COPILOT_PROVIDER_TYPE provider = COPILOT_PROVIDER_TYPE::ANTHROPIC;
    std::string           baseUrl;          ///< Empty means provider default.
    std::string           model;            ///< Empty means provider default.
    std::string           apiKey;
    int                   maxTokens = 4096;
    int                   timeoutSecs = 180;
    bool                  webSearch = true;         ///< Anthropic server-side web search.
};


/// Binary content attached to a tool result (e.g. a datasheet PDF).
struct COPILOT_ATTACHMENT
{
    std::string mediaType;      ///< e.g. application/pdf
    std::string base64;
    std::string title;
};


struct COPILOT_TOOL_CALL
{
    std::string    id;
    std::string    name;
    nlohmann::json input = nlohmann::json::object();
};


struct COPILOT_MESSAGE
{
    enum class ROLE
    {
        USER,
        ASSISTANT,
        TOOL_RESULT
    };

    ROLE                           role = ROLE::USER;

    /// Text content for USER and ASSISTANT messages.
    std::string                    text;

    /// Tool invocations requested by an ASSISTANT message.
    std::vector<COPILOT_TOOL_CALL> toolCalls;

    /// For TOOL_RESULT messages: which call this answers, and the (JSON or text) result.
    std::string                    toolCallId;
    std::string                    toolName;
    std::string                    toolResult;
    bool                           isError = false;
    std::vector<COPILOT_ATTACHMENT> attachments;

    /// Provider-native content blocks for an ASSISTANT message (e.g. Anthropic server tool
    /// use and web search results) that must be sent back verbatim.  Null if unused.
    nlohmann::json                 providerContent;

    static COPILOT_MESSAGE User( const std::string& aText )
    {
        COPILOT_MESSAGE m;
        m.role = ROLE::USER;
        m.text = aText;
        return m;
    }

    static COPILOT_MESSAGE ToolResult( const COPILOT_TOOL_CALL& aCall, const std::string& aResult,
                                       bool aIsError )
    {
        COPILOT_MESSAGE m;
        m.role = ROLE::TOOL_RESULT;
        m.toolCallId = aCall.id;
        m.toolName = aCall.name;
        m.toolResult = aResult;
        m.isError = aIsError;
        return m;
    }
};


struct COPILOT_TOOL_DEF
{
    std::string    name;
    std::string    description;
    nlohmann::json inputSchema = nlohmann::json::object();   ///< JSON schema (type: object)
    bool           modifiesDesign = false;
    bool           requiresApproval = false;    ///< Ask the user before running it.
    bool           allowAlways = true;          ///< Approval may be granted for the session.
};


struct COPILOT_REQUEST
{
    std::string                   system;
    std::vector<COPILOT_MESSAGE>  messages;
    std::vector<COPILOT_TOOL_DEF> tools;
};


struct COPILOT_RESPONSE
{
    bool            ok = false;
    std::string     error;          ///< Human readable error if !ok.
    COPILOT_MESSAGE message;        ///< The ASSISTANT message (text and/or tool calls).
    std::string     stopReason;     ///< Normalized: "end_turn", "tool_use", "max_tokens", other.
    int             inputTokens = 0;
    int             outputTokens = 0;
};


struct COPILOT_HTTP_REQUEST
{
    std::string                                      url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string                                      body;
};


/**
 * Translates COPILOT_REQUEST/RESPONSE to a specific vendor wire format.
 */
class COPILOT_PROVIDER
{
public:
    explicit COPILOT_PROVIDER( const COPILOT_CONFIG& aConfig ) : m_config( aConfig ) {}
    virtual ~COPILOT_PROVIDER() = default;

    virtual COPILOT_HTTP_REQUEST BuildHttpRequest( const COPILOT_REQUEST& aRequest ) const = 0;

    virtual COPILOT_RESPONSE ParseHttpResponse( long aStatus, const std::string& aBody ) const = 0;

    const COPILOT_CONFIG& Config() const { return m_config; }

    std::string BaseUrl() const;
    std::string Model() const;

    static std::unique_ptr<COPILOT_PROVIDER> Create( const COPILOT_CONFIG& aConfig );

protected:
    /// Pull a readable message out of a vendor error payload.
    static std::string extractError( long aStatus, const std::string& aBody );

    COPILOT_CONFIG m_config;
};


class COPILOT_ANTHROPIC_PROVIDER : public COPILOT_PROVIDER
{
public:
    using COPILOT_PROVIDER::COPILOT_PROVIDER;

    COPILOT_HTTP_REQUEST BuildHttpRequest( const COPILOT_REQUEST& aRequest ) const override;
    COPILOT_RESPONSE ParseHttpResponse( long aStatus, const std::string& aBody ) const override;
};


class COPILOT_OPENAI_PROVIDER : public COPILOT_PROVIDER
{
public:
    using COPILOT_PROVIDER::COPILOT_PROVIDER;

    COPILOT_HTTP_REQUEST BuildHttpRequest( const COPILOT_REQUEST& aRequest ) const override;
    COPILOT_RESPONSE ParseHttpResponse( long aStatus, const std::string& aBody ) const override;
};
