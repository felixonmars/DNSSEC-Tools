#ifndef VALIDATEVIEWBOX_H
#define VALIDATEVIEWBOX_H

#include <QGraphicsRectItem>
#include <QList>
#include <QColor>
#include <QPair>

typedef QPair<QGraphicsLineItem *, QColor> LineItemPair;
typedef QPair<QGraphicsPathItem *, QColor> PathItemPair;

class ValidateViewBox : public QGraphicsRectItem
{
public:
    ValidateViewBox ( qreal x, qreal y, qreal width, qreal height, QGraphicsItem * parent = 0 );
    void mousePressEvent(QGraphicsSceneMouseEvent *event);
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event);

    bool isSelected() const { return m_isSelected; }
    void addLineObject(LineItemPair *item) { m_lines.append(item); }
    void addPathObject(PathItemPair *item) { m_paths.append(item); }
    void addLineObject(QGraphicsLineItem *item, QColor color) { m_lines.append(new LineItemPair(item, color)); }
    void addPathObject(QGraphicsPathItem *item, QColor color) { m_paths.append(new PathItemPair(item, color)); }


private:
    bool m_isSelected;
    QList<LineItemPair *> m_lines;
    QList<PathItemPair *> m_paths;

};

#endif // VALIDATEVIEWBOX_H