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

#define QT_NO_DEBUG_OUTPUT

#include "autocompleter.hpp"
#include "editor.hpp"
#include "tokens.hpp"
#include "../util/popup_widget.hpp"
#include "../../core/sc_introspection.hpp"
#include "../../core/sc_process.hpp"
#include "../../core/main.hpp"

#include "yaml-cpp/node.h"
#include "yaml-cpp/parser.h"

#include <QDebug>
#include <QLabel>
#include <QListView>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QHBoxLayout>
#include <QApplication>

#ifdef Q_WS_X11
# include <QGtkStyle>
#endif

namespace ScIDE {

static bool tokenMaybeName( Token::Type type )
{
    return (type == Token::Name || type == Token::Keyword || type == Token::Builtin);
}

static QString incrementedString( const QString & other )
{
    if(other.isEmpty())
        return QString();

    QString str = other;
    int pos = str.length()-1;
    str[pos] = QChar( str[pos].unicode() + 1 );
    return str;
}

class CompletionMenu : public PopUpWidget
{
public:
    enum DataRole {
        CompletionRole = Qt::UserRole,
        MethodRole
    };

    CompletionMenu( QWidget * parent = 0 ):
        PopUpWidget(parent),
        mCompletionRole( Qt::DisplayRole )
    {
        mModel = new QStandardItemModel(this);
        mFilterModel = new QSortFilterProxyModel(this);
        mFilterModel->setSourceModel(mModel);

        mListView = new QListView();
        mListView->setModel(mFilterModel);
        mListView->setFrameShape(QFrame::NoFrame);

        QHBoxLayout *layout = new QHBoxLayout(this);
        layout->addWidget(mListView);
        layout->setContentsMargins(1,1,1,1);

        connect(mListView, SIGNAL(clicked(QModelIndex)), this, SLOT(accept()));

        mListView->setFocus(Qt::OtherFocusReason);

        resize(200, 200);
    }

    void addItem( QStandardItem * item )
    {
        mModel->appendRow( item );
    }

    void setCompletionRole( int role )
    {
        mFilterModel->setFilterRole(role);
        mFilterModel->setSortRole(role);
        mCompletionRole = role;
    }

    QString currentText()
    {
        QStandardItem *item =
            mModel->itemFromIndex (
                mFilterModel->mapToSource (
                    mListView->currentIndex()));
        if (item)
            return item->data(mCompletionRole).toString();

        return QString();
    }

    const ScLanguage::Method * currentMethod()
    {
        QStandardItem *item =
            mModel->itemFromIndex (
                mFilterModel->mapToSource (
                    mListView->currentIndex()));

        return item ? item->data(MethodRole).value<const ScLanguage::Method*>() : 0;
    }

    QString exec( const QPoint & pos )
    {
        QString result;
        QPointer<CompletionMenu> self = this;
        if (PopUpWidget::exec(pos)) {
            if (!self.isNull())
                result = currentText();
        }
        return result;
    }

    QSortFilterProxyModel *model() { return mFilterModel; }

    QListView *view() { return mListView; }

protected:
    virtual bool eventFilter( QObject * obj, QEvent * ev )
    {
        if (isVisible() && obj == parentWidget() && ev->type() == QEvent::KeyPress)
        {
            QKeyEvent *kev = static_cast<QKeyEvent*>(ev);
            switch(kev->key())
            {
            case Qt::Key_Up:
            case Qt::Key_Down:
            case Qt::Key_PageUp:
            case Qt::Key_PageDown:
                QApplication::sendEvent( mListView, ev );
                return true;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                accept();
                return true;
            }
        }

        return PopUpWidget::eventFilter(obj, ev);
    }

private:
    QListView *mListView;
    QStandardItemModel *mModel;
    QSortFilterProxyModel *mFilterModel;
    int mCompletionRole;
};

class MethodCallWidget : public QWidget
{
public:
    MethodCallWidget( QWidget * parent = 0 ):
        QWidget( parent, Qt::ToolTip )
    {
        mLabel = new QLabel();
        mLabel->setTextFormat( Qt::RichText );

#ifdef Q_WS_X11
        if (qobject_cast<QGtkStyle*>(style()) != 0) {
            QPalette p;
            p.setColor( QPalette::Window, QColor(255, 255, 220) );
            p.setColor( QPalette::WindowText, Qt::black );
            setPalette(p);
        }
        else
#endif
        {
            QPalette p( palette() );
            p.setColor( QPalette::Window, p.color(QPalette::ToolTipBase) );
            setPalette(p);
            mLabel->setForegroundRole(QPalette::ToolTipText);
        }

        QHBoxLayout *box = new QHBoxLayout;
        box->setContentsMargins(5,2,5,2);
        box->addWidget(mLabel);
        setLayout(box);

        parent->installEventFilter(this);
    }

    bool eventFilter( QObject *obj, QEvent *ev )
    {
        if (obj == parentWidget() && ev->type() == QEvent::FocusOut)
            hide();

        return QWidget::eventFilter(obj, ev);
    }

    void showMethod( const ScLanguage::Method * method, int argNum )
    {
        QString text;
        int argc = method->arguments.count();
        for (int i = 0; i < argc; ++i)
        {
            const ScLanguage::Argument & arg = method->arguments[i];

            if (i == argNum) {
                text += QString(
                    "<span style=\""
                    //"text-decoration: underline;"
                    "font-weight: bold;"
                    "\">");
            }

            text += arg.name;

            QString val = arg.defaultValue;
            if (!val.isEmpty())
                text += " = " + val;

            if (i == argNum)
                text += "</span>";

            if (i != argc - 1)
                text += ", &nbsp;&nbsp;";
        }
        mLabel->setText(text);
    }

private:
    QLabel *mLabel;
};

AutoCompleter::AutoCompleter( CodeEditor *editor ):
    QObject(editor),
    mEditor(editor)
{
    mCompletion.on = false;

    connect(editor, SIGNAL(cursorPositionChanged()),
            this, SLOT(onCursorChanged()));
}

void AutoCompleter::documentChanged( QTextDocument * doc )
{
    connect(doc, SIGNAL(contentsChange(int,int,int)),
            this, SLOT(onContentsChange(int,int,int)));
}

inline QTextDocument *AutoCompleter::document()
{
    return static_cast<QPlainTextEdit*>(mEditor)->document();
}

void AutoCompleter::keyPress( QKeyEvent *e )
{
    int key = e->key();
    switch (e->key())
    {
    case Qt::Key_ParenLeft:
    case Qt::Key_Comma:
        triggerMethodCallAid(false);
        break;
    case Qt::Key_Backspace:
    case Qt::Key_Delete:
        return;
    default:
        qDebug(">>> key");
        if (!e->text().isEmpty() && !mCompletion.on)
            triggerCompletion();
    }
}

void AutoCompleter::onContentsChange( int pos, int removed, int added )
{
    qDebug(">>> contentsChange");

    while (!mMethodCall.stack.isEmpty())
    {
        MethodCall & call = mMethodCall.stack.top();
        if (pos > call.position)
            break;
        else {
            qDebug("Method call: change before method call. popping.");
            mMethodCall.stack.pop();
        }
    }

    if (mCompletion.on)
    {
        if(pos < mCompletion.contextPos)
        {
            quitCompletion("context changed");
        }
        else if(pos <= mCompletion.pos + mCompletion.len)
        {
            QTextBlock block( document()->findBlock(mCompletion.pos) );
            TokenIterator it( block, mCompletion.pos - block.position() );
            Token::Type type = it.type();
            if (type == Token::Class || tokenMaybeName(type)) {
                mCompletion.len = it->length;
                mCompletion.text = tokenText(it);
            }
            else {
                mCompletion.len = 0;
                mCompletion.text.clear();
            }
            if (!mCompletion.menu.isNull())
                updateCompletionMenu(false);
        }
    }
}

void AutoCompleter::onCursorChanged()
{
    qDebug(">>> cursorChanged");
    int cursorPos = mEditor->textCursor().position();

    // completion
    if (mCompletion.on) {
        if (cursorPos < mCompletion.pos ||
            cursorPos > mCompletion.pos + mCompletion.len)
        {
            quitCompletion("out of bounds");
        }
    }

    if (!mMethodCall.menu.isNull()) {
        qDebug("Method call: quitting menu");
        delete mMethodCall.menu;
    }

    updateMethodCall(cursorPos);
}

void AutoCompleter::triggerCompletion(bool forceShow)
{
    if (mCompletion.on) {
        qDebug("AutoCompleter::triggerCompletion(): completion already started.");
        updateCompletionMenu(forceShow);
        return;
    }

    QTextCursor cursor( mEditor->textCursor() );
    const int cursorPos = cursor.positionInBlock();
    QTextBlock block( cursor.block() );
    TokenIterator it( block, cursorPos - 1 );

    if (!it.isValid())
        return;

    const Token & triggeringToken = *it;

    if (triggeringToken.type == Token::Class)
    {
        if (triggeringToken.length < 3)
            return;
        mCompletion.type = ClassCompletion;
        mCompletion.pos = it.position();
        mCompletion.len = it->length;
        mCompletion.text = tokenText(it);
        mCompletion.contextPos = mCompletion.pos + 3;
        mCompletion.base = mCompletion.text;
        mCompletion.base.truncate(3);
    }
    else {
        // Parse method call
        TokenIterator objectIt, dotIt, methodIt;

        Token::Type objectTokenType = Token::Unknown;

        bool objectIsClass            = false;
        bool objectIsInferredInstance = false;

        if (triggeringToken.character == '.') {
            dotIt = it;
            --it;

            objectTokenType = it.type();
            switch (objectTokenType) {
            case Token::Class:
                objectIt = it;
                objectIsClass = true;
                break;

            case Token::Char:
            case Token::String:
            case Token::Builtin:
            case Token::Float:
                // we could trigger on integers, but that conflicts with using point as comma
                objectIt = it;
                objectIsInferredInstance = true;
                break;

            default:
                return;
            }

            TokenIterator currentIt = dotIt.next();
            if (tokenMaybeName(currentIt.type())
                && currentIt.block() == dotIt.block()
                && currentIt->positionInBlock == dotIt->positionInBlock + 1)
                    methodIt = currentIt;

        } else if (tokenMaybeName(triggeringToken.type)) {
            methodIt = it;
            --it;
            if (it.isValid() && it->character == '.')
                dotIt = it;
            else
                return;
            --it;

            objectTokenType = it.type();
            switch (objectTokenType) {
            case Token::Class:
                objectIt = it;
                objectIsClass = true;
                break;

            case Token::Char:
            case Token::Symbol:
            case Token::String:
            case Token::Builtin:
            case Token::Float:
                // we could trigger on integers, but that conflicts with using point as comma
                objectIt = it;
                objectIsInferredInstance = true;
                break;

            default:
                ;
            }
        } else
            return;

        if (! (objectIsClass || objectIsInferredInstance) && methodIt->length < 3)
            return;

        if (methodIt.isValid()) {
            mCompletion.pos = methodIt.position();
            mCompletion.len = methodIt->length;
            mCompletion.text = tokenText(methodIt);
        } else {
            mCompletion.pos = dotIt.position() + 1;
            mCompletion.len = 0;
            mCompletion.text.clear();
        }

        if (objectIsClass) {
            mCompletion.contextPos = mCompletion.pos;
            mCompletion.base       = tokenText(objectIt);
            mCompletion.type       = ClassMethodCompletion;
        } else if (objectIsInferredInstance) {
            mCompletion.contextPos = mCompletion.pos;
            mCompletion.base       = tokenText(objectIt);
            mCompletion.type       = InferredObjectMethodCompletion;
            mCompletion.tokenType  = objectTokenType;
        } else {
            mCompletion.contextPos = mCompletion.pos + 3;
            mCompletion.base = tokenText(methodIt);
            mCompletion.base.truncate(3);
            mCompletion.type = MethodCompletion;
        }
    }

    mCompletion.on = true;

    qDebug() << QString("Completion: ON <%1>").arg(mCompletion.base);

    showCompletionMenu(forceShow);
}

void AutoCompleter::quitCompletion( const QString & reason )
{
    Q_ASSERT(mCompletion.on);

    qDebug() << QString("Completion: OFF (%1)").arg(reason);

    if (mCompletion.menu) {
        mCompletion.menu->hide();
        mCompletion.menu->deleteLater();
        mCompletion.menu = 0;
    }

    mCompletion.on = false;
}

void AutoCompleter::showCompletionMenu(bool forceShow)
{
    qDebug(">>> showCompletionMenu");

    using namespace ScLanguage;
    using ScLanguage::Method;

    Q_ASSERT(mCompletion.on);
    Q_ASSERT(mCompletion.menu.isNull());

    QPointer<CompletionMenu> menu;

    switch (mCompletion.type) {
    case ClassCompletion:
        menu = menuForClassCompletion(mCompletion, mEditor);
        break;

    case ClassMethodCompletion:
        menu = menuForClassMethodCompletion(mCompletion, mEditor);
        break;

    case MethodCompletion:
        menu = menuForMethodCompletion(mCompletion, mEditor);
        break;

    case InferredObjectMethodCompletion:
        qDebug() << "Inferred Object" << mCompletion.base;
        menu = menuForInferedObjectMethodCompletion(mCompletion, mEditor);
        break;
    }

    if (menu == NULL) return;

    mCompletion.menu = menu;

    connect(menu, SIGNAL(finished(int)), this, SLOT(onCompletionMenuFinished(int)));

    QTextCursor cursor(document());
    cursor.setPosition(mCompletion.pos);
    QPoint pos = mEditor->viewport()->mapToGlobal( mEditor->cursorRect(cursor).bottomLeft() )
        + QPoint(0,5);

    menu->popup(pos);

    updateCompletionMenu(forceShow);
}

CompletionMenu * AutoCompleter::menuForClassCompletion(CompletionDescription const & completion,
                                                       CodeEditor * editor)
{
    using namespace ScLanguage;
    const Introspection & introspection = Main::instance()->scProcess()->introspection();

    const ClassMap & classes = introspection.classMap();

    QString min = completion.base;
    QString max = incrementedString(min);

    ClassMap::const_iterator matchStart, matchEnd;
    matchStart = classes.lower_bound(min);
    matchEnd = classes.lower_bound(max);
    if (matchStart == matchEnd) {
        qDebug() << "Completion: no class matches:" << completion.base;
        return NULL;
    }

    CompletionMenu * menu = new CompletionMenu(editor);

    for (ClassMap::const_iterator it = matchStart; it != matchEnd; ++it) {
        Class *klass = it->second.data();
        menu->addItem( new QStandardItem(klass->name) );
    }

    return menu;
}

CompletionMenu * AutoCompleter::menuForClassMethodCompletion(CompletionDescription const & completion,
                                                             CodeEditor * editor)
{
    using namespace ScLanguage;
    const Introspection & introspection = Main::instance()->scProcess()->introspection();

    const ClassMap & classes = introspection.classMap();
    ClassMap::const_iterator it = classes.find(completion.base);
    if (it == classes.end()) {
        qDebug() << "Completion: class not found:" << completion.base;
        return NULL;
    }

    Class *metaClass = it->second->metaClass;
    QMap<QString, const Method*> matching;
    do {
        foreach (const Method * method, metaClass->methods)
        {
            QString methodName = method->name.get();
            // Operators are also methods, but are not valid in
            // a method call syntax, so filter them out.
            Q_ASSERT(!methodName.isEmpty());
            if (!methodName[0].isLetter())
                continue;

            if (matching.value(methodName) != 0)
                continue;

            matching.insert(methodName, method);
        }
        metaClass = metaClass->superClass;
    } while (metaClass);

    CompletionMenu * menu = new CompletionMenu(editor);

    foreach(const Method *method, matching) {
        QStandardItem *item = new QStandardItem(method->name.get());
        item->setData( QVariant::fromValue(method),
                       CompletionMenu::MethodRole );
        menu->addItem(item);
    }

    return menu;
}

CompletionMenu * AutoCompleter::menuForMethodCompletion(CompletionDescription const & completion,
                                                        CodeEditor * editor)
{
    using namespace ScLanguage;
    const Introspection & introspection = Main::instance()->scProcess()->introspection();

    const MethodMap & methods = introspection.methodMap();

    QString min = completion.base;
    QString max = incrementedString(min);

    MethodMap::const_iterator matchStart, matchEnd;
    matchStart = methods.lower_bound(min);
    matchEnd = methods.lower_bound(max);
    if (matchStart == matchEnd) {
        qDebug() << "Completion: no method matches:" << completion.base;
        return NULL;
    }

    CompletionMenu *menu = new CompletionMenu(editor);
    menu->setCompletionRole(CompletionMenu::CompletionRole);

    for (MethodMap::const_iterator it = matchStart; it != matchEnd; ) {
        const Method *method = it->second.data();

        std::pair<MethodMap::const_iterator, MethodMap::const_iterator> range
            = methods.equal_range(it->first);

        int count = std::distance(range.first, range.second);

        QStandardItem *item = new QStandardItem();

        QString methodName = method->name.get();
        QString detail(" [ %1 ]");
        if (count == 1) {
            item->setText( methodName + detail.arg(method->ownerClass->name) );
            item->setData( QVariant::fromValue(method), CompletionMenu::MethodRole );
        } else
            item->setText(methodName + detail.arg(count));

        item->setData(methodName, CompletionMenu::CompletionRole);

        menu->addItem(item);

        it = range.second;
    }
    return menu;
}

const ScLanguage::Class * AutoCompleter::classForCompletionDescription(AutoCompleter::CompletionDescription const & completion)
{
    using namespace ScLanguage;
    const Introspection & introspection = Main::instance()->scProcess()->introspection();

    switch (completion.tokenType) {
    case Token::Float:
        if (completion.base.contains(".")) // else it is an int
            return introspection.findClass("Float");
        else
            return NULL;

    case Token::Char:
        return introspection.findClass("Char");

    case Token::String:
        return introspection.findClass("String");

    case Token::Symbol:
        return introspection.findClass("Symbol");

    default:
        ;
    }

    QString const & objectString = completion.base;

    if (objectString == QString("true"))
        return introspection.findClass("True");

    if (objectString == QString("false"))
        return introspection.findClass("False");

    if (objectString == QString("nil"))
        return introspection.findClass("Nil");

    if (objectString == QString("thisProcess"))
        return introspection.findClass("Main");

    if (objectString == QString("thisFunction"))
        return introspection.findClass("Function");

    if (objectString == QString("thisMethod"))
        return introspection.findClass("Method");

    if (objectString == QString("thisFunctionDef"))
        return introspection.findClass("FunctionDef");

    if (objectString == QString("thisThread"))
        return introspection.findClass("Thread");

    if (objectString == QString("currentEnvironment"))
        return introspection.findClass("Environment");

    if (objectString == QString("topEnvironment"))
        return introspection.findClass("Environment");

    if (objectString == QString("inf"))
        return introspection.findClass("Float");

    return NULL;
}

CompletionMenu * AutoCompleter::menuForInferedObjectMethodCompletion(CompletionDescription const & completion,
                                                                     CodeEditor * editor)
{
    using namespace ScLanguage;
    const Introspection & introspection = Main::instance()->scProcess()->introspection();

    const MethodMap & allMethods = introspection.methodMap();

    const Class * classOfObject = classForCompletionDescription(completion);
    if (classOfObject == NULL) {
        qDebug() << "autocompletion unimplemented for" << completion.base;
        return NULL;
    }

    CompletionMenu *menu = new CompletionMenu(editor);
    menu->setCompletionRole(CompletionMenu::CompletionRole);

    for (MethodMap::const_iterator it = allMethods.begin(); it != allMethods.end(); ++it) {
        const Method *method = it->second.data();

        if (introspection.isClassMethod(method))
            continue;

        if ((classOfObject == method->ownerClass) || (classOfObject->isSubclassOf(method->ownerClass)) ) {
            QStandardItem *item = new QStandardItem();
            QString methodName = method->name.get();
            QString detail(" [ %1 ]");
            item->setText( methodName + detail.arg(method->ownerClass->name) );
            item->setData( QVariant::fromValue(method), CompletionMenu::MethodRole );

            item->setData(methodName, CompletionMenu::CompletionRole);

            menu->addItem(item);
        }
    }
    return menu;
}

void AutoCompleter::updateCompletionMenu(bool forceShow)
{
    Q_ASSERT(mCompletion.on && !mCompletion.menu.isNull());

    CompletionMenu *menu = mCompletion.menu;

    if (!mCompletion.text.isEmpty()) {
        QString pattern = mCompletion.text;
        pattern.prepend("^");
        menu->model()->setFilterRegExp(pattern);
    } else
        menu->model()->setFilterRegExp(QString());

    if (menu->model()->hasChildren()) {
        menu->model()->sort(0);
        menu->view()->setCurrentIndex( menu->model()->index(0,0) );
        if (forceShow || menu->currentText() != mCompletion.text)
            menu->show();
        else
            menu->hide();
    } else
        menu->hide();
}

void AutoCompleter::onCompletionMenuFinished( int result )
{
    qDebug("Completion: menu finished");

    if (!mCompletion.on)
        return;

    if (result) {
        QString text = mCompletion.menu->currentText();

        if (!text.isEmpty()) {
#if 0
            CompletionType type = mCompletion.type;
            const ScLanguage::Method *method = 0;
            if (type == MethodCompletion || type == ClassMethodCompletion)
                method = mCompletion.menu->currentMethod();
#endif

            quitCompletion("done");

            QTextCursor cursor( mEditor->textCursor() );
            cursor.setPosition( mCompletion.pos );
            cursor.setPosition( mCompletion.pos + mCompletion.len, QTextCursor::KeepAnchor );
            cursor.insertText(text);
#if 0
            if (method) {
                cursor.insertText("(");
                MethodCall call;
                call.position = cursor.position() - 1;
                call.method = method;
                pushMethodCall(call);
                showMethodCall(call, 0);
            }
#endif
            return;
        }
    }

    // Do not cancel completion whenever menu hidden.
    // It could be hidden because of current filter yielding 0 results.

    //quitCompletion("cancelled");
}

void AutoCompleter::triggerMethodCallAid( bool force )
{
    // go find the bracket that I'm currently in,
    // and count relevant commas along the way

    QTextDocument *doc = document();
    QTextCursor cursor( mEditor->textCursor() );

    int pos = cursor.position();

    QTextBlock block( doc->findBlock(pos) );
    if (!block.isValid())
        return;
    pos -= block.position();

    TokenIterator it( TokenIterator::leftOf( block, pos ) );

    int level = 1;
    int argPos = 0;

    while (it.isValid())
    {
        char chr = it->character;
        Token::Type type = it->type;
        if (chr == ',') {
            if (level == 1)
                ++argPos;
        }
        else if (type == Token::ClosingBracket)
            ++level;
        else if (type == Token::OpeningBracket)
        {
            --level;
            if (level == 0) {
                if (chr == '(')
                    break;
                else
                    return;
            }
        }
        --it;
    }

    if (!it.isValid())
        return;

    int bracketPos;
    pos = bracketPos = it.position();

    QString className, methodName;

    --it;
    if (it.isValid())
    {
        int type = it->type;
        if (type == Token::Class) {
            className = tokenText(it);
            methodName = "new";
        }
        else if (type == Token::Name) {
            methodName = tokenText(it);
            --it;
            if (it.isValid() && it->character == '.') {
                --it;
                if (it.isValid() && it->type == Token::Class)
                    className = tokenText(it);
            }
        }
    }

    if (methodName.isEmpty())
        return;

    qDebug("Method call: found call: %s.%s(%i)",
           className.toStdString().c_str(),
           methodName.toStdString().c_str(),
           argPos);

    if ( !mMethodCall.stack.isEmpty() && mMethodCall.stack.last().position == bracketPos )
    {
        qDebug("Method call: call already on stack");
        // method call popup should have been updated by updateMethodCall();

        if (force) {
            qDebug("Method call: forced re-trigger, popping current call.");
            mMethodCall.stack.pop();
            hideMethodCall();
        }
        else
            return;
    }

    qDebug("Method call: new call");
    MethodCall call;
    call.position = bracketPos;
    pushMethodCall(call);

    using namespace ScLanguage;
    using std::pair;

    const Introspection & introspection = Main::instance()->scProcess()->introspection();

    const Method *method = 0;

    if (!className.isEmpty())
    {
        const ClassMap & classes = introspection.classMap();
        ClassMap::const_iterator it = classes.find(className);
        if (it == classes.end()) {
            qDebug() << "MethodCall: class not found:" << className;
            return;
        }

        Class *metaClass = it->second->metaClass;
        do {
            foreach (const Method * m, metaClass->methods)
            {
                if (m->name == methodName) {
                    method = m;
                    break;
                }
            }
            if (method) break;
            metaClass = metaClass->superClass;
        } while (metaClass);
    }
    else {
        const MethodMap & methods = introspection.methodMap();

        pair<MethodMap::const_iterator, MethodMap::const_iterator> match =
            methods.equal_range(methodName);

        if (match.first == match.second) {
            qDebug() << "MethodCall: no method matches:" << methodName;
            return;
        } else if (std::distance(match.first, match.second) == 1)
            method = match.first->second.data();
        else {
            Q_ASSERT(mMethodCall.menu.isNull());
            QPointer<CompletionMenu> menu = new CompletionMenu(mEditor);
            mMethodCall.menu = menu;

            for (MethodMap::const_iterator it = match.first; it != match.second; ++it)
            {
                const Method *method = it->second.data();
                QStandardItem *item = new QStandardItem();
                item->setText(method->name + " (" + method->ownerClass->name + ')');
                item->setData( QVariant::fromValue(method), CompletionMenu::MethodRole );
                menu->addItem(item);
            }

            QTextCursor cursor(document());
            cursor.setPosition(bracketPos);
            QPoint pos =
                mEditor->viewport()->mapToGlobal( mEditor->cursorRect(cursor).bottomLeft() )
                + QPoint(0,5);

            if ( ! static_cast<PopUpWidget*>(menu)->exec(pos) ) {
                delete menu;
                return;
            }

            method = menu->currentMethod();
            delete menu;
        }
    }

    if (method) {
        Q_ASSERT(!mMethodCall.stack.isEmpty());
        mMethodCall.stack.top().method = method;
        updateMethodCall( mEditor->textCursor().position() );
    }
}

void AutoCompleter::updateMethodCall( int cursorPos )
{
    int i = mMethodCall.stack.count();
    while (i--)
    {
        MethodCall & call = mMethodCall.stack[i];
        if (call.position >= cursorPos) {
            qDebug("Method call: call right of cursor. popping.");
            mMethodCall.stack.pop();
            continue;
        }

        QTextBlock block( document()->findBlock( call.position ) );
        TokenIterator token = TokenIterator::rightOf(block, call.position - block.position());
        if (!token.isValid()) {
            qWarning("Method call: call stack out of sync!");
            mMethodCall.stack.clear();
            break;
        }

        ++token;
        int arg = 0;
        int level = 1;
        TokenIterator argNameToken;
        while( level > 0 && token.isValid() && token.position() < cursorPos )
        {
            char chr = token.character();
            Token::Type type = token->type;
            if (level == 1) {
                if (type == Token::SymbolArg) {
                    argNameToken = token;
                    arg = -1;
                }
                else if (chr == ',') {
                    argNameToken = TokenIterator();
                    if (arg != -1)
                        ++arg;
                }
            }

            if (type == Token::OpeningBracket)
                ++level;
            else if (type == Token::ClosingBracket)
                --level;

            ++token;
        }

        if (level <= 0) {
            Q_ASSERT(i == mMethodCall.stack.count() - 1);
            qDebug("Method call: call left of cursor. popping.");
            mMethodCall.stack.pop();
            continue;
        }

        if (!call.method || !call.method->arguments.count()) {
            qDebug("Method call: no info to show. skipping.");
            continue;
        }

        if (argNameToken.isValid()) {
            arg = -1;
            QString argName = tokenText(argNameToken);
            argName.chop(1);
            for (int idx = 0; idx < call.method->arguments.count(); ++idx) {
                if (call.method->arguments[idx].name == argName) {
                    arg = idx;
                    break;
                }
            }
        }
        qDebug("Method call: found current call: %s(%i)",
            call.method->name.get().toStdString().c_str(), arg);
        showMethodCall(call, arg);
        return;
    }

    hideMethodCall();
}

void AutoCompleter::pushMethodCall( const MethodCall & call )
{
    qDebug("Method Call: pushing on stack.");
    Q_ASSERT( mMethodCall.stack.isEmpty()
        || mMethodCall.stack.last().position < call.position );

    mMethodCall.stack.push(call);
}

void AutoCompleter::showMethodCall( const MethodCall & call, int arg )
{
    QTextCursor cursor(document());
    cursor.setPosition(call.position);
    QPoint pos =
        mEditor->viewport()->mapToGlobal( mEditor->cursorRect(cursor).topLeft() );
    pos += QPoint(0, -20);

    if (mMethodCall.widget.isNull())
        mMethodCall.widget = new MethodCallWidget(mEditor);

    MethodCallWidget *w = mMethodCall.widget;

    w->showMethod( call.method, arg );
    w->resize(w->sizeHint());
    w->move(pos);
    w->show();
}

void AutoCompleter::hideMethodCall()
{
    delete mMethodCall.widget;
}

QString AutoCompleter::tokenText( TokenIterator & it )
{
    if (!it.isValid())
        return QString();

    int pos = it.position();
    QTextCursor cursor(document());
    cursor.setPosition(pos);
    cursor.setPosition(pos + it->length, QTextCursor::KeepAnchor);
    return cursor.selectedText();
}

} // namespace ScIDE

#undef QT_NO_DEBUG_OUTPUT
