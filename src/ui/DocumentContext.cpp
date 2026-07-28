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
