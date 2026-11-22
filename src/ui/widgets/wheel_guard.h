#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QObject>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QAbstractScrollArea>
#include <QtWidgets/QAbstractSlider>
#include <QtWidgets/QAbstractSpinBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QWidget>

// Prevents accidental mouse-wheel changes on spin boxes, sliders, and closed
// combo boxes even when they currently hold keyboard focus. When the guarded
// control sits inside a scroll area (such as a dock panel), the wheel event is
// forwarded to that scroll area's viewport so scrolling the panel still feels
// natural while the control's value stays untouched.
class WheelGuard : public QObject
{
    Q_OBJECT

public:
    explicit WheelGuard(QObject* parent = nullptr) : QObject(parent) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (forwarding_ || event->type() != QEvent::Wheel)
            return false;

        auto* widget = qobject_cast<QWidget*>(watched);
        if (!widget)
            return false;

        // Guard spin boxes, sliders (excluding scroll bars!), and closed combo boxes.
        // Also catch wheel events landing on a spin box's inner QLineEdit child so
        // they cannot bubble up into the spin box and mutate it.
        const bool isControl = isGuardedControl(widget)
            || (widget->parentWidget() && qobject_cast<QAbstractSpinBox*>(widget->parentWidget()) != nullptr);
        if (!isControl)
            return false;

        auto* wheel = static_cast<QWheelEvent*>(event);
        if (QAbstractScrollArea* area = enclosingScrollArea(widget))
        {
            if (QWidget* viewport = area->viewport())
            {
                forwarding_ = true;
                QWheelEvent forwarded(
                    viewport->mapFromGlobal(wheel->globalPosition()),
                    wheel->globalPosition(),
                    wheel->pixelDelta(),
                    wheel->angleDelta(),
                    wheel->buttons(),
                    wheel->modifiers(),
                    wheel->phase(),
                    wheel->inverted(),
                    wheel->source());
                QCoreApplication::sendEvent(viewport, &forwarded);
                forwarding_ = false;
            }
        }
        event->ignore();
        return true;
    }

private:
    static bool isGuardedControl(QWidget* widget)
    {
        if (qobject_cast<QAbstractSpinBox*>(widget) != nullptr) return true;
        if (qobject_cast<QComboBox*>(widget) != nullptr) return true;
        if (qobject_cast<QAbstractSlider*>(widget) != nullptr
            && qobject_cast<QScrollBar*>(widget) == nullptr)
            return true;
        return false;
    }

    static QAbstractScrollArea* enclosingScrollArea(QWidget* widget)
    {
        for (QWidget* p = widget->parentWidget(); p; p = p->parentWidget())
        {
            if (auto* area = qobject_cast<QAbstractScrollArea*>(p))
                return area;
        }
        return nullptr;
    }

    bool forwarding_{false};
};
