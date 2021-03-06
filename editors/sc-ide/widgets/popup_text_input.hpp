/*
    SuperCollider Qt IDE
    Copyright (c) 2012 Jakob Leben & Tim Blechmann
    http://www.audiosynth.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifndef SCIDE_WIDGETS_POPUP_TEXT_INPUT_HPP_INCLUDED
#define SCIDE_WIDGETS_POPUP_TEXT_INPUT_HPP_INCLUDED

#include <QInputDialog>
#include <QKeyEvent>

namespace ScIDE {

class PopupTextInput : public QInputDialog
{
    Q_OBJECT

public:
    PopupTextInput(QString const & labelText, QWidget * parent):
        QInputDialog(parent, Qt::Popup)
    {
        setOption(QInputDialog::NoButtons);
        setInputMode(QInputDialog::TextInput);
        setLabelText(labelText);
        setModal(true);

        if (parent) {
            QRect position = rect();
            position.moveCenter(parent->rect().center());
            resize(position.size());
            move(parent->mapToGlobal(position.topLeft()));
        }
    }

private:
    virtual void keyPressEvent(QKeyEvent * event)
    {
        switch (event->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
            accept();
            event->accept();
            return;

        default:;
        }

        QInputDialog::keyPressEvent(event);
    }

};

}

#endif // SCIDE_WIDGETS_POPUP_TEXT_INPUT_HPP_INCLUDED
