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

#include "DocumentContext.h"

#include "Document.h"
#include "IndexController.h"
#include "LiveController.h"
#include "LogModel.h"

namespace loftail {

DocumentContext::DocumentContext() = default;

DocumentContext::~DocumentContext()
{
    stopWorkers();
    delete model;
    model = nullptr;
    // After the live controller, which holds a pointer to it (setDigestModel).
    delete digestModel;
    digestModel = nullptr;
}

void DocumentContext::stopWorkers()
{
    // Order matters: the live watcher references the model and the Document.
    if (live) {
        live->stop();
        delete live;
        live = nullptr;
    }
    if (controller) {
        controller->cancel();
        delete controller; // dtor joins the worker thread
        controller = nullptr;
    }
    indexing = false;
}

} // namespace loftail
