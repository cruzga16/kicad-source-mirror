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

#include <copilot/copilot_tool_registry.h>

#include <algorithm>
#include <cmath>


using json = nlohmann::json;


namespace COPILOT_ARGS
{

std::string Str( const json& aIn, const char* aKey, const std::string& aDefault )
{
    if( aIn.is_object() && aIn.contains( aKey ) )
    {
        const json& v = aIn[aKey];

        if( v.is_string() )
            return v.get<std::string>();

        if( v.is_number() || v.is_boolean() )
            return v.dump();
    }

    return aDefault;
}


std::optional<double> Num( const json& aIn, const char* aKey )
{
    if( !aIn.is_object() || !aIn.contains( aKey ) )
        return std::nullopt;

    const json& v = aIn[aKey];
    double      d = 0.0;

    if( v.is_number() )
    {
        d = v.get<double>();
    }
    else if( v.is_string() )
    {
        try
        {
            d = std::stod( v.get<std::string>() );
        }
        catch( ... )
        {
            return std::nullopt;
        }
    }
    else
    {
        return std::nullopt;
    }

    if( !std::isfinite( d ) )
        return std::nullopt;

    return d;
}


int Int( const json& aIn, const char* aKey, int aDefault, int aMin, int aMax )
{
    std::optional<double> v = Num( aIn, aKey );

    if( !v )
        return std::clamp( aDefault, aMin, aMax );

    // Clamp before converting: casting an out of range double to int is undefined.
    return (int) std::clamp( std::round( *v ), (double) aMin, (double) aMax );
}


std::optional<bool> Bool( const json& aIn, const char* aKey )
{
    if( !aIn.is_object() || !aIn.contains( aKey ) )
        return std::nullopt;

    const json& v = aIn[aKey];

    if( v.is_boolean() )
        return v.get<bool>();

    if( v.is_string() )
    {
        std::string s = v.get<std::string>();

        if( s == "true" || s == "yes" || s == "1" )
            return true;

        if( s == "false" || s == "no" || s == "0" )
            return false;
    }

    return std::nullopt;
}


std::vector<std::string> StrList( const json& aIn, const char* aKey )
{
    std::vector<std::string> out;

    if( !aIn.is_object() || !aIn.contains( aKey ) )
        return out;

    const json& v = aIn[aKey];

    if( v.is_array() )
    {
        for( const json& e : v )
        {
            if( e.is_string() )
                out.push_back( e.get<std::string>() );
            else if( e.is_number() )
                out.push_back( e.dump() );
        }
    }
    else if( v.is_string() )
    {
        // Accept "a, b, c" as well.
        std::string s = v.get<std::string>();
        size_t      start = 0;

        while( start <= s.size() )
        {
            size_t      comma = s.find( ',', start );
            std::string part = s.substr( start, comma == std::string::npos ? std::string::npos
                                                                            : comma - start );
            part.erase( 0, part.find_first_not_of( " \t" ) );
            part.erase( part.find_last_not_of( " \t" ) + 1 );

            if( !part.empty() )
                out.push_back( part );

            if( comma == std::string::npos )
                break;

            start = comma + 1;
        }
    }

    return out;
}


json String( const std::string& aDescription )
{
    return { { "type", "string" }, { "description", aDescription } };
}


json Number( const std::string& aDescription )
{
    return { { "type", "number" }, { "description", aDescription } };
}


json Integer( const std::string& aDescription )
{
    return { { "type", "integer" }, { "description", aDescription } };
}


json Boolean( const std::string& aDescription )
{
    return { { "type", "boolean" }, { "description", aDescription } };
}


json StringArray( const std::string& aDescription )
{
    return { { "type", "array" }, { "items", { { "type", "string" } } },
             { "description", aDescription } };
}


json Enum( const std::vector<std::string>& aValues, const std::string& aDescription )
{
    return { { "type", "string" }, { "enum", aValues }, { "description", aDescription } };
}

} // namespace COPILOT_ARGS


void COPILOT_REGISTRY_TOOLBOX::AddTool( const std::string& aName, const std::string& aDescription,
                                        const json& aProperties,
                                        const std::vector<std::string>& aRequired, int aFlags,
                                        HANDLER aHandler )
{
    TOOL tool;
    tool.def.name = aName;
    tool.def.description = aDescription;
    tool.def.modifiesDesign = ( aFlags & MODIFIES ) != 0;
    tool.def.requiresApproval = ( aFlags & ( APPROVAL | ASK_EVERY_TIME ) ) != 0;
    tool.def.allowAlways = ( aFlags & ASK_EVERY_TIME ) == 0;
    tool.def.inputSchema = { { "type", "object" },
                             { "properties", aProperties.is_object() ? aProperties
                                                                     : json::object() } };

    if( !aRequired.empty() )
        tool.def.inputSchema["required"] = aRequired;

    tool.handler = std::move( aHandler );

    if( auto it = m_index.find( aName ); it != m_index.end() )
    {
        m_tools[it->second] = std::move( tool );
        return;
    }

    m_index[aName] = m_tools.size();
    m_tools.push_back( std::move( tool ) );
}


const COPILOT_TOOL_DEF* COPILOT_REGISTRY_TOOLBOX::findDef( const std::string& aName ) const
{
    auto it = m_index.find( aName );
    return it == m_index.end() ? nullptr : &m_tools[it->second].def;
}


std::vector<COPILOT_TOOL_DEF> COPILOT_REGISTRY_TOOLBOX::GetTools( bool aAllowEdits )
{
    std::vector<COPILOT_TOOL_DEF> defs;

    for( const TOOL& tool : m_tools )
    {
        if( aAllowEdits || !tool.def.modifiesDesign )
            defs.push_back( tool.def );
    }

    return defs;
}


COPILOT_TOOL_RESULT COPILOT_REGISTRY_TOOLBOX::ExecuteTool( const std::string& aName,
                                                           const json& aInput )
{
    auto it = m_index.find( aName );

    if( it == m_index.end() )
        return COPILOT_TOOL_RESULT::Error( "Unknown tool '" + aName + "'." );

    if( std::optional<COPILOT_TOOL_RESULT> refused = preExecute( aName ) )
        return *refused;

    struct DEPTH
    {
        explicit DEPTH( int& aCount ) : m_count( aCount ) { ++m_count; }
        ~DEPTH() { --m_count; }
        int& m_count;
    } depth( m_executing );

    return m_tools[it->second].handler( aInput.is_object() ? aInput : json::object() );
}


std::string CopilotUrlHost( const std::string& aUrl )
{
    size_t scheme = aUrl.find( "://" );

    if( scheme == std::string::npos )
        return "";

    size_t      start = scheme + 3;
    size_t      end = aUrl.find_first_of( "/?#", start );
    std::string host = aUrl.substr( start, end == std::string::npos ? std::string::npos
                                                                     : end - start );

    if( size_t at = host.rfind( '@' ); at != std::string::npos )
        host = host.substr( at + 1 );

    if( !host.empty() && host[0] == '[' )
    {
        // IPv6 literal: keep the brackets, drop any port.
        if( size_t close = host.find( ']' ); close != std::string::npos )
            host = host.substr( 0, close + 1 );
    }
    else if( size_t colon = host.find( ':' ); colon != std::string::npos )
    {
        host = host.substr( 0, colon );
    }

    std::transform( host.begin(), host.end(), host.begin(),
                    []( unsigned char c ) { return (char) std::tolower( c ); } );
    return host;
}


std::string CopilotVisibleText( const std::string& aText, size_t aMaxBytes )
{
    // Make everything that could hide part of a command visible in an approval prompt: line
    // breaks, tabs and other control characters, and Unicode direction overrides.
    std::string out;

    for( size_t i = 0; i < aText.size(); ++i )
    {
        unsigned char c = static_cast<unsigned char>( aText[i] );

        if( c == '\n' )
            out += "\xE2\x8F\x8E";                 // U+23CE return symbol
        else if( c == '\t' )
            out += "\xE2\x86\xB9";                 // U+21B9 tab symbol
        else if( c < 0x20 || c == 0x7F )
            out += "\xEF\xBF\xBD";                 // U+FFFD
        else if( c == 0xE2 && i + 2 < aText.size()
                 && ( ( static_cast<unsigned char>( aText[i + 1] ) == 0x80
                        && ( ( static_cast<unsigned char>( aText[i + 2] ) >= 0xAA
                               && static_cast<unsigned char>( aText[i + 2] ) <= 0xAE )
                             || static_cast<unsigned char>( aText[i + 2] ) == 0x8E
                             || static_cast<unsigned char>( aText[i + 2] ) == 0x8F ) )
                      || ( static_cast<unsigned char>( aText[i + 1] ) == 0x81
                           && static_cast<unsigned char>( aText[i + 2] ) >= 0xA6
                           && static_cast<unsigned char>( aText[i + 2] ) <= 0xA9 ) ) )
        {
            // U+200E/F, U+202A..U+202E, U+2066..U+2069
            out += "[direction mark]";
            i += 2;
        }
        else
        {
            out.push_back( static_cast<char>( c ) );
        }
    }

    out = CopilotSanitizeUtf8( out );

    if( out.size() > aMaxBytes )
    {
        size_t more = out.size() - aMaxBytes;
        out = CopilotTruncateUtf8( out, aMaxBytes ) + " ... (" + std::to_string( more )
              + " more bytes)";
    }

    return out;
}


std::string COPILOT_REGISTRY_TOOLBOX::ApprovalKey( const std::string& aName, const json& aInput )
{
    std::string url = COPILOT_ARGS::Str( aInput, "url" );

    if( !url.empty() )
        return aName + "@" + CopilotUrlHost( url );

    return aName;
}


std::string COPILOT_REGISTRY_TOOLBOX::DescribeToolCall( const std::string& aName,
                                                        const json& aInput )
{
    std::string url = COPILOT_ARGS::Str( aInput, "url" );

    if( aName == "run_command" || aName.ends_with( "_run_command" ) )
    {
        std::string text = "Run this command on your computer:\n\n"
                           + CopilotVisibleText( COPILOT_ARGS::Str( aInput, "command" ), 2000 );
        std::string cwd = COPILOT_ARGS::Str( aInput, "working_dir" );

        if( !cwd.empty() )
            text += "\n\nin " + CopilotVisibleText( cwd, 500 );

        return text;
    }

    if( !url.empty() )
    {
        std::string host = CopilotUrlHost( url );
        std::string what = aName.find( "pdf" ) != std::string::npos ? "Download this PDF"
                                                                     : "Read this web page";
        return what + ":\n\n" + url + "\n\n\"Always Allow\" applies to " + host + " only.";
    }

    return COPILOT_TOOLBOX::DescribeToolCall( aName, aInput );
}


std::string COPILOT_REGISTRY_TOOLBOX::HandleMail( const std::string& aPayload )
{
    json req = json::parse( aPayload, nullptr, false );
    json reply;

    if( req.is_discarded() || !req.is_object() )
    {
        reply = { { "content", "Malformed Copilot request." }, { "is_error", true } };
        return reply.dump();
    }

    std::string op = req.value( "op", "" );

    if( op == "list" )
    {
        json tools = json::array();

        for( const TOOL& tool : m_tools )
        {
            tools.push_back( { { "name", tool.def.name },
                               { "description", tool.def.description },
                               { "input_schema", tool.def.inputSchema },
                               { "modifies", tool.def.modifiesDesign },
                               { "approval", tool.def.requiresApproval },
                               { "allow_always", tool.def.allowAlways } } );
        }

        reply = { { "tools", tools } };
    }
    else if( op == "call" )
    {
        COPILOT_TOOL_RESULT result;
        bool                previousEdits = m_editsAllowed;

        // The caller's edit permission applies to forwarded calls.
        SetEditsAllowed( req.value( "allow_edits", false ) );

        try
        {
            result = ExecuteTool( req.value( "name", "" ),
                                  req.contains( "input" ) ? req["input"] : json::object() );
        }
        catch( const std::exception& e )
        {
            result = COPILOT_TOOL_RESULT::Error( std::string( "Tool failed: " ) + e.what() );
        }
        catch( ... )
        {
            result = COPILOT_TOOL_RESULT::Error( "Tool failed with an unexpected error." );
        }

        SetEditsAllowed( previousEdits );

        reply = { { "content", result.content },
                  { "is_error", result.isError },
                  { "summary", result.summary } };

        if( !result.attachments.empty() )
        {
            json atts = json::array();

            for( const COPILOT_ATTACHMENT& a : result.attachments )
                atts.push_back( { { "media_type", a.mediaType }, { "data", a.base64 },
                                  { "title", a.title } } );

            reply["attachments"] = atts;
        }
    }
    else if( op == "context" )
    {
        reply = { { "context", GetContextSnapshot() } };
    }
    else if( op == "ready" )
    {
        std::string why;
        bool        ready = IsReadyForTools( why );
        reply = { { "ready", ready }, { "why", why } };
    }
    else
    {
        reply = { { "content", "Unknown Copilot request '" + op + "'." }, { "is_error", true } };
    }

    return reply.dump( -1, ' ', false, json::error_handler_t::replace );
}


std::string COPILOT_REGISTRY_TOOLBOX::MakeListRequest()
{
    return json( { { "op", "list" } } ).dump();
}


std::string COPILOT_REGISTRY_TOOLBOX::MakeReadyRequest()
{
    return json( { { "op", "ready" } } ).dump();
}


bool COPILOT_REGISTRY_TOOLBOX::ParseReadyReply( const std::string& aReply, std::string& aWhy )
{
    json reply = json::parse( aReply, nullptr, false );

    // An editor that does not understand the request is treated as ready.
    if( reply.is_discarded() || !reply.is_object() || !reply.contains( "ready" ) )
        return true;

    aWhy = reply.value( "why", "" );
    return reply.value( "ready", true );
}


std::string COPILOT_REGISTRY_TOOLBOX::MakeCallRequest( const std::string& aName,
                                                       const json& aInput, bool aAllowEdits )
{
    return json( { { "op", "call" }, { "name", aName }, { "input", aInput },
                   { "allow_edits", aAllowEdits } } )
            .dump( -1, ' ', false, json::error_handler_t::replace );
}


std::vector<COPILOT_TOOL_DEF> COPILOT_REGISTRY_TOOLBOX::ParseToolList( const std::string& aReply )
{
    std::vector<COPILOT_TOOL_DEF> defs;
    json                          reply = json::parse( aReply, nullptr, false );

    if( reply.is_discarded() || !reply.is_object() || !reply.contains( "tools" ) )
        return defs;

    for( const json& t : reply["tools"] )
    {
        COPILOT_TOOL_DEF def;
        def.name = t.value( "name", "" );
        def.description = t.value( "description", "" );
        def.inputSchema = t.contains( "input_schema" ) ? t["input_schema"] : json::object();
        def.modifiesDesign = t.value( "modifies", false );
        def.requiresApproval = t.value( "approval", false );
        def.allowAlways = t.value( "allow_always", true );

        if( !def.name.empty() )
            defs.push_back( def );
    }

    return defs;
}


COPILOT_TOOL_RESULT COPILOT_REGISTRY_TOOLBOX::ParseCallResult( const std::string& aReply )
{
    json reply = json::parse( aReply, nullptr, false );

    if( reply.is_discarded() || !reply.is_object() || !reply.contains( "content" ) )
        return COPILOT_TOOL_RESULT::Error( "The editor did not answer the request." );

    COPILOT_TOOL_RESULT result;
    result.content = reply.value( "content", "" );
    result.isError = reply.value( "is_error", false );
    result.summary = reply.value( "summary", "" );

    if( reply.contains( "attachments" ) && reply["attachments"].is_array() )
    {
        for( const json& a : reply["attachments"] )
            result.attachments.push_back( { a.value( "media_type", "" ), a.value( "data", "" ),
                                            a.value( "title", "" ) } );
    }

    return result;
}
