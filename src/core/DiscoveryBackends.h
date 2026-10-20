// loftail — a desktop viewer for log4cplus logs.
// Copyright (C) 2026 Valentyn Pavliuchenko
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The one function a Bonjour backend supplies, kept out of DiscoveryBrowser.h so that header
// names no platform API. Defined by exactly one of AvahiBrowser.cpp, DnsSdBrowser.cpp and
// WinDnsBrowser.cpp — or by DiscoveryBrowser.cpp's refusal when none of them is built.

#include "DiscoveryBrowser.h"

namespace loftail {

std::unique_ptr<DiscoveryBrowser> makeBonjourBrowser(QObject *parent);

// The ONVIF browser, always built: loftail's own WS-Discovery probe over a QUdpSocket.
std::unique_ptr<DiscoveryBrowser> makeOnvifBrowser(QObject *parent);

} // namespace loftail
