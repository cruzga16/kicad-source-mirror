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

#include <map>

#include <dialog_shim.h>
#include <copilot/copilot_provider.h>
#include <settings/common_settings.h>

class wxCheckBox;
class wxChoice;
class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;


/**
 * Configure the Copilot back end: provider, endpoint, model, API key and limits.
 */
class DIALOG_COPILOT_SETTINGS : public DIALOG_SHIM
{
public:
    DIALOG_COPILOT_SETTINGS( wxWindow* aParent );

    bool TransferDataToWindow() override;
    bool TransferDataFromWindow() override;

private:
    void onProviderChanged( wxCommandEvent& aEvent );
    void onTestConnection( wxCommandEvent& aEvent );

    COPILOT_PROVIDER_TYPE selectedProvider() const;

    /// Save the visible fields into m_settings/m_keys for the given provider.
    void stashFields( COPILOT_PROVIDER_TYPE aType );

    /// Load the fields for the given provider from m_settings/m_keys.
    void loadFields( COPILOT_PROVIDER_TYPE aType );

    COPILOT_CONFIG configFromFields();

    wxChoice*     m_provider;
    wxTextCtrl*   m_baseUrl;
    wxTextCtrl*   m_model;
    wxTextCtrl*   m_apiKey;
    wxStaticText* m_keyHelp;
    wxSpinCtrl*   m_maxTokens;
    wxSpinCtrl*   m_timeout;
    wxStaticText* m_testResult;
    wxCheckBox*   m_webSearch;
    wxTextCtrl*   m_freerouting;

    COMMON_SETTINGS::COPILOT                  m_settings;   ///< Working copy.
    std::map<COPILOT_PROVIDER_TYPE, wxString> m_keys;       ///< Edited keys per provider.
    std::map<COPILOT_PROVIDER_TYPE, bool>     m_keyFromEnv;
    COPILOT_PROVIDER_TYPE                     m_shownProvider;
};
