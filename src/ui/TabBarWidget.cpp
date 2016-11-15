/**************************************************************************
* Otter Browser: Web browser controlled by the user, not vice-versa.
* Copyright (C) 2013 - 2016 Michal Dutkiewicz aka Emdek <michal@emdek.pl>
* Copyright (C) 2014 Piotr Wójcik <chocimier@tlen.pl>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
**************************************************************************/

#include "TabBarWidget.h"
#include "ContentsWidget.h"
#include "MainWindow.h"
#include "PreviewWidget.h"
#include "ToolBarWidget.h"
#include "Window.h"
#include "../core/ActionsManager.h"
#include "../core/GesturesManager.h"
#include "../core/SettingsManager.h"
#include "../core/ThemesManager.h"

#include <QtCore/QMimeData>
#include <QtCore/QtMath>
#include <QtCore/QTimer>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QMovie>
#include <QtGui/QPainter>
#include <QtGui/QStatusTipEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDesktopWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOption>
#include <QtWidgets/QToolTip>

namespace Otter
{

TabDrag::TabDrag(quint64 window, QObject *parent) : QDrag(parent),
	m_window(window),
	m_releaseTimer(0)
{
	m_releaseTimer = startTimer(250);
}

TabDrag::~TabDrag()
{
	if (!target())
	{
		QVariantMap parameters;
		parameters[QLatin1String("window")] = m_window;

		ActionsManager::triggerAction(ActionsManager::DetachTabAction, this, parameters);
	}
}

void TabDrag::timerEvent(QTimerEvent *event)
{
	if (event->timerId() == m_releaseTimer && QGuiApplication::mouseButtons() == Qt::NoButton)
	{
		deleteLater();
	}

	QDrag::timerEvent(event);
}

TabBarStyle::TabBarStyle(QStyle *style) : QProxyStyle(style)
{
}

void TabBarStyle::drawControl(QStyle::ControlElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget) const
{
	if (element == CE_TabBarTabLabel)
	{
		const QStyleOptionTab *tabOption(qstyleoption_cast<const QStyleOptionTab*>(option));

		if (tabOption)
		{
			QStyleOptionTab mutableTabOption(*tabOption);
			mutableTabOption.shape = QTabBar::RoundedNorth;

			QProxyStyle::drawControl(element, &mutableTabOption, painter, widget);

			return;
		}
	}

	QProxyStyle::drawControl(element, option, painter, widget);
}

QSize TabBarStyle::sizeFromContents(QStyle::ContentsType type, const QStyleOption *option, const QSize &size, const QWidget *widget) const
{
	QSize mutableSize(QProxyStyle::sizeFromContents(type, option, size, widget));
	const QStyleOptionTab *tabOption(qstyleoption_cast<const QStyleOptionTab*>(option));

	if (type == QStyle::CT_TabBarTab && tabOption && (tabOption->shape == QTabBar::RoundedEast || tabOption->shape == QTabBar::RoundedWest))
	{
		mutableSize.transpose();
	}

	return mutableSize;
}

QRect TabBarStyle::subElementRect(QStyle::SubElement element, const QStyleOption *option, const QWidget *widget) const
{
	if (element == QStyle::SE_TabBarTabLeftButton || element == QStyle::SE_TabBarTabRightButton || element == QStyle::SE_TabBarTabText)
	{
		const QStyleOptionTab *tabOption(qstyleoption_cast<const QStyleOptionTab*>(option));

		if (tabOption->shape == QTabBar::RoundedEast || tabOption->shape == QTabBar::RoundedWest)
		{
			QStyleOptionTab mutableTabOption(*tabOption);
			mutableTabOption.shape = QTabBar::RoundedNorth;

			QRect rectangle(QProxyStyle::subElementRect(element, &mutableTabOption, widget));
			rectangle.translate(0, option->rect.top());

			return rectangle;
		}
	}

	return QProxyStyle::subElementRect(element, option, widget);
}

TabBarWidget::TabBarWidget(QWidget *parent) : QTabBar(parent),
	m_previewWidget(nullptr),
	m_closeButtonPosition(static_cast<QTabBar::ButtonPosition>(QApplication::style()->styleHint(QStyle::SH_TabBar_CloseButtonPosition))),
	m_iconButtonPosition(((m_closeButtonPosition == QTabBar::RightSide) ? QTabBar::LeftSide : QTabBar::RightSide)),
	m_tabSize(0),
	m_maximumTabSize(40),
	m_minimumTabSize(250),
	m_pinnedTabsAmount(0),
	m_clickedTab(-1),
	m_hoveredTab(-1),
	m_previewTimer(0),
	m_showCloseButton(SettingsManager::getValue(SettingsManager::TabBar_ShowCloseButtonOption).toBool()),
	m_showUrlIcon(SettingsManager::getValue(SettingsManager::TabBar_ShowUrlIconOption).toBool()),
	m_enablePreviews(SettingsManager::getValue(SettingsManager::TabBar_EnablePreviewsOption).toBool()),
	m_isDraggingTab(false),
	m_isDetachingTab(false),
	m_isIgnoringTabDrag(false)
{
	setAcceptDrops(true);
	setDrawBase(false);
	setExpanding(false);
	setMovable(true);
	setSelectionBehaviorOnRemove(QTabBar::SelectPreviousTab);
	setElideMode(Qt::ElideRight);
	setMouseTracking(true);
	setDocumentMode(true);
	setMaximumSize(0, 0);
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	setStyle(new TabBarStyle());
	optionChanged(SettingsManager::TabBar_MaximumTabSizeOption, SettingsManager::getValue(SettingsManager::TabBar_MaximumTabSizeOption));
	optionChanged(SettingsManager::TabBar_MinimumTabSizeOption, SettingsManager::getValue(SettingsManager::TabBar_MinimumTabSizeOption));

	ToolBarWidget *toolBar(qobject_cast<ToolBarWidget*>(parent));

	if (toolBar)
	{
		setArea(toolBar->getArea());

		connect(toolBar, SIGNAL(areaChanged(Qt::ToolBarArea)), this, SLOT(setArea(Qt::ToolBarArea)));
	}

	connect(SettingsManager::getInstance(), SIGNAL(valueChanged(int,QVariant)), this, SLOT(optionChanged(int,QVariant)));
	connect(this, SIGNAL(currentChanged(int)), this, SLOT(currentTabChanged(int)));
}

void TabBarWidget::timerEvent(QTimerEvent *event)
{
	if (event->timerId() == m_previewTimer)
	{
		killTimer(m_previewTimer);

		m_previewTimer = 0;

		showPreview(tabAt(mapFromGlobal(QCursor::pos())));
	}
}

void TabBarWidget::resizeEvent(QResizeEvent *event)
{
	QTabBar::resizeEvent(event);

	QTimer::singleShot(100, this, SLOT(updateTabs()));
}

void TabBarWidget::paintEvent(QPaintEvent *event)
{
	QTabBar::paintEvent(event);

	if (!m_dragMovePosition.isNull())
	{
		const int dropIndex(getDropIndex());

		if (dropIndex >= 0)
		{
			const bool isHorizontal(shape() == QTabBar::RoundedNorth || shape() == QTabBar::RoundedSouth);
			int lineOffset(0);

			if (count() == 0)
			{
				lineOffset = 0;
			}
			else if (dropIndex >= count())
			{
				lineOffset = tabRect(count() - 1).right();
			}
			else
			{
				lineOffset = tabRect(dropIndex).left();
			}

			QPainter painter(this);
			painter.setPen(QPen(palette().text(), 3, Qt::DotLine));

			if (isHorizontal)
			{
				painter.drawLine(lineOffset, 0, lineOffset, height());
			}
			else
			{
				painter.drawLine(0, lineOffset, width(), lineOffset);
			}

			painter.setPen(QPen(palette().text(), 9, Qt::SolidLine, Qt::RoundCap));

			if (isHorizontal)
			{
				painter.drawPoint(lineOffset, 0);
				painter.drawPoint(lineOffset, height());
			}
			else
			{
				painter.drawPoint(0, lineOffset);
				painter.drawPoint(width(), lineOffset);
			}
		}
	}
}

void TabBarWidget::enterEvent(QEvent *event)
{
	QTabBar::enterEvent(event);

	m_previewTimer = startTimer(250);
}

void TabBarWidget::leaveEvent(QEvent *event)
{
	QTabBar::leaveEvent(event);

	hidePreview();

	m_tabSize = 0;
	m_hoveredTab = -1;

	QStatusTipEvent statusTipEvent((QString()));

	QApplication::sendEvent(this, &statusTipEvent);

	updateGeometry();
	adjustSize();
}

void TabBarWidget::contextMenuEvent(QContextMenuEvent *event)
{
	if (event->reason() == QContextMenuEvent::Mouse)
	{
		event->accept();

		return;
	}

	m_clickedTab = tabAt(event->pos());

	hidePreview();

	MainWindow *mainWindow(MainWindow::findMainWindow(this));
	QVariantMap parameters;
	QMenu menu(this);
	menu.addAction(ActionsManager::getAction(ActionsManager::NewTabAction, this));
	menu.addAction(ActionsManager::getAction(ActionsManager::NewTabPrivateAction, this));

	if (m_clickedTab >= 0)
	{
		Window *window(getWindow(m_clickedTab));

		if (window)
		{
			parameters[QLatin1String("window")] = window->getIdentifier();

			const int amount(count() - getPinnedTabsAmount());
			const bool isPinned(window->isPinned());
			Action *cloneTabAction(new Action(ActionsManager::CloneTabAction, &menu));
			cloneTabAction->setEnabled(window->canClone());
			cloneTabAction->setData(parameters);

			Action *pinTabAction(new Action(ActionsManager::PinTabAction, &menu));
			pinTabAction->setOverrideText(isPinned ? QT_TRANSLATE_NOOP("actions", "Unpin Tab") : QT_TRANSLATE_NOOP("actions", "Pin Tab"));
			pinTabAction->setData(parameters);

			Action *detachTabAction(new Action(ActionsManager::DetachTabAction, &menu));
			detachTabAction->setEnabled(count() > 1);
			detachTabAction->setData(parameters);

			Action *closeTabAction(new Action(ActionsManager::CloseTabAction, &menu));
			closeTabAction->setEnabled(!isPinned);
			closeTabAction->setData(parameters);

			Action *closeOtherTabsAction(new Action(ActionsManager::CloseOtherTabsAction, &menu));
			closeOtherTabsAction->setEnabled(amount > 0 && !(amount == 1 && !isPinned));
			closeOtherTabsAction->setData(parameters);

			menu.addAction(cloneTabAction);
			menu.addAction(pinTabAction);
			menu.addAction(window ? window->getContentsWidget()->getAction(ActionsManager::MuteTabMediaAction) : new Action(ActionsManager::MuteTabMediaAction, &menu));
			menu.addSeparator();
			menu.addAction(detachTabAction);
			menu.addSeparator();
			menu.addAction(closeTabAction);
			menu.addAction(closeOtherTabsAction);
			menu.addAction(ActionsManager::getAction(ActionsManager::ClosePrivateTabsAction, this));

			connect(cloneTabAction, SIGNAL(triggered()), mainWindow, SLOT(triggerAction()));
			connect(pinTabAction, SIGNAL(triggered()), mainWindow, SLOT(triggerAction()));
			connect(detachTabAction, SIGNAL(triggered()), mainWindow, SLOT(triggerAction()));
			connect(closeTabAction, SIGNAL(triggered()), mainWindow, SLOT(triggerAction()));
			connect(closeOtherTabsAction, SIGNAL(triggered()), mainWindow, SLOT(triggerAction()));
		}
	}

	menu.addSeparator();

	QMenu *arrangeMenu(menu.addMenu(tr("Arrange")));
	Action *restoreTabAction(new Action(ActionsManager::RestoreTabAction, &menu));
	restoreTabAction->setEnabled(m_clickedTab >= 0);
	restoreTabAction->setData(parameters);

	Action *minimizeTabAction(new Action(ActionsManager::MinimizeTabAction, &menu));
	minimizeTabAction->setEnabled(m_clickedTab >= 0);
	minimizeTabAction->setData(parameters);

	Action *maximizeTabAction(new Action(ActionsManager::MaximizeTabAction, &menu));
	maximizeTabAction->setEnabled(m_clickedTab >= 0);
	maximizeTabAction->setData(parameters);

	arrangeMenu->addAction(restoreTabAction);
	arrangeMenu->addAction(minimizeTabAction);
	arrangeMenu->addAction(maximizeTabAction);
	arrangeMenu->addSeparator();
	arrangeMenu->addAction(ActionsManager::getAction(ActionsManager::RestoreAllAction, this));
	arrangeMenu->addAction(ActionsManager::getAction(ActionsManager::MaximizeAllAction, this));
	arrangeMenu->addAction(ActionsManager::getAction(ActionsManager::MinimizeAllAction, this));
	arrangeMenu->addSeparator();
	arrangeMenu->addAction(ActionsManager::getAction(ActionsManager::CascadeAllAction, this));
	arrangeMenu->addAction(ActionsManager::getAction(ActionsManager::TileAllAction, this));

	QAction *cycleAction(new QAction(tr("Switch Tabs Using the Mouse Wheel"), this));
	cycleAction->setCheckable(true);
	cycleAction->setChecked(!SettingsManager::getValue(SettingsManager::TabBar_RequireModifierToSwitchTabOnScrollOption).toBool());

	connect(cycleAction, SIGNAL(toggled(bool)), this, SLOT(setCycle(bool)));
	connect(restoreTabAction, SIGNAL(triggered()), mainWindow, SLOT(triggerAction()));
	connect(minimizeTabAction, SIGNAL(triggered()), mainWindow, SLOT(triggerAction()));
	connect(maximizeTabAction, SIGNAL(triggered()), mainWindow, SLOT(triggerAction()));

	ToolBarWidget *toolBar(qobject_cast<ToolBarWidget*>(parentWidget()));

	if (toolBar)
	{
		QList<QAction*> actions;
		actions.append(cycleAction);

		menu.addMenu(ToolBarWidget::createCustomizationMenu(ToolBarsManager::TabBar, actions, &menu));
	}
	else
	{
		QMenu *customizationMenu(menu.addMenu(tr("Customize")));
		customizationMenu->addAction(cycleAction);
		customizationMenu->addSeparator();
		customizationMenu->addAction(ActionsManager::getAction(ActionsManager::LockToolBarsAction, this));
	}

	menu.exec(event->globalPos());

	cycleAction->deleteLater();

	m_clickedTab = -1;

	if (underMouse())
	{
		m_previewTimer = startTimer(250);
	}
}

void TabBarWidget::mousePressEvent(QMouseEvent *event)
{
	QTabBar::mousePressEvent(event);

	if (event->button() == Qt::LeftButton)
	{
		Window *window(getWindow(tabAt(event->pos())));

		m_isIgnoringTabDrag = (count() == 1 || (window && window->isPinned() && m_pinnedTabsAmount == 1));

		if (window)
		{
			m_dragStartPosition = event->pos();
			m_draggedWindow = window->getIdentifier();
		}
	}

	hidePreview();
}

void TabBarWidget::mouseMoveEvent(QMouseEvent *event)
{
	tabHovered(tabAt(event->pos()));

	if (!m_isDraggingTab && !m_dragStartPosition.isNull())
	{
		m_isDraggingTab = ((event->pos() - m_dragStartPosition).manhattanLength() > QApplication::startDragDistance());
	}

	if (m_isDraggingTab && !rect().adjusted(-10, -10, 10, 10).contains(event->pos()))
	{
		m_isDraggingTab = false;

		QMouseEvent mouseEvent(QEvent::MouseButtonRelease, event->pos(), Qt::LeftButton, Qt::LeftButton, event->modifiers());

		QApplication::sendEvent(this, &mouseEvent);

		m_isDetachingTab = true;

		updateGeometry();
		adjustSize();

		MainWindow *mainWindow(MainWindow::findMainWindow(this));

		if (mainWindow)
		{
			Window *window(mainWindow->getWindowsManager()->getWindowByIdentifier(m_draggedWindow));

			if (window)
			{
				QDrag *drag(new TabDrag(window->getIdentifier(), this));

				connect(drag, &QDrag::destroyed, [&]()
				{
					m_isDetachingTab = false;
				});

				QMimeData *mimeData(new QMimeData());
				mimeData->setText(window->getUrl().toString());
				mimeData->setUrls(QList<QUrl>({window->getUrl()}));
				mimeData->setProperty("x-url-title", window->getTitle());
				mimeData->setProperty("x-window-identifier", window->getIdentifier());

				const QPixmap thumbnail(window->getThumbnail());

				drag->setMimeData(mimeData);
				drag->setPixmap(thumbnail.isNull() ? window->getIcon().pixmap(16, 16) : thumbnail);
				drag->exec(Qt::CopyAction | Qt::MoveAction);
			}
		}

		return;
	}

	if (m_isIgnoringTabDrag || m_isDetachingTab)
	{
		return;
	}

	QTabBar::mouseMoveEvent(event);
}

void TabBarWidget::mouseReleaseEvent(QMouseEvent *event)
{
	QTabBar::mouseReleaseEvent(event);

	if (event->button() == Qt::LeftButton)
	{
		if (m_isDetachingTab)
		{
			QVariantMap parameters;
			parameters[QLatin1String("window")] = m_draggedWindow;

			ActionsManager::triggerAction(ActionsManager::DetachTabAction, this, parameters);

			m_isDetachingTab = false;
		}

		m_dragStartPosition = QPoint();
		m_isDraggingTab = false;
	}
}

void TabBarWidget::wheelEvent(QWheelEvent *event)
{
	QWidget::wheelEvent(event);

	if (!(event->modifiers().testFlag(Qt::ControlModifier)) && SettingsManager::getValue(SettingsManager::TabBar_RequireModifierToSwitchTabOnScrollOption).toBool())
	{
		return;
	}

	if (event->delta() > 0)
	{
		activateTabOnLeft();
	}
	else
	{
		activateTabOnRight();
	}
}

void TabBarWidget::dragEnterEvent(QDragEnterEvent *event)
{
	if (event->mimeData()->hasUrls() || (event->source() && !event->mimeData()->property("x-window-identifier").isNull()))
	{
		event->accept();

		m_dragMovePosition = event->pos();

		update();
	}
}

void TabBarWidget::dragMoveEvent(QDragMoveEvent *event)
{
	m_dragMovePosition = event->pos();

	update();
}

void TabBarWidget::dragLeaveEvent(QDragLeaveEvent *event)
{
	Q_UNUSED(event)

	m_dragMovePosition = QPoint();

	update();
}

void TabBarWidget::dropEvent(QDropEvent *event)
{
	const int dropIndex(getDropIndex());

	if (event->source() && !event->mimeData()->property("x-window-identifier").isNull())
	{
		event->setDropAction(Qt::MoveAction);
		event->accept();

		int previousIndex(-1);
		const quint64 windowIdentifier(event->mimeData()->property("x-window-identifier").toULongLong());

		if (event->source() == this)
		{
			for (int i = 0; i < count(); ++i)
			{
				Window *window(getWindow(i));

				if (window && window->getIdentifier() == windowIdentifier)
				{
					previousIndex = i;

					break;
				}
			}
		}

		if (previousIndex < 0)
		{
			MainWindow *mainWindow(MainWindow::findMainWindow(this));

			if (mainWindow)
			{
				const QList<MainWindow*> mainWindows(SessionsManager::getWindows());

				for (int i = 0; i < mainWindows.count(); ++i)
				{
					if (mainWindows.at(i))
					{
						Window *window(mainWindows.at(i)->getWindowsManager()->getWindowByIdentifier(windowIdentifier));

						if (window)
						{
							mainWindows.at(i)->getWindowsManager()->moveWindow(window, mainWindow, dropIndex);

							break;
						}
					}
				}
			}
		}
		else if (previousIndex != dropIndex && (previousIndex + 1) != dropIndex)
		{
			moveTab(previousIndex, (dropIndex - ((dropIndex > previousIndex) ? 1 : 0)));
		}
	}
	else if (event->mimeData()->hasUrls())
	{
		MainWindow *mainWindow(MainWindow::findMainWindow(this));
		bool canOpen(mainWindow != nullptr);

		if (canOpen)
		{
			const QList<QUrl> urls(event->mimeData()->urls());

			if (urls.count() > 1 && SettingsManager::getValue(SettingsManager::Choices_WarnOpenMultipleDroppedUrlsOption).toBool())
			{
				QMessageBox messageBox;
				messageBox.setWindowTitle(tr("Question"));
				messageBox.setText(tr("You are about to open %n URL(s).", "", urls.count()));
				messageBox.setInformativeText(tr("Do you want to continue?"));
				messageBox.setIcon(QMessageBox::Question);
				messageBox.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
				messageBox.setDefaultButton(QMessageBox::Yes);
				messageBox.setCheckBox(new QCheckBox(tr("Do not show this message again")));

				if (messageBox.exec() == QMessageBox::Cancel)
				{
					canOpen = false;
				}

				SettingsManager::setValue(SettingsManager::Choices_WarnOpenMultipleDroppedUrlsOption, !messageBox.checkBox()->isChecked());
			}

			if (canOpen)
			{
				for (int i = 0; i < urls.count(); ++i)
				{
					mainWindow->getWindowsManager()->open(urls.at(i), WindowsManager::DefaultOpen, (dropIndex + i));
				}
			}
		}

		if (canOpen)
		{
			event->setDropAction(Qt::CopyAction);
			event->accept();
		}
		else
		{
			event->ignore();
		}
	}
	else
	{
		event->ignore();
	}

	m_dragMovePosition = QPoint();

	update();
}

void TabBarWidget::tabLayoutChange()
{
	QTabBar::tabLayoutChange();

	updateButtons();

	emit layoutChanged();
}

void TabBarWidget::tabInserted(int index)
{
	setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);

	QTabBar::tabInserted(index);

	if (m_showUrlIcon)
	{
		QLabel *label(new QLabel());
		label->setFixedSize(QSize(16, 16));

		setTabButton(index, m_iconButtonPosition, label);
	}
	else
	{
		setTabButton(index, m_iconButtonPosition, nullptr);
	}

	Window *window(getWindow(index));

	if (m_showCloseButton || (window && window->isPinned()))
	{
		QLabel *label(new QLabel());
		label->setFixedSize(QSize(16, 16));
		label->installEventFilter(this);

		setTabButton(index, m_closeButtonPosition, label);
	}

	updateTabs();

	emit tabsAmountChanged(count());
}

void TabBarWidget::tabRemoved(int index)
{
	QTabBar::tabRemoved(index);

	if (count() == 0)
	{
		setMaximumSize(0, 0);
	}
	else
	{
		QTimer::singleShot(100, this, SLOT(updateTabs()));
	}

	emit tabsAmountChanged(count());
}

void TabBarWidget::tabHovered(int index)
{
	if (index == m_hoveredTab)
	{
		return;
	}

	m_hoveredTab = index;

	if (m_previewWidget && !m_previewWidget->isVisible() && m_previewTimer == 0)
	{
		m_previewWidget->show();
	}

	Window *window(getWindow(index));
	QStatusTipEvent statusTipEvent(window ? window->getUrl().toDisplayString() : QString());

	QApplication::sendEvent(this, &statusTipEvent);

	if (m_previewWidget && m_previewWidget->isVisible())
	{
		showPreview(index);
	}
}

void TabBarWidget::addTab(int index, Window *window)
{
	insertTab(index, window->getTitle());
	setTabData(index, window->getIdentifier());

	connect(window, SIGNAL(iconChanged(QIcon)), this, SLOT(updateTabs()));
	connect(window, SIGNAL(loadingStateChanged(WindowsManager::LoadingState)), this, SLOT(updateTabs()));
	connect(window, SIGNAL(updatePinnedTabsAmount(bool)), this, SLOT(updatePinnedTabsAmount()));

	if (window->isPinned())
	{
		updatePinnedTabsAmount(window);
	}

	updateTabs(index);
}

void TabBarWidget::removeTab(int index)
{
	if (underMouse())
	{
		const QSize size(tabSizeHint(count() - 1));

		m_tabSize = size.width();
	}

	Window *window(getWindow(index));

	if (window)
	{
		window->deleteLater();
	}

	QTabBar::removeTab(index);

	if (window && window->isPinned())
	{
		updatePinnedTabsAmount();
		updateGeometry();
		adjustSize();
	}

	if (underMouse() && tabAt(mapFromGlobal(QCursor::pos())) < 0)
	{
		m_tabSize = 0;

		updateGeometry();
		adjustSize();
	}
}

void TabBarWidget::activateTabOnLeft()
{
	setCurrentIndex((currentIndex() > 0) ? (currentIndex() - 1) : (count() - 1));
}

void TabBarWidget::activateTabOnRight()
{
	setCurrentIndex((currentIndex() + 1 < count()) ? (currentIndex() + 1) : 0);
}

void TabBarWidget::showPreview(int index)
{
	if (!m_enablePreviews || !isActiveWindow())
	{
		hidePreview();

		return;
	}

	Window *window(getWindow(index));

	if (window && m_clickedTab < 0)
	{
		if (!m_previewWidget)
		{
			m_previewWidget = new PreviewWidget(this);
		}

		QPoint position;
		// Note that screen rectangle, tab rectangle and preview rectangle could have
		// negative values on multiple monitors systems. All calculations must be done in context
		// of a current screen rectangle. Because top left point of current screen could
		// have coordinates (-1366, 250) instead of (0, 0).
		///TODO: Calculate screen rectangle based on current mouse pointer position
		const QRect screen(QApplication::desktop()->screenGeometry(this));
		QRect rectangle(tabRect(index));
		rectangle.moveTo(mapToGlobal(rectangle.topLeft()));

		m_previewWidget->setPreview(window->getTitle(), ((index == currentIndex()) ? QPixmap() : window->getThumbnail()));

		switch (shape())
		{
			case QTabBar::RoundedEast:
				position = QPoint((rectangle.left() - m_previewWidget->width()), qMax(screen.top(), ((rectangle.bottom() - (rectangle.height() / 2)) - (m_previewWidget->height() / 2))));

				break;
			case QTabBar::RoundedWest:
				position = QPoint(rectangle.right(), qMax(screen.top(), ((rectangle.bottom() - (rectangle.height() / 2)) - (m_previewWidget->height() / 2))));

				break;
			case QTabBar::RoundedSouth:
				position = QPoint(qMax(screen.left(), ((rectangle.right() - (rectangle.width() / 2)) - (m_previewWidget->width() / 2))), (rectangle.top() - m_previewWidget->height()));

				break;
			default:
				position = QPoint(qMax(screen.left(), ((rectangle.right() - (rectangle.width() / 2)) - (m_previewWidget->width() / 2))), rectangle.bottom());

				break;
		}

		if ((position.x() + m_previewWidget->width()) > screen.right())
		{
			position.setX(screen.right() - m_previewWidget->width());
		}

		if ((position.y() + m_previewWidget->height()) > screen.bottom())
		{
			position.setY(screen.bottom() - m_previewWidget->height());
		}

		if (m_previewWidget->isVisible())
		{
			m_previewWidget->setPosition(position);
		}
		else
		{
			m_previewWidget->move(position);
			m_previewWidget->show();
		}
	}
	else if (m_previewWidget)
	{
		m_previewWidget->hide();
	}
}

void TabBarWidget::hidePreview()
{
	if (m_previewWidget)
	{
		m_previewWidget->hide();
	}

	if (m_previewTimer > 0)
	{
		killTimer(m_previewTimer);

		m_previewTimer = 0;
	}
}

void TabBarWidget::optionChanged(int identifier, const QVariant &value)
{
	if (identifier == SettingsManager::TabBar_ShowCloseButtonOption)
	{
		const bool showCloseButton(value.toBool());

		if (showCloseButton != m_showCloseButton)
		{
			for (int i = 0; i < count(); ++i)
			{
				if (showCloseButton)
				{
					Window *window(getWindow(i));
					QLabel *label(new QLabel());
					label->setFixedSize(QSize(16, 16));

					if (window)
					{
						label->setBuddy(window);
					}

					setTabButton(i, m_closeButtonPosition, label);
				}
				else
				{
					setTabButton(i, m_closeButtonPosition, nullptr);
				}
			}

			updateTabs();
		}

		m_showCloseButton = showCloseButton;
	}
	else if (identifier == SettingsManager::TabBar_ShowUrlIconOption)
	{
		const bool showUrlIcon(value.toBool());

		if (showUrlIcon != m_showUrlIcon)
		{
			for (int i = 0; i < count(); ++i)
			{
				if (showUrlIcon)
				{
					QLabel *label(new QLabel());
					label->setFixedSize(QSize(16, 16));

					setTabButton(i, m_iconButtonPosition, label);
				}
				else
				{
					setTabButton(i, m_iconButtonPosition, nullptr);
				}
			}

			updateTabs();
		}

		m_showUrlIcon = showUrlIcon;
	}
	else if (identifier == SettingsManager::TabBar_EnablePreviewsOption)
	{
		m_enablePreviews = value.toBool();
	}
	else if (identifier == SettingsManager::TabBar_MaximumTabSizeOption)
	{
		const int oldValue(m_maximumTabSize);

		m_maximumTabSize = value.toInt();

		if (m_maximumTabSize < 0)
		{
			m_maximumTabSize = 250;
		}

		if (m_maximumTabSize != oldValue)
		{
			updateGeometry();
			updateTabs();
		}
	}
	else if (identifier == SettingsManager::TabBar_MinimumTabSizeOption && value.toInt() != m_minimumTabSize)
	{
		const int oldValue(m_minimumTabSize);

		m_minimumTabSize = value.toInt();

		if (m_minimumTabSize < 0)
		{
			m_minimumTabSize = 40;
		}

		if (m_minimumTabSize != oldValue)
		{
			updateGeometry();
			updateTabs();
		}
	}
}

void TabBarWidget::currentTabChanged(int index)
{
	Q_UNUSED(index)

	if (m_previewWidget && m_previewWidget->isVisible())
	{
		showPreview(tabAt(mapFromGlobal(QCursor::pos())));
	}

	if (m_showCloseButton)
	{
		updateButtons();
	}
}

void TabBarWidget::updatePinnedTabsAmount(Window *modifiedWindow)
{
	int amount(0);

	for (int i = 0; i < count(); ++i)
	{
		Window *window(getWindow(i));

		if (window && window->isPinned())
		{
			++amount;
		}
	}

	m_pinnedTabsAmount = amount;

	if (!modifiedWindow)
	{
		modifiedWindow = qobject_cast<Window*>(sender());
	}

	if (modifiedWindow)
	{
		int index(-1);

		for (int i = 0; i < count(); ++i)
		{
			if (tabData(i).toULongLong() == modifiedWindow->getIdentifier())
			{
				index = i;

				break;
			}
		}

		if (index >= 0)
		{
			moveTab(index, (modifiedWindow->isPinned() ? qMax(0, (m_pinnedTabsAmount - 1)) : m_pinnedTabsAmount));
			updateButtons();
			updateGeometry();
			adjustSize();
		}
	}

	updateTabs();
}

void TabBarWidget::updateButtons()
{
	const QSize size(tabSizeHint(count() - 1));
	const bool isVertical(shape() == QTabBar::RoundedWest || shape() == QTabBar::RoundedEast);
	const bool isNarrow(size.width() < 60);

	for (int i = 0; i < count(); ++i)
	{
		Window *window(getWindow(i));
		QLabel *closeLabel(qobject_cast<QLabel*>(tabButton(i, m_closeButtonPosition)));
		QLabel *iconLabel(qobject_cast<QLabel*>(tabButton(i, m_iconButtonPosition)));
		const bool isCurrent(i == currentIndex());
		const bool isPinned(window ? window->isPinned() : false);

		if (iconLabel)
		{
			iconLabel->setVisible(isPinned || !isCurrent || !isNarrow);
		}

		if (!closeLabel)
		{
			continue;
		}

		const bool wasPinned(closeLabel->property("isPinned").toBool());

		if (!closeLabel->buddy())
		{
			Window *window(getWindow(i));

			if (window)
			{
				closeLabel->setBuddy(window);
			}
		}

		if (isPinned != wasPinned || !closeLabel->pixmap())
		{
			closeLabel->setProperty("isPinned", isPinned);

			if (isPinned)
			{
				closeLabel->setPixmap(ThemesManager::getIcon(QLatin1String("object-locked")).pixmap(16, 16));
			}
			else
			{
				QStyleOption option;
				option.rect = QRect(0, 0, 16, 16);

				QPixmap pixmap(QSize(16, 16) * devicePixelRatio());
				pixmap.setDevicePixelRatio(devicePixelRatio());
				pixmap.fill(Qt::transparent);

				QPainter painter(&pixmap);

				style()->drawPrimitive(QStyle::PE_IndicatorTabClose, &option, &painter, this);

				closeLabel->setPixmap(pixmap);
			}
		}

		closeLabel->setVisible((isPinned && isVertical) || ((isCurrent || !isNarrow) && !isPinned));
	}
}

void TabBarWidget::updateTabs(int index)
{
	if (index < 0 && sender() && sender()->inherits(QStringLiteral("Otter::Window").toLatin1()))
	{
		for (int i = 0; i < count(); ++i)
		{
			if (sender() == getWindow(i))
			{
				index = i;

				break;
			}
		}
	}

	const int limit((index >= 0) ? (index + 1) : count());

	for (int i = ((index >= 0) ? index : 0); i < limit; ++i)
	{
		Window *window(getWindow(i));
		const WindowsManager::LoadingState loadingState(window ? window->getLoadingState() : WindowsManager::FinishedLoadingState);
		QLabel *label(qobject_cast<QLabel*>(tabButton(i, m_iconButtonPosition)));

		if (label)
		{
			if (loadingState == WindowsManager::DelayedLoadingState || loadingState == WindowsManager::OngoingLoadingState)
			{
				if (!label->movie())
				{
					QMovie *movie(new QMovie(QLatin1String(":/icons/loading.gif"), QByteArray(), label));
					movie->start();

					label->setMovie(movie);
				}

				label->movie()->setSpeed((loadingState == WindowsManager::OngoingLoadingState) ? 100 : 10);
			}
			else
			{
				if (label->movie())
				{
					label->movie()->deleteLater();
					label->setMovie(nullptr);
				}

				QIcon icon;

				if (loadingState == WindowsManager::CrashedLoadingState)
				{
					icon = ThemesManager::getIcon(QLatin1String("tab-crashed"));
				}
				else if (window)
				{
					icon = window->getIcon();
				}
				else
				{
					icon = ThemesManager::getIcon(QLatin1String("tab"));
				}

				label->setPixmap(icon.pixmap(16, 16));
			}
		}
	}

	m_hoveredTab = -1;

	tabHovered(tabAt(mapFromGlobal(QCursor::pos())));
}

void TabBarWidget::setCycle(bool enable)
{
	SettingsManager::setValue(SettingsManager::TabBar_RequireModifierToSwitchTabOnScrollOption, !enable);
}

void TabBarWidget::setArea(Qt::ToolBarArea area)
{
	switch (area)
	{
		case Qt::LeftToolBarArea:
			setShape(QTabBar::RoundedWest);

			break;
		case Qt::RightToolBarArea:
			setShape(QTabBar::RoundedEast);

			break;
		case Qt::BottomToolBarArea:
			setShape(QTabBar::RoundedSouth);

			break;
		default:
			setShape(QTabBar::RoundedNorth);

			break;
	}
}

void TabBarWidget::setShape(QTabBar::Shape shape)
{
	QTabBar::setShape(shape);

	QTimer::singleShot(100, this, SLOT(updateTabs()));
}

Window* TabBarWidget::getWindow(int index) const
{
	if (index < 0 || index >= count())
	{
		return nullptr;
	}

	MainWindow *mainWindow(MainWindow::findMainWindow(parentWidget()));

	if (mainWindow)
	{
		return mainWindow->getWindowsManager()->getWindowByIdentifier(tabData(index).toULongLong());
	}

	return nullptr;
}

QSize TabBarWidget::tabSizeHint(int index) const
{
	if (shape() == QTabBar::RoundedNorth || shape() == QTabBar::RoundedSouth)
	{
		Window *window(getWindow(index));

		if (window && window->isPinned())
		{
			return QSize(m_minimumTabSize, QTabBar::tabSizeHint(0).height());
		}

		const int amount(getPinnedTabsAmount());

		return QSize(((m_tabSize > 0) ? m_tabSize : qBound(m_minimumTabSize, qFloor((geometry().width() - (amount * m_minimumTabSize)) / qMax(1, (count() - amount))), m_maximumTabSize)), QTabBar::tabSizeHint(0).height());
	}

	return QSize(m_maximumTabSize, QTabBar::tabSizeHint(0).height());
}

QSize TabBarWidget::minimumSizeHint() const
{
	return QSize(0, 0);
}

QSize TabBarWidget::sizeHint() const
{
	if (shape() == QTabBar::RoundedNorth || shape() == QTabBar::RoundedSouth)
	{
		int size(0);

		for (int i = 0; i < count(); ++i)
		{
			Window *window(getWindow(i));

			size += ((window && window->isPinned()) ? m_minimumTabSize : m_maximumTabSize);
		}

		return QSize(size, QTabBar::sizeHint().height());
	}

	return QSize(QTabBar::sizeHint().width(), (tabSizeHint(0).height() * count()));
}

int TabBarWidget::getDropIndex() const
{
	if (m_dragMovePosition.isNull())
	{
		return ((count() > 0) ? (count() + 1) : 0);
	}

	int index(tabAt(m_dragMovePosition));
	const bool isHorizontal((shape() == QTabBar::RoundedNorth || shape() == QTabBar::RoundedSouth));

	if (index >= 0)
	{
		const QPoint tabCenter(tabRect(index).center());

		if ((isHorizontal && m_dragMovePosition.x() > tabCenter.x()) || (!isHorizontal && m_dragMovePosition.y() > tabCenter.y()))
		{
			++index;
		}
	}
	else
	{
		index = (((isHorizontal && m_dragMovePosition.x() < rect().left()) || (!isHorizontal && m_dragMovePosition.y() < rect().top())) ? count() : 0);
	}

	return index;
}

int TabBarWidget::getPinnedTabsAmount() const
{
	return m_pinnedTabsAmount;
}

bool TabBarWidget::event(QEvent *event)
{
	if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick || event->type() == QEvent::Wheel)
	{
		QVariantMap parameters;
		int tab(-1);

		if (event->type() == QEvent::Wheel)
		{
			QWheelEvent *wheelEvent(dynamic_cast<QWheelEvent*>(event));

			if (wheelEvent)
			{
				tab = tabAt(wheelEvent->pos());
			}
		}
		else
		{
			QMouseEvent *mouseEvent(dynamic_cast<QMouseEvent*>(event));

			if (mouseEvent)
			{
				tab = tabAt(mouseEvent->pos());
			}
		}

		if (tab >= 0)
		{
			Window *window(getWindow(tab));

			if (window)
			{
				parameters[QLatin1String("window")] = window->getIdentifier();
			}
		}

		QList<GesturesManager::GesturesContext> contexts;

		if (tab < 0)
		{
			contexts.append(GesturesManager::NoTabHandleGesturesContext);
		}
		else if (tab == currentIndex())
		{
			contexts.append(GesturesManager::ActiveTabHandleGesturesContext);
			contexts.append(GesturesManager::TabHandleGesturesContext);
		}
		else
		{
			contexts.append(GesturesManager::TabHandleGesturesContext);
		}

		if (qobject_cast<ToolBarWidget*>(parentWidget()))
		{
			contexts.append(GesturesManager::ToolBarGesturesContext);
		}

		contexts.append(GesturesManager::GenericGesturesContext);

		GesturesManager::startGesture(this, event, contexts, parameters);
	}

	return QTabBar::event(event);
}

bool TabBarWidget::eventFilter(QObject *object, QEvent *event)
{
	if (event->type() == QEvent::Enter && !object->property("isPinned").toBool())
	{
		hidePreview();
	}
	else if (event->type() == QEvent::Leave)
	{
		m_previewTimer = startTimer(250);
	}
	else if (event->type() == QEvent::ToolTip)
	{
		QHelpEvent *helpEvent(dynamic_cast<QHelpEvent*>(event));

		if (helpEvent && !object->property("isPinned").toBool())
		{
			const QVector<QKeySequence> shortcuts(ActionsManager::getActionDefinition(ActionsManager::CloseTabAction).shortcuts);

			QToolTip::showText(helpEvent->globalPos(), tr("Close Tab") + (shortcuts.isEmpty() ? QString() : QLatin1String(" (") + shortcuts.at(0).toString(QKeySequence::NativeText) + QLatin1Char(')')));
		}

		return true;
	}
	else if (event->type() == QEvent::MouseButtonPress && !object->property("isPinned").toBool())
	{
		return true;
	}
	else if (event->type() == QEvent::MouseButtonRelease && !object->property("isPinned").toBool())
	{
		QMouseEvent *mouseEvent(dynamic_cast<QMouseEvent*>(event));
		QLabel *label(qobject_cast<QLabel*>(object));

		if (label && mouseEvent && mouseEvent->button() == Qt::LeftButton)
		{
			Window *window(qobject_cast<Window*>(label->buddy()));

			if (window)
			{
				window->close();

				return true;
			}
		}
	}

	return QTabBar::eventFilter(object, event);
}

}
