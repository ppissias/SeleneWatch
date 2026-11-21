#pragma once

class QLabel;
class QWidget;

// Feature-24: one help text per control. A control's tool tip is its help;
// it is also its "What's This?" text (Help → What's This?, Shift+F1), and the
// label that names the control gets a small ⓘ that shows it on a click.

// For every control under root: copies its tool tip to What's This, and adds
// the ⓘ to the label that names it (a form row's label, or a label whose
// buddy it is). Safe to call again after rows are added.
void installControlHelp(QWidget* root);

// Adds the ⓘ to the label, showing the control's current tool tip when
// clicked. Does nothing when the control has no tool tip or the label
// already has one.
void addHelpLink(QLabel* label, QWidget* control);
