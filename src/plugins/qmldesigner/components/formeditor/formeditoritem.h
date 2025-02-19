/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company.  For licensing terms and
** conditions see http://www.qt.io/terms-conditions.  For further information
** use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file.  Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, The Qt Company gives you certain additional
** rights.  These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#ifndef FORMEDITORITEM_H
#define FORMEDITORITEM_H

#include <QPointer>
#include <QGraphicsWidget>
#include <qmlitemnode.h>
#include "snappinglinecreator.h"

QT_BEGIN_NAMESPACE
class QTimeLine;
QT_END_NAMESPACE

namespace QmlDesigner {

class FormEditorScene;
class FormEditorView;
class AbstractFormEditorTool;

namespace Internal {
    class GraphicItemResizer;
    class MoveController;
}

class QMLDESIGNERCORE_EXPORT FormEditorItem : public QGraphicsObject
{
    Q_OBJECT

    friend class QmlDesigner::FormEditorScene;
public:
    ~FormEditorItem();

    void paint(QPainter * painter, const QStyleOptionGraphicsItem * option, QWidget * widget = 0 );

    bool isContainer() const;
    QmlItemNode qmlItemNode() const;


    enum { Type = UserType + 0xfffa };

    int type() const;

    static FormEditorItem* fromQGraphicsItem(QGraphicsItem *graphicsItem);

    void updateSnappingLines(const QList<FormEditorItem*> &exceptionList,
                             FormEditorItem *transformationSpaceItem);

    SnapLineMap topSnappingLines() const;
    SnapLineMap bottomSnappingLines() const;
    SnapLineMap leftSnappingLines() const;
    SnapLineMap rightSnappingLines() const;
    SnapLineMap horizontalCenterSnappingLines() const;
    SnapLineMap verticalCenterSnappingLines() const;

    SnapLineMap topSnappingOffsets() const;
    SnapLineMap bottomSnappingOffsets() const;
    SnapLineMap leftSnappingOffsets() const;
    SnapLineMap rightSnappingOffsets() const;

    QList<FormEditorItem*> childFormEditorItems() const;
    QList<FormEditorItem*> offspringFormEditorItems() const;

    FormEditorScene *scene() const;
    FormEditorItem *parentItem() const;

    QRectF boundingRect() const;
    QPainterPath shape() const;
    bool contains(const QPointF &point) const;

    void updateGeometry();
    void updateVisibilty();

    void showAttention();

    FormEditorView *formEditorView() const;

    void setHighlightBoundingRect(bool highlight);
    void blurContent(bool blurContent);

    void setContentVisible(bool visible);
    bool isContentVisible() const;

    bool isFormEditorVisible() const;
    void setFormEditorVisible(bool isVisible);

    QPointF center() const;
    qreal selectionWeigth(const QPointF &point, int iteration);

protected:
    AbstractFormEditorTool* tool() const;
    void paintBoundingRect(QPainter *painter) const;
    void paintPlaceHolderForInvisbleItem(QPainter *painter) const;
    void paintComponentContentVisualisation(QPainter *painter, const QRectF &clippinRectangle) const;
    QList<FormEditorItem*> offspringFormEditorItemsRecursive(const FormEditorItem *formEditorItem) const;

private: // functions
    FormEditorItem(const QmlItemNode &qmlItemNode, FormEditorScene* scene);
    void setup();

private: // variables
    SnappingLineCreator m_snappingLineCreator;
    QmlItemNode m_qmlItemNode;
    QPointer<QTimeLine> m_attentionTimeLine;
    QTransform m_inverseAttentionTransform;
    QRectF m_boundingRect;
    QRectF m_paintedBoundingRect;
    QRectF m_selectionBoundingRect;
    double m_borderWidth;
    bool m_highlightBoundingRect;
    bool m_blurContent;
    bool m_isContentVisible;
    bool m_isFormEditorVisible;
};


inline int FormEditorItem::type() const
{
    return UserType + 0xfffa;
}

}

#endif // FORMEDITORITEM_H
