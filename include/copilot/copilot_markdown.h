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

#include <string>

/// Escape text for inclusion in HTML.
std::string CopilotHtmlEscape( const std::string& aText );

/**
 * Convert the small subset of Markdown that chat models produce (headings, paragraphs,
 * bullet and numbered lists, fenced code, inline code, bold, italics, simple tables) to
 * HTML that wxHtmlWindow can render.  All input text is escaped; no raw HTML passes through.
 */
std::string CopilotMarkdownToHtml( const std::string& aMarkdown );
