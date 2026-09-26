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

#include <copilot/copilot_markdown.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>


std::string CopilotHtmlEscape( const std::string& aText )
{
    std::string out;
    out.reserve( aText.size() + 16 );

    for( char c : aText )
    {
        switch( c )
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c; break;
        }
    }

    return out;
}


static std::string trim( const std::string& aStr )
{
    size_t b = aStr.find_first_not_of( " \t\r" );

    if( b == std::string::npos )
        return "";

    size_t e = aStr.find_last_not_of( " \t\r" );
    return aStr.substr( b, e - b + 1 );
}


/// Inline formatting: `code`, **bold**, *italic* / _italic_.  Input is raw; output is HTML.
static std::string inlineFormat( const std::string& aText )
{
    std::string out;
    size_t      i = 0;
    bool        bold = false;
    bool        italic = false;

    while( i < aText.size() )
    {
        char c = aText[i];

        if( c == '`' )
        {
            size_t end = aText.find( '`', i + 1 );

            if( end != std::string::npos )
            {
                out += "<code>" + CopilotHtmlEscape( aText.substr( i + 1, end - i - 1 ) )
                       + "</code>";
                i = end + 1;
                continue;
            }
        }

        if( c == '*' && i + 1 < aText.size() && aText[i + 1] == '*' )
        {
            out += bold ? "</b>" : "<b>";
            bold = !bold;
            i += 2;
            continue;
        }

        if( ( c == '*' || c == '_' ) && !( c == '_' && i > 0 && isalnum( (unsigned char) aText[i - 1] ) ) )
        {
            // Only treat as emphasis when it opens before a non-space or closes after one.
            bool opens = !italic && i + 1 < aText.size() && !isspace( (unsigned char) aText[i + 1] );
            bool closes = italic && i > 0 && !isspace( (unsigned char) aText[i - 1] );

            if( opens || closes )
            {
                out += italic ? "</i>" : "<i>";
                italic = !italic;
                ++i;
                continue;
            }
        }

        out += CopilotHtmlEscape( std::string( 1, c ) );
        ++i;
    }

    if( italic )
        out += "</i>";

    if( bold )
        out += "</b>";

    return out;
}


static bool isTableSeparator( const std::string& aLine )
{
    std::string t = trim( aLine );

    if( t.size() < 3 || t.find( '-' ) == std::string::npos || t.find( '|' ) == std::string::npos )
        return false;

    for( char c : t )
    {
        if( c != '|' && c != '-' && c != ':' && c != ' ' )
            return false;
    }

    return true;
}


static std::vector<std::string> splitRow( const std::string& aLine )
{
    std::string t = trim( aLine );

    if( !t.empty() && t.front() == '|' )
        t.erase( 0, 1 );

    if( !t.empty() && t.back() == '|' )
        t.pop_back();

    std::vector<std::string> cells;
    std::string              cell;

    for( char c : t )
    {
        if( c == '|' )
        {
            cells.push_back( trim( cell ) );
            cell.clear();
        }
        else
        {
            cell += c;
        }
    }

    cells.push_back( trim( cell ) );
    return cells;
}


std::string CopilotMarkdownToHtml( const std::string& aMarkdown )
{
    std::vector<std::string> lines;
    {
        std::stringstream ss( aMarkdown );
        std::string       line;

        while( std::getline( ss, line ) )
        {
            if( !line.empty() && line.back() == '\r' )
                line.pop_back();

            lines.push_back( line );
        }
    }

    std::string out;
    std::string para;
    std::string listType;   // "ul", "ol" or empty

    auto flushPara = [&]()
    {
        if( !para.empty() )
        {
            out += "<p>" + inlineFormat( para ) + "</p>";
            para.clear();
        }
    };

    auto closeList = [&]()
    {
        if( !listType.empty() )
        {
            out += "</" + listType + ">";
            listType.clear();
        }
    };

    for( size_t i = 0; i < lines.size(); ++i )
    {
        const std::string& raw = lines[i];
        std::string        t = trim( raw );

        // Fenced code block
        if( t.rfind( "```", 0 ) == 0 )
        {
            flushPara();
            closeList();

            std::string code;

            for( ++i; i < lines.size() && trim( lines[i] ).rfind( "```", 0 ) != 0; ++i )
                code += lines[i] + "\n";

            if( !code.empty() && code.back() == '\n' )
                code.pop_back();

            out += "<pre>" + CopilotHtmlEscape( code ) + "</pre>";
            continue;
        }

        if( t.empty() )
        {
            flushPara();
            closeList();
            continue;
        }

        // Headings
        if( t[0] == '#' )
        {
            size_t level = t.find_first_not_of( '#' );

            if( level != std::string::npos && level <= 6 && t[level] == ' ' )
            {
                flushPara();
                closeList();
                // Keep headings small; the pane is narrow.
                int h = std::min<int>( 6, (int) level + 2 );
                out += "<h" + std::to_string( h ) + ">" + inlineFormat( trim( t.substr( level ) ) )
                       + "</h" + std::to_string( h ) + ">";
                continue;
            }
        }

        // Horizontal rule
        if( t == "---" || t == "***" || t == "___" )
        {
            flushPara();
            closeList();
            out += "<hr>";
            continue;
        }

        // Table
        if( t.find( '|' ) != std::string::npos && i + 1 < lines.size()
            && isTableSeparator( lines[i + 1] ) )
        {
            flushPara();
            closeList();

            out += "<table border=\"1\" cellspacing=\"0\" cellpadding=\"3\"><tr>";

            for( const std::string& cell : splitRow( t ) )
                out += "<th>" + inlineFormat( cell ) + "</th>";

            out += "</tr>";

            for( i += 2; i < lines.size() && trim( lines[i] ).find( '|' ) != std::string::npos; ++i )
            {
                out += "<tr>";

                for( const std::string& cell : splitRow( lines[i] ) )
                    out += "<td>" + inlineFormat( cell ) + "</td>";

                out += "</tr>";
            }

            --i;
            out += "</table>";
            continue;
        }

        // Bullet list
        if( ( t[0] == '-' || t[0] == '*' || t[0] == '+' ) && t.size() > 1 && t[1] == ' ' )
        {
            flushPara();

            if( listType != "ul" )
            {
                closeList();
                out += "<ul>";
                listType = "ul";
            }

            out += "<li>" + inlineFormat( trim( t.substr( 2 ) ) ) + "</li>";
            continue;
        }

        // Numbered list
        {
            size_t d = 0;

            while( d < t.size() && isdigit( (unsigned char) t[d] ) )
                ++d;

            if( d > 0 && d + 1 < t.size() && ( t[d] == '.' || t[d] == ')' ) && t[d + 1] == ' ' )
            {
                flushPara();

                if( listType != "ol" )
                {
                    closeList();
                    out += "<ol>";
                    listType = "ol";
                }

                out += "<li>" + inlineFormat( trim( t.substr( d + 2 ) ) ) + "</li>";
                continue;
            }
        }

        // Continuation of a list item (indented line)
        if( !listType.empty() && ( raw[0] == ' ' || raw[0] == '\t' ) )
        {
            // Append to the previous <li>
            size_t pos = out.rfind( "</li>" );

            if( pos != std::string::npos )
            {
                out.insert( pos, " " + inlineFormat( t ) );
                continue;
            }
        }

        closeList();

        if( !para.empty() )
            para += "\n";

        para += t;
    }

    flushPara();
    closeList();

    // Hard line breaks inside paragraphs
    std::string result;
    result.reserve( out.size() );

    for( char c : out )
    {
        if( c == '\n' )
            result += "<br>";
        else
            result += c;
    }

    // <pre> content must keep its newlines; restore them.
    size_t start = 0;

    while( ( start = result.find( "<pre>", start ) ) != std::string::npos )
    {
        size_t end = result.find( "</pre>", start );

        if( end == std::string::npos )
            break;

        std::string inner = result.substr( start + 5, end - start - 5 );
        size_t      p = 0;

        while( ( p = inner.find( "<br>", p ) ) != std::string::npos )
            inner.replace( p, 4, "\n" );

        result.replace( start + 5, end - start - 5, inner );
        start = start + 5 + inner.size() + 6;
    }

    return result;
}
