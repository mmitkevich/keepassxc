/*
 *  Copyright (C) 2020 KeePassXC Team <team@keepassxc.org>
 *  Copyright (C) 2012 Felix Geyer <debfx@fobos.de>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 or (at your option)
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "AutoTypeSelectDialog.h"
#include "ui_AutoTypeSelectDialog.h"

#include <QApplication>
#include <QCloseEvent>
#include <QMenu>
#include <QShortcut>
#include <QSortFilterProxyModel>
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#include <QScreen>
#else
#include <QDesktopWidget>
#endif

#include "core/Config.h"
#include "core/Database.h"
#include "core/Entry.h"
#include "core/EntrySearcher.h"
#include "gui/Clipboard.h"
#include "gui/Icons.h"

AutoTypeSelectDialog::AutoTypeSelectDialog(QWidget* parent)
    : QDialog(parent)
    , m_ui(new Ui::AutoTypeSelectDialog())
{
    setAttribute(Qt::WA_DeleteOnClose);
    // Places the window on the active (virtual) desktop instead of where the main window is.
    setAttribute(Qt::WA_X11BypassTransientForHint);
    setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
    setWindowIcon(icons()->applicationIcon());

    buildActionMenu();

    m_ui->setupUi(this);

#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    auto screen = QApplication::screenAt(QCursor::pos());
    if (!screen) {
        // screenAt can return a nullptr, default to the primary screen
        screen = QApplication::primaryScreen();
    }
    QRect screenGeometry = screen->availableGeometry();
#else
    QRect screenGeometry = QApplication::desktop()->availableGeometry(QCursor::pos());
#endif
    QSize size = config()->get(Config::GUI_AutoTypeSelectDialogSize).toSize();
    size.setWidth(qMin(size.width(), screenGeometry.width()));
    size.setHeight(qMin(size.height(), screenGeometry.height()));
    resize(size);

    // move dialog to the center of the screen
    auto screenCenter = screenGeometry.center();
    move(screenCenter.x() - (size.width() / 2), screenCenter.y() - (size.height() / 2));

    connect(m_ui->view, &AutoTypeMatchView::matchActivated, this, &AutoTypeSelectDialog::submitAutoTypeMatch);
    connect(m_ui->view, &AutoTypeMatchView::currentMatchChanged, this, &AutoTypeSelectDialog::updateActionMenu);
    connect(m_ui->view, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (m_ui->view->currentMatch().first) {
            m_actionMenu->popup(m_ui->view->viewport()->mapToGlobal(pos));
        }
    });


    m_ui->search->setFocus();
    m_ui->search->installEventFilter(this);

    m_searchTimer.setInterval(300);
    m_searchTimer.setSingleShot(true);

    connect(m_ui->search, SIGNAL(textChanged(QString)), &m_searchTimer, SLOT(start()));
    connect(m_ui->search, SIGNAL(returnPressed()), SLOT(activateCurrentMatch()));
    connect(&m_searchTimer, SIGNAL(timeout()), SLOT(performSearch()));

    connect(m_ui->filterRadio, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) {
            // Reset to original match list
            m_ui->view->setMatchList(m_matches);
            performSearch();
            m_ui->search->setFocus();
        }
    });
    connect(m_ui->searchRadio, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) {
            performSearch();
            m_ui->search->setFocus();
        }
    });

    m_ui->action->setMenu(m_actionMenu);
    m_ui->action->installEventFilter(this);
    connect(m_ui->action, &QToolButton::clicked, this, &AutoTypeSelectDialog::activateCurrentMatch);

    connect(m_ui->cancelButton, SIGNAL(clicked()), SLOT(reject()));
}

void AutoTypeSelectDialog::setMatches(const QList<AutoTypeMatch>& matches, const QList<QSharedPointer<Database>>& dbs)
{
    m_matches = matches;
    m_dbs = dbs;

    m_ui->view->setMatchList(m_matches);
    if (m_matches.isEmpty()) {
        m_ui->searchRadio->setChecked(true);
    } else {
        m_ui->filterRadio->setChecked(true);
    }
}

void AutoTypeSelectDialog::submitAutoTypeMatch(AutoTypeMatch match)
{
    accept();
    m_accepted = true;
    emit matchActivated(match);
}

void AutoTypeSelectDialog::performSearch()
{
    if (m_ui->filterRadio->isChecked()) {
        m_ui->view->filterList(m_ui->search->text());
        return;
    }
    if (m_ui->search->text().isEmpty()) {
        m_ui->view->setMatchList({});
        return;
    }

    EntrySearcher searcher;
    QList<AutoTypeMatch> matches;
    for (const auto& db : m_dbs) {
        auto found = searcher.search(m_ui->search->text(), db->rootGroup());
        for (auto entry : found) {
            QSet<QString> sequences;
            auto defSequence = entry->effectiveAutoTypeSequence();
            if (!defSequence.isEmpty()) {
                matches.append({entry, defSequence});
                sequences << defSequence;
            }
            for (auto assoc : entry->autoTypeAssociations()->getAll()) {
                if (!sequences.contains(assoc.sequence) && !assoc.sequence.isEmpty()) {
                    matches.append({entry, assoc.sequence});
                    sequences << assoc.sequence;
                }
            }
        }
    }

    m_ui->view->setMatchList(matches);
}

void AutoTypeSelectDialog::moveSelectionUp()
{
    auto current = m_ui->view->currentIndex();
    auto previous = current.sibling(current.row() - 1, 0);

    if (previous.isValid()) {
        m_ui->view->setCurrentIndex(previous);
    }
}

void AutoTypeSelectDialog::moveSelectionDown()
{
    auto current = m_ui->view->currentIndex();
    auto next = current.sibling(current.row() + 1, 0);

    if (next.isValid()) {
        m_ui->view->setCurrentIndex(next);
    }
}

void AutoTypeSelectDialog::activateCurrentMatch()
{
    submitAutoTypeMatch(m_ui->view->currentMatch());
}

bool AutoTypeSelectDialog::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_ui->action) {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Down) {
            m_ui->action->showMenu();
            return true;
        }
    } else if (obj == m_ui->search) {
        if (event->type() == QEvent::KeyPress) {
            QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
            switch (keyEvent->key()) {
            case Qt::Key_Up:
                moveSelectionUp();
                return true;
            case Qt::Key_Down:
                moveSelectionDown();
                return true;
            case Qt::Key_Escape:
                if (m_ui->search->text().isEmpty()) {
                    reject();
                } else {
                    m_ui->search->clear();
                }
                return true;
            default:
                break;
            }
        }
    }

    return QDialog::eventFilter(obj, event);
}

void AutoTypeSelectDialog::updateActionMenu(AutoTypeMatch match)
{
    if (!match.first) {
        m_ui->action->setEnabled(false);
        return;
    }

    m_ui->action->setEnabled(true);

    bool hasUsername = !match.first->username().isEmpty();
    bool hasPassword = !match.first->password().isEmpty();
    bool hasTotp = match.first->hasTotp();

    auto actions = m_actionMenu->actions();
    Q_ASSERT(actions.size() >= 6);
    actions[0]->setEnabled(hasUsername);
    actions[1]->setEnabled(hasPassword);
    actions[2]->setEnabled(hasTotp);
    actions[3]->setEnabled(hasUsername);
    actions[4]->setEnabled(hasPassword);
    actions[5]->setEnabled(hasTotp);
}

void AutoTypeSelectDialog::buildActionMenu()
{
    m_actionMenu = new QMenu(this);
    auto typeUsernameAction = new QAction(icons()->icon("auto-type"), tr("Type {USERNAME}"), this);
    auto typePasswordAction = new QAction(icons()->icon("auto-type"), tr("Type {PASSWORD}"), this);
    auto typeTotpAction = new QAction(icons()->icon("auto-type"), tr("Type {TOTP}"), this);
    auto copyUsernameAction = new QAction(icons()->icon("username-copy"), tr("Copy Username"), this);
    auto copyPasswordAction = new QAction(icons()->icon("password-copy"), tr("Copy Password"), this);
    auto copyTotpAction = new QAction(icons()->icon("chronometer"), tr("Copy TOTP"), this);
    m_actionMenu->addAction(typeUsernameAction);
    m_actionMenu->addAction(typePasswordAction);
    m_actionMenu->addAction(typeTotpAction);
    m_actionMenu->addAction(copyUsernameAction);
    m_actionMenu->addAction(copyPasswordAction);
    m_actionMenu->addAction(copyTotpAction);

    connect(typeUsernameAction, &QAction::triggered, this, [&] {
        auto match = m_ui->view->currentMatch();
        match.second = QStringLiteral("{USERNAME}");
        submitAutoTypeMatch(match);
    });
    connect(typePasswordAction, &QAction::triggered, this, [&] {
        auto match = m_ui->view->currentMatch();
        match.second = QStringLiteral("{PASSWORD}");
        submitAutoTypeMatch(match);
    });
    connect(typeTotpAction, &QAction::triggered, this, [&] {
        auto match = m_ui->view->currentMatch();
        match.second = QStringLiteral("{TOTP}");
        submitAutoTypeMatch(match);
    });

    connect(copyUsernameAction, &QAction::triggered, this, [&] {
        clipboard()->setText(m_ui->view->currentMatch().first->username());
        reject();
    });
    connect(copyPasswordAction, &QAction::triggered, this, [&] {
        clipboard()->setText(m_ui->view->currentMatch().first->password());
        reject();
    });
    connect(copyTotpAction, &QAction::triggered, this, [&] {
        clipboard()->setText(m_ui->view->currentMatch().first->totp());
        reject();
    });
}

void AutoTypeSelectDialog::closeEvent(QCloseEvent* event)
{
    config()->set(Config::GUI_AutoTypeSelectDialogSize, size());
    if (!m_accepted) {
        emit rejected();
    }
    QDialog::closeEvent(event);
}
