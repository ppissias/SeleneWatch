#include "ui/cameras/camera_support_dialog.h"
#include "ui/cameras/runtime_chooser.h"

#include <QtCore/QDir>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

namespace {
// Every camera family SeleneWatch knows, so that one missing from this build is listed too.
struct KnownFamily { const char* key; const char* name; };
constexpr KnownFamily kFamilies[] = {
    {"zwo", "ZWO ASI"},
    {"qhy", "QHY"},
    {"playerone", "Player One"},
    {"touptek", "ToupTek, and its other brands"},
};
enum Column { Family, State, Library, Version, Cameras };
} // namespace

CameraSupportDialog::CameraSupportDialog(const std::vector<CameraFamilyPanel*>& families, bool streamRunning, QWidget* parent)
    : QDialog(parent), running_(streamRunning)
{
    setWindowTitle("Camera support");
    setObjectName("cameraSupportDialog");
    resize(820, 380);
    for (const auto& known : kFamilies)
    {
        Row row{known.key, known.name};
        for (auto* panel : families)
            if (panel->sourceKey() == row.key) row.panel = panel;
        rows_.push_back(row);
    }

    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel("The camera families this version of SeleneWatch supports, the SDK library each one uses, and the "
        "cameras connected now. Nothing here opens a camera.", this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    table_ = new QTableWidget(static_cast<int>(rows_.size()), 5, this);
    table_->setObjectName("cameraSupportTable");
    table_->setHorizontalHeaderLabels({"Camera family", "State", "SDK library", "SDK version", "Connected cameras"});
    table_->verticalHeader()->hide();
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setWordWrap(false);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(Library, QHeaderView::Stretch);
    layout->addWidget(table_, 1);
    details_ = new QLabel(this);
    details_->setObjectName("cameraSupportDetails");
    details_->setWordWrap(true);
    details_->setTextFormat(Qt::PlainText);
    details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(details_);

    auto* buttons = new QHBoxLayout;
    choose_ = new QPushButton("Choose DLL…", this);
    choose_->setObjectName("chooseCameraRuntime");
    automatic_ = new QPushButton("Use automatic discovery", this);
    automatic_->setObjectName("automaticCameraRuntime");
    checkAgain_ = new QPushButton("Check again", this);
    checkAgain_->setObjectName("checkCameraSupport");
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->addWidget(choose_);
    buttons->addWidget(automatic_);
    buttons->addWidget(checkAgain_);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(checkAgain_, &QPushButton::clicked, this, &CameraSupportDialog::checkAll);
    connect(table_, &QTableWidget::itemSelectionChanged, this, &CameraSupportDialog::showSelection);
    connect(choose_, &QPushButton::clicked, this, [this] {
        const int row = selectedRow();
        if (row >= 0 && rows_[row].panel && rows_[row].panel->runtimeChooser()) rows_[row].panel->runtimeChooser()->choose();
    });
    connect(automatic_, &QPushButton::clicked, this, [this] {
        const int row = selectedRow();
        if (row >= 0 && rows_[row].panel && rows_[row].panel->runtimeChooser()) rows_[row].panel->runtimeChooser()->setPath({});
    });
    // A new choice of DLL is checked at once. The panel refreshes itself too.
    for (size_t row = 0; row < rows_.size(); ++row)
        if (rows_[row].panel && rows_[row].panel->runtimeChooser())
            connect(rows_[row].panel->runtimeChooser(), &RuntimeChooser::changed, this, [this, row] { check(row); showSelection(); });

    for (size_t row = 0; row < rows_.size(); ++row) showRow(row);
    table_->selectRow(0);
    showSelection();
}

void CameraSupportDialog::checkAll()
{
    if (running_) return;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    for (size_t row = 0; row < rows_.size(); ++row) check(row);
    QGuiApplication::restoreOverrideCursor();
    showSelection();
}

void CameraSupportDialog::check(size_t row)
{
    auto& r = rows_[row];
    if (running_ || !r.panel) return;
    r.support = r.panel->checkSupport();
    r.checked = true;
    showRow(row);
}

void CameraSupportDialog::showRow(size_t row)
{
    const auto& r = rows_[row];
    QString state, library, version, cameras;
    if (!r.panel) state = "Not in this version";
    else if (running_) state = "Stop the stream to check";
    else if (!r.checked) state = "Not checked";
    else if (!r.support.problem.isEmpty()) state = r.support.library.isEmpty() ? "SDK library not found" : "SDK error";
    else state = r.support.cameras.isEmpty() ? "Ready; no camera connected" : "Ready";
    if (r.checked)
    {
        library = QDir::toNativeSeparators(r.support.library);
        version = r.support.sdkVersion;
        cameras = r.support.cameras.isEmpty() ? QString("none") : r.support.cameras.join(", ");
    }
    const QStringList cells{r.name, state, library, version, cameras};
    for (int column = 0; column < cells.size(); ++column)
    {
        auto* item = new QTableWidgetItem(cells[column]);
        if (column == Library) item->setToolTip(library);
        table_->setItem(static_cast<int>(row), column, item);
    }
}

int CameraSupportDialog::selectedRow() const
{
    const auto selected = table_->selectionModel()->selectedRows();
    return selected.isEmpty() ? -1 : selected.front().row();
}

void CameraSupportDialog::showSelection()
{
    const int row = selectedRow();
    RuntimeChooser* chooser = row >= 0 && rows_[row].panel ? rows_[row].panel->runtimeChooser() : nullptr;
    choose_->setEnabled(chooser && !running_);
    automatic_->setEnabled(chooser && !running_ && !chooser->path().isEmpty());
    checkAgain_->setEnabled(!running_);
    if (row < 0) { details_->clear(); return; }
    const auto& r = rows_[row];
    QStringList lines{r.name + ":"};
    if (!r.panel)
        lines << "This version of SeleneWatch was built without this camera family.";
    else
    {
        lines << r.panel->installHint();
        if (chooser)
            lines << (chooser->path().isEmpty() ? QString("SDK library: automatic discovery.")
                : QString("SDK library chosen: %1").arg(QDir::toNativeSeparators(chooser->path())));
        if (running_) lines << "Stop the stream to check or change the camera libraries.";
        else if (r.checked && !r.support.problem.isEmpty()) lines << r.support.problem;
    }
    details_->setText(lines.join('\n'));
}
