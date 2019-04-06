/*
    Copyright 2015-2018 Clément Gallet <clement.gallet@ens-lyon.org>

    This file is part of libTAS.

    libTAS is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    libTAS is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with libTAS.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <QInputDialog>
#include <QHeaderView>

#include "InputEditorView.h"
#include "MainWindow.h"

InputEditorView::InputEditorView(Context* c, QWidget *parent) : QTableView(parent), context(c)
{
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setShowGrid(true);
    setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

    MovieFile *movie = nullptr;
    MainWindow *mw = qobject_cast<MainWindow*>(parent->parent());
    if (mw) {
        movie = &mw->gameLoop->movie;
    }

    inputEditorModel = new InputEditorModel(context, movie);
    setModel(inputEditorModel);

    connect(inputEditorModel, &InputEditorModel::inputSetChanged, this, &InputEditorView::resizeAllColumns);

    /* Horizontal header */
    // horizontalHeader()->setMinimumSectionSize(20);
    horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    horizontalHeader()->setResizeContentsPrecision(1);

    /* Savestate column is fixed */
    horizontalHeader()->resizeSection(0, 20);

    /* Frame column is fixed */
    horizontalHeader()->resizeSection(1, 80);

    horizontalHeader()->setSectionsMovable(true);
    horizontalHeader()->setHighlightSections(false);
    horizontalHeader()->setDropIndicatorShown(true);
    horizontalHeader()->setDragEnabled(true);
    horizontalHeader()->setDragDropMode(QTableView::InternalMove);

    horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(horizontalHeader(), &QWidget::customContextMenuRequested, this, &InputEditorView::horizontalMenu);

    /* Horizontal menu */
    horMenu = new QMenu(this);
    horMenu->addAction(tr("Rename label"), this, &InputEditorView::renameLabel);
    horMenu->addAction(tr("Add input column"), this, &InputEditorView::addInputColumn);
    horMenu->addAction(tr("Clear input column"), this, &InputEditorView::clearInputColumn);
    lockAction = horMenu->addAction(tr("Lock input column"), this, &InputEditorView::lockInputColumn);
    lockAction->setCheckable(true);

    /* Vertical header */
    verticalHeader()->setVisible(false);
    verticalHeader()->setDefaultSectionSize(verticalHeader()->minimumSectionSize());

    /* Main menu */
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, &InputEditorView::mainMenu);

    menu = new QMenu(this);
    QAction* a = menu->addAction(tr("Insert"), this, &InputEditorView::insertInput, QKeySequence(Qt::CTRL + Qt::Key_Plus));

    /* Shortcuts for context menus are special, they won't work by default
     * because the menu is hidden, so actions need to be added to the View as
     * well. Also, starting Qt 5.10, shortcuts are hidden in context menus
     * for almost all platform styles (except KDE apparently?), and
     * the option to globally enable them (AA_DontShowShortcutsInContextMenus)
     * is buggy, so we must enable for every single action.
     */
#if (QT_VERSION >= QT_VERSION_CHECK(5, 10, 0))
    a->setShortcutVisibleInContextMenu(true);
#endif

    this->addAction(a);

    menu->addAction(tr("Insert # frames"), this, &InputEditorView::insertInputs);

    a = menu->addAction(tr("Delete"), this, &InputEditorView::deleteInput, QKeySequence(Qt::CTRL + Qt::Key_Minus));
#if (QT_VERSION >= QT_VERSION_CHECK(5, 10, 0))
    a->setShortcutVisibleInContextMenu(true);
#endif
    this->addAction(a);

    menu->addAction(tr("Truncate"), this, &InputEditorView::truncateInputs);
    a = menu->addAction(tr("Clear"), this, &InputEditorView::clearInput, QKeySequence::Delete);
#if (QT_VERSION >= QT_VERSION_CHECK(5, 10, 0))
    a->setShortcutVisibleInContextMenu(true);
#endif
    this->addAction(a);

    menu->addSeparator();
    a = menu->addAction(tr("Copy"), this, &InputEditorView::copyInputs, QKeySequence::Copy);
#if (QT_VERSION >= QT_VERSION_CHECK(5, 10, 0))
    a->setShortcutVisibleInContextMenu(true);
#endif
    this->addAction(a);

    a = menu->addAction(tr("Cut"), this, &InputEditorView::cutInputs, QKeySequence::Cut);
#if (QT_VERSION >= QT_VERSION_CHECK(5, 10, 0))
    a->setShortcutVisibleInContextMenu(true);
#endif
    this->addAction(a);

    a = menu->addAction(tr("Paste"), this, &InputEditorView::pasteInputs, QKeySequence::Paste);
#if (QT_VERSION >= QT_VERSION_CHECK(5, 10, 0))
    a->setShortcutVisibleInContextMenu(true);
#endif
    this->addAction(a);

    a = menu->addAction(tr("Paste Insert"), this, &InputEditorView::pasteInsertInputs, QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_V));
#if (QT_VERSION >= QT_VERSION_CHECK(5, 10, 0))
    a->setShortcutVisibleInContextMenu(true);
#endif
    this->addAction(a);

    keyDialog = new KeyPressedDialog(this);
}

void InputEditorView::resizeAllColumns()
{

    resizeColumnsToContents();
    horizontalHeader()->resizeSection(0, 20);
    horizontalHeader()->resizeSection(1, 80);

    /* Resize analog columns to a fixed value */
    for (int c = 2; c < inputEditorModel->columnCount(); c++)
        if (inputEditorModel->isInputAnalog(c)) {
            int size = horizontalHeader()->sectionSize(c);
            if (size < 70)
                horizontalHeader()->resizeSection(c, 70);
        }

}

void InputEditorView::update()
{
    inputEditorModel->update();

    /* Scroll to make the current frame visible */
    QModelIndex index = inputEditorModel->index(context->framecount-1, 0);
    if (index.isValid())
        scrollTo(index, QAbstractItemView::PositionAtCenter);
}

void InputEditorView::resetInputs()
{
    inputEditorModel->resetInputs();
}

void InputEditorView::mousePressEvent(QMouseEvent *event)
{
    mouseSection = -1;

    if (event->button() != Qt::LeftButton) {
        return QTableView::mousePressEvent(event);
    }

    /* Get the table cell under the mouse position */
    const QModelIndex index = indexAt(event->pos());
    if (!index.isValid()) {
        return QTableView::mousePressEvent(event);
    }

    if (index.column() < 2) {
        return QTableView::mousePressEvent(event);
    }

    selectionModel()->clear();
    mouseSection = index.column();

    /* For editable items, copy the value. Else, copy the opposite value */
    if (inputEditorModel->flags(index) & Qt::ItemIsEditable) {
        mouseValue = inputEditorModel->data(index, Qt::EditRole).toInt(nullptr);
        return QTableView::mousePressEvent(event);
    }
    else {
        mouseValue = inputEditorModel->toggleInput(index);
    }

    event->accept();
}

void InputEditorView::mouseMoveEvent(QMouseEvent *event)
{
    /* Check if the mouse press event was valid */
    if (mouseSection < 0) {
        return QTableView::mouseMoveEvent(event);
    }

    if (!(event->buttons() & Qt::LeftButton)) {
        return QTableView::mouseMoveEvent(event);
    }

    /* Get the table cell under the mouse position */
    const QModelIndex index = indexAt(event->pos());
    if (!index.isValid()) {
        return QTableView::mouseMoveEvent(event);
    }

    /* Toggle the cell with the same row as the cell under the mouse */
    QModelIndex toggle_index = inputEditorModel->index(index.row(), mouseSection);

    inputEditorModel->setData(toggle_index, QVariant(mouseValue), Qt::EditRole);
    event->accept();
}

void InputEditorView::horizontalMenu(QPoint pos)
{
    /* Storing the index of the section where context menu was shown */
    contextSection = horizontalHeader()->logicalIndexAt(pos);

    if (contextSection < 2)
        return;

    /* Update the status of the lock action */
    lockAction->setChecked(inputEditorModel->isLockedUniqueInput(contextSection));

    /* Display the context menu */
    horMenu->popup(horizontalHeader()->viewport()->mapToGlobal(pos));
}

void InputEditorView::renameLabel()
{
    if (contextSection < 2)
        return;

    QString text = QString("New label for input %1 is: ").arg(inputEditorModel->inputDescription(contextSection).c_str());
    QString newLabel = QInputDialog::getText(this, tr("Rename label"), text, QLineEdit::Normal, QString(inputEditorModel->inputLabel(contextSection).c_str()));

    if (!newLabel.isEmpty()) {
        inputEditorModel->renameLabel(contextSection, newLabel.toStdString());
    }
}

void InputEditorView::addInputColumn()
{
    /* Get an input from the user */
    xcb_keysym_t ks = keyDialog->exec();

    /* Get the mapped input */
    if (context->config.km.input_mapping.find(ks) != context->config.km.input_mapping.end()) {
        SingleInput si = context->config.km.input_mapping[ks];
        si.description = context->config.km.input_description(ks);
        inputEditorModel->addUniqueInput(si);
    }
}

void InputEditorView::clearInputColumn()
{
    if (contextSection < 2)
        return;

    inputEditorModel->clearUniqueInput(contextSection);
}

void InputEditorView::lockInputColumn(bool checked)
{
    if (contextSection < 2)
        return;

    inputEditorModel->lockUniqueInput(contextSection, checked);
}

void InputEditorView::mainMenu(QPoint pos)
{
    /* Storing the index of the section where context menu was shown */
    // contextSection = inputEditorView->verticalHeader()->logicalIndexAt(pos);

    /* Display the context menu */
    menu->popup(viewport()->mapToGlobal(pos));
}

void InputEditorView::insertInput()
{
    const QModelIndex index = selectionModel()->currentIndex();

    /* If no row was selected, return */
    if (!index.isValid())
        return;

    inputEditorModel->insertRows(index.row(), 1);
}

void InputEditorView::insertInputs()
{
    const QModelIndex index = selectionModel()->currentIndex();

    /* If no row was selected, return */
    if (!index.isValid())
        return;

    bool ok;
    int nbFrames = QInputDialog::getInt(this, tr("Insert frames"), tr("Number of frames to insert: "), 1, 0, 100000, 1, &ok);

    if (ok) {
        inputEditorModel->insertRows(index.row(), nbFrames);
    }
}

void InputEditorView::deleteInput()
{
    if (!selectionModel()->hasSelection())
        return;

    /* Selection mode garanties that we select a contiguous range, so getting
     * the min and max row.
     */
    int min_row = 2000000000;
    int max_row = -1;
    for (const QModelIndex index : selectionModel()->selectedRows()) {
        min_row = (index.row()<min_row)?index.row():min_row;
        max_row = (index.row()>max_row)?index.row():max_row;
    }
    inputEditorModel->removeRows(min_row, max_row-min_row+1);

    /* Select the next frame */
    QModelIndex newSel = inputEditorModel->index(min_row, 0);
    selectionModel()->clear();
    selectionModel()->select(QItemSelection(newSel, newSel), QItemSelectionModel::Select | QItemSelectionModel::Rows);
}

void InputEditorView::truncateInputs()
{
    const QModelIndex index = selectionModel()->currentIndex();

    /* If no row was selected, return */
    if (!index.isValid())
        return;

    int nbRows = inputEditorModel->rowCount();

    inputEditorModel->removeRows(index.row()+1, nbRows-index.row()-1);
}

void InputEditorView::clearInput()
{
    for (const QModelIndex index : selectionModel()->selectedRows()) {
        inputEditorModel->clearInput(index.row());
    }
}

void InputEditorView::copyInputs()
{
    if (!selectionModel()->hasSelection())
        return;

    /* Selection mode garanties that we select a contiguous range, so getting
     * the min and max row.
     */
    int min_row = 2000000000;
    int max_row = -1;
    for (const QModelIndex index : selectionModel()->selectedRows()) {
        min_row = (index.row()<min_row)?index.row():min_row;
        max_row = (index.row()>max_row)?index.row():max_row;
    }
    inputEditorModel->copyInputs(min_row, max_row-min_row+1);
}

void InputEditorView::cutInputs()
{
    if (!selectionModel()->hasSelection())
        return;

    /* Selection mode garanties that we select a contiguous range, so getting
     * the min and max row.
     */
    int min_row = 2000000000;
    int max_row = -1;
    for (const QModelIndex index : selectionModel()->selectedRows()) {
        min_row = (index.row()<min_row)?index.row():min_row;
        max_row = (index.row()>max_row)?index.row():max_row;
    }
    inputEditorModel->copyInputs(min_row, max_row-min_row+1);
    inputEditorModel->removeRows(min_row, max_row-min_row+1);

}

void InputEditorView::pasteInputs()
{
    const QModelIndex index = selectionModel()->currentIndex();

    /* If no row was selected, return */
    if (!index.isValid())
        return;

    int nbFrames = inputEditorModel->pasteInputs(index.row());

    /* Select the pasted inputs */
    QModelIndex top = inputEditorModel->index(index.row(), 0);
    QModelIndex bottom = inputEditorModel->index(index.row()+nbFrames-1, 0);
    selectionModel()->clear();
    selectionModel()->select(QItemSelection(top, bottom), QItemSelectionModel::Select | QItemSelectionModel::Rows);
}

void InputEditorView::pasteInsertInputs()
{
    const QModelIndex index = selectionModel()->currentIndex();

    /* If no row was selected, return */
    if (!index.isValid())
        return;

    int nbFrames = inputEditorModel->pasteInsertInputs(index.row());

    /* Select the pasted inputs */
    QModelIndex top = inputEditorModel->index(index.row(), 0);
    QModelIndex bottom = inputEditorModel->index(index.row()+nbFrames-1, 0);
    selectionModel()->clear();
    selectionModel()->select(QItemSelection(top, bottom), QItemSelectionModel::Select | QItemSelectionModel::Rows);
}
