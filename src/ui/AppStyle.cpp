#include "AppStyle.h"

namespace loftail {

// Base style left unset on purpose. QProxyStyle then resolves it from the DESKTOP style
// key, not from QApplication::style() — which matters precisely because this instance is
// about to become QApplication::style(), and taking the current one as a base would
// either recurse or hand ownership of a style QApplication also intends to delete.
AppStyle::AppStyle(QObject *parent)
{
    setParent(parent);
}

int AppStyle::styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
                        QStyleHintReturn *returnData) const
{
    if (hint == SH_DialogButtonBox_ButtonsHaveIcons)
        return 0;
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

} // namespace loftail
