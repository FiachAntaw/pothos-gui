// Copyright (c) 2013-2014 Josh Blum
// SPDX-License-Identifier: BSL-1.0

#include "PothosGuiUtils.hpp" //action and object map
#include "GraphEditor/GraphDraw.hpp"
#include "GraphEditor/GraphEditor.hpp"
#include "GraphObjects/GraphBreaker.hpp"
#include "GraphObjects/GraphConnection.hpp"
#include "GraphEditor/Constants.hpp"
#include <Poco/JSON/Parser.h>
#include <QGraphicsScene>
#include <QMenu>
#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QColor>
#include <QAction>
#include <QChildEvent>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <iostream>
#include <cassert>

GraphDraw::GraphDraw(QWidget *parent):
    QGraphicsView(parent),
    _graphEditor(dynamic_cast<GraphEditor *>(parent)),
    _zoomScale(1.0),
    _mouseLeftDown(false),
    _showGraphConnectionPoints(false),
    _showGraphBoundingBoxes(false)
{
    //setup scene
    this->setScene(new QGraphicsScene(QRectF(QPointF(), GraphDrawCanvasSize), this));
    //required: BspTreeIndex is too smart for its own good, connections will not render properly
    this->scene()->setItemIndexMethod(QGraphicsScene::NoIndex);
    this->scene()->setBackgroundBrush(QColor(GraphDrawBackgroundColor));
    this->setDragMode(QGraphicsView::RubberBandDrag);

    //set high quality rendering
    this->setRenderHint(QPainter::Antialiasing);
    this->setRenderHint(QPainter::HighQualityAntialiasing);
    this->setRenderHint(QPainter::SmoothPixmapTransform);

    assert(this->getGraphEditor() != nullptr);
    this->setAcceptDrops(true);

    this->setZoomScale(1.0);

    this->setFocusPolicy(Qt::ClickFocus);

    this->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, SIGNAL(customContextMenuRequested(const QPoint &)),
        this, SLOT(handleCustomContextMenuRequested(const QPoint &)));
    connect(this, SIGNAL(stateChanged(const GraphState &)),
        parent, SLOT(handleStateChange(const GraphState &)));
    connect(this, SIGNAL(modifyProperties(GraphObject *)),
        getObjectMap()["propertiesPanel"], SLOT(handleGraphModifyProperties(GraphObject *)));
    connect(this->scene(), SIGNAL(selectionChanged(void)), this, SLOT(updateEnabledActions(void)));

    //debug view - connect and initialize
    connect(getActionMap()["showGraphConnectionPoints"], SIGNAL(triggered(void)),
        this, SLOT(handleGraphDebugViewChange(void)));
    connect(getActionMap()["showGraphBoundingBoxes"], SIGNAL(triggered(void)),
        this, SLOT(handleGraphDebugViewChange(void)));
    this->handleGraphDebugViewChange();
}

void GraphDraw::handleGraphDebugViewChange(void)
{
    _showGraphConnectionPoints = getActionMap()["showGraphConnectionPoints"]->isChecked();
    _showGraphBoundingBoxes = getActionMap()["showGraphBoundingBoxes"]->isChecked();
    if (not this->isVisible()) return;
    this->render();
}

void GraphDraw::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat("text/json/pothos_block") and
        not event->mimeData()->data("text/json/pothos_block").isEmpty())
    {
        event->acceptProposedAction();
    }
    else QGraphicsView::dragEnterEvent(event);
}

void GraphDraw::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasFormat("text/json/pothos_block") and
        not event->mimeData()->data("text/json/pothos_block").isEmpty())
    {
        event->acceptProposedAction();
    }
    else QGraphicsView::dragMoveEvent(event);
}

void GraphDraw::dropEvent(QDropEvent *event)
{
    const auto &byteArray = event->mimeData()->data("text/json/pothos_block");
    Poco::JSON::Parser p; p.parse(std::string(byteArray.constData(), byteArray.size()));
    const auto blockDesc = p.getHandler()->asVar().extract<Poco::JSON::Object::Ptr>();

    this->getGraphEditor()->handleAddBlock(blockDesc, this->mapToScene(event->pos()));
    event->acceptProposedAction();
}

void GraphDraw::setZoomScale(const qreal zoom)
{
    _zoomScale = zoom;
    this->setTransform(QTransform()); //clear
    this->scale(this->zoomScale(), this->zoomScale());
    this->render();
}

void GraphDraw::showEvent(QShowEvent *event)
{
    emit this->modifyProperties(nullptr); //resets the state of whoever is modding the properties
    this->render();
    QWidget::showEvent(event);
}

void GraphDraw::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Plus) getActionMap()["increment"]->activate(QAction::Trigger);
    if (event->key() == Qt::Key_Minus) getActionMap()["decrement"]->activate(QAction::Trigger);
    QGraphicsView::keyPressEvent(event);
}

void GraphDraw::updateEnabledActions(void)
{
    auto selectedObjsNoC = this->getObjectsSelected(~GRAPH_CONNECTION);
    const bool selectedNoC = not selectedObjsNoC.empty();

    auto selectedObjs = this->getObjectsSelected();
    const bool selected = not selectedObjs.empty();

    auto selectedObjBlocks = this->getObjectsSelected(GRAPH_BLOCK);
    const bool selectedBlocks = not selectedObjBlocks.empty();

    getActionMap()["cut"]->setEnabled(selectedNoC);
    getActionMap()["copy"]->setEnabled(selectedNoC);
    getActionMap()["delete"]->setEnabled(selected);
    getActionMap()["rotateLeft"]->setEnabled(selectedNoC);
    getActionMap()["rotateRight"]->setEnabled(selectedNoC);
    getActionMap()["properties"]->setEnabled(selected);
    getActionMap()["increment"]->setEnabled(selectedBlocks);
    getActionMap()["decrement"]->setEnabled(selectedBlocks);
    getMenuMap()["setAffinityZone"]->setEnabled(selectedBlocks);

    //and enable/disable the actions in the move graph objects submenu
    for (auto child : getMenuMap()["moveGraphObjects"]->children())
    {
        auto action = dynamic_cast<QAction *>(child);
        if (action != nullptr) action->setEnabled(selectedNoC);
    }
}

void GraphDraw::render(void)
{
    if (not this->isVisible()) return;

    //pre-render to perform connection calculations
    const auto allObjs = this->getGraphObjects();
    for (auto obj : allObjs) obj->prerender();

    //clip the bounds
    for (auto obj : allObjs)
    {
        auto oldPos = obj->pos();
        oldPos.setX(std::min(std::max(oldPos.x(), 0.0), this->sceneRect().width()));
        oldPos.setY(std::min(std::max(oldPos.y(), 0.0), this->sceneRect().height()));
        obj->setPos(oldPos);
    }

    //cause full redraw
    this->scene()->update();

    //render objects
    /*
    for (auto obj : allObjs)
    {
        //draw the object
        painter.save();
        painter.scale(this->zoomScale(), this->zoomScale());
        obj->render(painter);
        painter.restore();

        //draw connection points (for debug)
        if (_showGraphConnectionPoints)
        {
            painter.save();
            painter.scale(this->zoomScale(), this->zoomScale());
            obj->renderConnectablePoints(painter);
            painter.restore();
        }

        //draw bounding boxes (for debug)
        if (_showGraphBoundingBoxes)
        {
            painter.save();
            painter.scale(this->zoomScale(), this->zoomScale());
            auto boundingRect = obj->getBoundingRect();
            painter.setPen(QPen(QColor("red")));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(boundingRect);
            painter.restore();
        }
    }
    */

    this->repaint();
}

void GraphDraw::handleCustomContextMenuRequested(const QPoint &pos)
{
    _lastContextMenuPos = pos;
    getMenuMap()["edit"]->exec(this->mapToGlobal(pos));
}

QWidget *makeGraphDraw(QWidget *parent)
{
    return new GraphDraw(parent);
};
