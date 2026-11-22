#include "ui/widgets/control_help.h"

#include <QtCore/QPointer>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QWhatsThis>
#include <QtWidgets/QWidget>

namespace {

constexpr char kHasHelpLink[] = "selenewatchHelpLink";

// The widget whose tool tip explains a form row's field: the field itself,
// or for a row of several widgets (a slider beside its box), the first one
// with a tool tip.
QWidget* explainedControl(QLayoutItem* field)
{
    if (!field) return nullptr;
    if (QWidget* widget = field->widget())
    {
        if (!widget->toolTip().isEmpty()) return widget;
        for (QWidget* child : widget->findChildren<QWidget*>())
            if (!child->toolTip().isEmpty()) return child;
        return nullptr;
    }
    if (QLayout* layout = field->layout())
        for (int i = 0; i < layout->count(); ++i)
            if (QWidget* explained = explainedControl(layout->itemAt(i))) return explained;
    return nullptr;
}

} // namespace

void addHelpLink(QLabel* label, QWidget* control)
{
    if (!label || !control || control->toolTip().isEmpty() || label->property(kHasHelpLink).toBool()) return;
    if (label->text().isEmpty()) return;
    label->setProperty(kHasHelpLink, true);
    const QString name = label->textFormat() == Qt::RichText ? label->text() : label->text().toHtmlEscaped();
    label->setTextFormat(Qt::RichText);
    // Segoe UI has no ⓘ; Segoe UI Symbol, part of every Windows, has.
    label->setText(QString("%1 <a href=\"help\" style=\"text-decoration:none; font-family:'Segoe UI Symbol'\">ⓘ</a>").arg(name));
    label->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    label->setOpenExternalLinks(false);
    QObject::connect(label, &QLabel::linkActivated, label, [label, explained = QPointer<QWidget>(control)] {
        if (explained) QWhatsThis::showText(label->mapToGlobal(QPoint(0, label->height())), explained->toolTip(), label);
    });
}

void installControlHelp(QWidget* root)
{
    if (!root) return;
    for (QWidget* widget : root->findChildren<QWidget*>())
        if (!widget->toolTip().isEmpty() && widget->whatsThis().isEmpty()) widget->setWhatsThis(widget->toolTip());

    for (QFormLayout* form : root->findChildren<QFormLayout*>())
        for (int row = 0; row < form->rowCount(); ++row)
            if (auto* label = qobject_cast<QLabel*>(form->itemAt(row, QFormLayout::LabelRole)
                    ? form->itemAt(row, QFormLayout::LabelRole)->widget() : nullptr))
                addHelpLink(label, explainedControl(form->itemAt(row, QFormLayout::FieldRole)));

    for (QLabel* label : root->findChildren<QLabel*>())
        if (label->buddy()) addHelpLink(label, label->buddy());
}
