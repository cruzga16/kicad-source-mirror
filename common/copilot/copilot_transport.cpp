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

#include <build_version.h>
#include <kicad_curl/kicad_curl_easy.h>

#include <curl/curl.h>
#include <wx/string.h>


COPILOT_RESPONSE CopilotHttpTransport( const COPILOT_PROVIDER& aProvider,
                                       const COPILOT_REQUEST& aRequest,
                                       const std::function<bool()>& aCancelled )
{
    COPILOT_RESPONSE     response;
    COPILOT_HTTP_REQUEST http = aProvider.BuildHttpRequest( aRequest );

    if( http.url.rfind( "http://", 0 ) != 0 && http.url.rfind( "https://", 0 ) != 0 )
    {
        response.error = "Invalid endpoint URL '" + http.url
                         + "'. Set a base URL in the Copilot settings.";
        return response;
    }

    KICAD_CURL_EASY curl;

    if( !curl.SetURL( http.url ) )
    {
        response.error = "Unable to use endpoint URL '" + http.url + "'.";
        return response;
    }

    curl.SetUserAgent( "KiCad-Copilot/" + std::string( GetBuildVersion().utf8_str() ) );

    // Never follow redirects: custom headers (x-api-key, Authorization) would be sent to
    // whatever host the redirect points at.
    curl.SetFollowRedirects( false );
    curl.SetConnectTimeout( 20 );

    if( aProvider.Config().timeoutSecs > 0 )
        curl.SetTimeout( aProvider.Config().timeoutSecs );

    for( const auto& [name, value] : http.headers )
        curl.SetHeader( name, value );

    curl.SetTransferCallback(
            [&aCancelled]( size_t, size_t, size_t, size_t ) -> int
            {
                return aCancelled && aCancelled() ? 1 : 0;
            },
            0 );

    curl.SetPostFields( http.body );

    int result = curl.Perform();

    if( aCancelled && aCancelled() )
    {
        response.error = "Cancelled.";
        return response;
    }

    if( result != CURLE_OK )
    {
        response.error = "Could not reach " + http.url + ": " + curl.GetErrorText( result );
        return response;
    }

    long status = curl.GetResponseStatusCode();

    if( status >= 300 && status < 400 )
    {
        char* location = nullptr;
        curl_easy_getinfo( curl.GetCurl(), CURLINFO_REDIRECT_URL, &location );

        response.error = "The endpoint " + http.url + " redirected (HTTP " + std::to_string( status )
                         + ( location ? std::string( ") to " ) + location : std::string( ")" ) )
                         + ". Enter the final address as the base URL in the Copilot settings.";
        return response;
    }

    return aProvider.ParseHttpResponse( status, curl.GetBuffer() );
}
