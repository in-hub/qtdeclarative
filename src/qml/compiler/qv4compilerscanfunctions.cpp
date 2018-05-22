/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtQml module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qv4compilerscanfunctions_p.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>
#include <QtCore/QSet>
#include <QtCore/QBuffer>
#include <QtCore/QBitArray>
#include <QtCore/QLinkedList>
#include <QtCore/QStack>
#include <private/qqmljsast_p.h>
#include <private/qv4compilercontext_p.h>
#include <private/qv4codegen_p.h>
#include <private/qv4string_p.h>

QT_USE_NAMESPACE
using namespace QV4;
using namespace QV4::Compiler;
using namespace QQmlJS::AST;

ScanFunctions::ScanFunctions(Codegen *cg, const QString &sourceCode, ContextType defaultProgramType)
    : _cg(cg)
    , _sourceCode(sourceCode)
    , _context(nullptr)
    , _allowFuncDecls(true)
    , defaultProgramType(defaultProgramType)
{
}

void ScanFunctions::operator()(Node *node)
{
    if (node)
        node->accept(this);

    calcEscapingVariables();
}

void ScanFunctions::enterGlobalEnvironment(ContextType compilationMode)
{
    enterEnvironment(astNodeForGlobalEnvironment, compilationMode, QStringLiteral("%GlobalCode"));
}

void ScanFunctions::enterEnvironment(Node *node, ContextType compilationMode, const QString &name)
{
    Context *c = _cg->_module->contextMap.value(node);
    if (!c)
        c = _cg->_module->newContext(node, _context, compilationMode);
    if (!c->isStrict)
        c->isStrict = _cg->_strictMode;
    c->name = name;
    _contextStack.append(c);
    _context = c;
}

void ScanFunctions::leaveEnvironment()
{
    _contextStack.pop();
    _context = _contextStack.isEmpty() ? nullptr : _contextStack.top();
}

void ScanFunctions::checkDirectivePrologue(StatementList *ast)
{
    for (StatementList *it = ast; it; it = it->next) {
        if (ExpressionStatement *expr = cast<ExpressionStatement *>(it->statement)) {
            if (StringLiteral *strLit = cast<StringLiteral *>(expr->expression)) {
                // Use the source code, because the StringLiteral's
                // value might have escape sequences in it, which is not
                // allowed.
                if (strLit->literalToken.length < 2)
                    continue;
                QStringRef str = _sourceCode.midRef(strLit->literalToken.offset + 1, strLit->literalToken.length - 2);
                if (str == QLatin1String("use strict")) {
                    _context->isStrict = true;
                } else {
                    // TODO: give a warning.
                }
                continue;
            }
        }

        break;
    }
}

void ScanFunctions::checkName(const QStringRef &name, const SourceLocation &loc)
{
    if (_context->isStrict) {
        if (name == QLatin1String("implements")
                || name == QLatin1String("interface")
                || name == QLatin1String("let")
                || name == QLatin1String("package")
                || name == QLatin1String("private")
                || name == QLatin1String("protected")
                || name == QLatin1String("public")
                || name == QLatin1String("static")
                || name == QLatin1String("yield")) {
            _cg->throwSyntaxError(loc, QStringLiteral("Unexpected strict mode reserved word"));
        }
    }
}

bool ScanFunctions::visit(Program *ast)
{
    enterEnvironment(ast, defaultProgramType, QStringLiteral("%ProgramCode"));
    checkDirectivePrologue(ast->statements);
    return true;
}

void ScanFunctions::endVisit(Program *)
{
    leaveEnvironment();
}

bool ScanFunctions::visit(CallExpression *ast)
{
    if (!_context->hasDirectEval) {
        if (IdentifierExpression *id = cast<IdentifierExpression *>(ast->base)) {
            if (id->name == QLatin1String("eval")) {
                if (_context->usesArgumentsObject == Context::ArgumentsObjectUnknown)
                    _context->usesArgumentsObject = Context::ArgumentsObjectUsed;
                _context->hasDirectEval = true;
            }
        }
    }
    return true;
}

bool ScanFunctions::visit(PatternElement *ast)
{
    if (!ast->isVariableDeclaration())
        return true;

    QStringList names;
    ast->boundNames(&names);

    for (const QString &name : qAsConst(names)) {
        if (_context->isStrict && (name == QLatin1String("eval") || name == QLatin1String("arguments")))
            _cg->throwSyntaxError(ast->identifierToken, QStringLiteral("Variable name may not be eval or arguments in strict mode"));
        checkName(QStringRef(&name), ast->identifierToken);
        if (name == QLatin1String("arguments"))
            _context->usesArgumentsObject = Context::ArgumentsObjectNotUsed;
        if (ast->scope == VariableScope::Const && !ast->initializer && !ast->destructuringPattern()) {
            _cg->throwSyntaxError(ast->identifierToken, QStringLiteral("Missing initializer in const declaration"));
            return false;
        }
        if (!_context->addLocalVar(name, ast->initializer ? Context::VariableDefinition : Context::VariableDeclaration, ast->scope)) {
            _cg->throwSyntaxError(ast->identifierToken, QStringLiteral("Identifier %1 has already been declared").arg(name));
            return false;
        }
    }
    return true;
}

bool ScanFunctions::visit(IdentifierExpression *ast)
{
    checkName(ast->name, ast->identifierToken);
    if (_context->usesArgumentsObject == Context::ArgumentsObjectUnknown && ast->name == QLatin1String("arguments"))
        _context->usesArgumentsObject = Context::ArgumentsObjectUsed;
    _context->addUsedVariable(ast->name.toString());
    return true;
}

bool ScanFunctions::visit(ExpressionStatement *ast)
{
    if (FunctionExpression* expr = AST::cast<AST::FunctionExpression*>(ast->expression)) {
        if (!_allowFuncDecls)
            _cg->throwSyntaxError(expr->functionToken, QStringLiteral("conditional function or closure declaration"));

        if (!enterFunction(expr, /*enterName*/ true))
            return false;
        Node::accept(expr->formals, this);
        Node::accept(expr->body, this);
        leaveEnvironment();
        return false;
    } else {
        SourceLocation firstToken = ast->firstSourceLocation();
        if (_sourceCode.midRef(firstToken.offset, firstToken.length) == QLatin1String("function")) {
            _cg->throwSyntaxError(firstToken, QStringLiteral("unexpected token"));
        }
    }
    return true;
}

bool ScanFunctions::visit(FunctionExpression *ast)
{
    return enterFunction(ast, /*enterName*/ false);
}

bool ScanFunctions::visit(ClassExpression *ast)
{
    enterEnvironment(ast, ContextType::Block, QStringLiteral("%Class"));
    _context->isStrict = true;
    _context->hasNestedFunctions = true;
    if (!ast->name.isEmpty())
        _context->addLocalVar(ast->name.toString(), Context::VariableDefinition, AST::VariableScope::Const);
    return true;
}

void ScanFunctions::endVisit(ClassExpression *)
{
    leaveEnvironment();
}

bool ScanFunctions::visit(ClassDeclaration *ast)
{
    if (!ast->name.isEmpty())
        _context->addLocalVar(ast->name.toString(), Context::VariableDeclaration, AST::VariableScope::Let);

    enterEnvironment(ast, ContextType::Block, QStringLiteral("%Class"));
    _context->isStrict = true;
    _context->hasNestedFunctions = true;
    if (!ast->name.isEmpty())
        _context->addLocalVar(ast->name.toString(), Context::VariableDefinition, AST::VariableScope::Const);
    return true;
}

void ScanFunctions::endVisit(ClassDeclaration *)
{
    leaveEnvironment();
}

bool ScanFunctions::visit(TemplateLiteral *ast)
{
    while (ast) {
        if (ast->expression)
            Node::accept(ast->expression, this);
        ast = ast->next;
    }
    return true;

}

bool ScanFunctions::enterFunction(FunctionExpression *ast, bool enterName)
{
    if (_context->isStrict && (ast->name == QLatin1String("eval") || ast->name == QLatin1String("arguments")))
        _cg->throwSyntaxError(ast->identifierToken, QStringLiteral("Function name may not be eval or arguments in strict mode"));
    return enterFunction(ast, ast->name.toString(), ast->formals, ast->body, enterName);
}

void ScanFunctions::endVisit(FunctionExpression *)
{
    leaveEnvironment();
}

bool ScanFunctions::visit(ObjectPattern *ast)
{
    TemporaryBoolAssignment allowFuncDecls(_allowFuncDecls, true);
    Node::accept(ast->properties, this);
    return false;
}

bool ScanFunctions::visit(PatternProperty *ast)
{
    Q_UNUSED(ast);
    // ### Shouldn't be required anymore
//    if (ast->type == PatternProperty::Getter || ast->type == PatternProperty::Setter) {
//        TemporaryBoolAssignment allowFuncDecls(_allowFuncDecls, true);
//        return enterFunction(ast, QString(), ast->formals, ast->functionBody, /*enterName */ false);
//    }
    return true;
}

void ScanFunctions::endVisit(PatternProperty *)
{
    // ###
//    if (ast->type == PatternProperty::Getter || ast->type == PatternProperty::Setter)
//        leaveEnvironment();
}

bool ScanFunctions::visit(FunctionDeclaration *ast)
{
    return enterFunction(ast, /*enterName*/ true);
}

void ScanFunctions::endVisit(FunctionDeclaration *)
{
    leaveEnvironment();
}

bool ScanFunctions::visit(DoWhileStatement *ast) {
    {
        TemporaryBoolAssignment allowFuncDecls(_allowFuncDecls, !_context->isStrict);
        Node::accept(ast->statement, this);
    }
    Node::accept(ast->expression, this);
    return false;
}

bool ScanFunctions::visit(ForStatement *ast) {
    enterEnvironment(ast, ContextType::Block, QStringLiteral("%For"));
    Node::accept(ast->initialiser, this);
    Node::accept(ast->declarations, this);
    Node::accept(ast->condition, this);
    Node::accept(ast->expression, this);

    TemporaryBoolAssignment allowFuncDecls(_allowFuncDecls, !_context->isStrict);
    Node::accept(ast->statement, this);

    return false;
}

void ScanFunctions::endVisit(ForStatement *)
{
    leaveEnvironment();
}

bool ScanFunctions::visit(ForEachStatement *ast) {
    enterEnvironment(ast, ContextType::Block, QStringLiteral("%Foreach"));
    Node::accept(ast->lhs, this);
    Node::accept(ast->expression, this);

    TemporaryBoolAssignment allowFuncDecls(_allowFuncDecls, !_context->isStrict);
    Node::accept(ast->statement, this);

    return false;
}

void ScanFunctions::endVisit(ForEachStatement *)
{
    leaveEnvironment();
}

bool ScanFunctions::visit(ThisExpression *)
{
    _context->usesThis = true;
    return false;
}

bool ScanFunctions::visit(Block *ast)
{
    TemporaryBoolAssignment allowFuncDecls(_allowFuncDecls, _context->isStrict ? false : _allowFuncDecls);
    enterEnvironment(ast, ContextType::Block, QStringLiteral("%Block"));
    Node::accept(ast->statements, this);
    return false;
}

void ScanFunctions::endVisit(Block *)
{
    leaveEnvironment();
}

bool ScanFunctions::visit(CaseBlock *ast)
{
    enterEnvironment(ast, ContextType::Block, QStringLiteral("%CaseBlock"));
    return true;
}

void ScanFunctions::endVisit(CaseBlock *)
{
    leaveEnvironment();
}

bool ScanFunctions::visit(Catch *ast)
{
    TemporaryBoolAssignment allowFuncDecls(_allowFuncDecls, _context->isStrict ? false : _allowFuncDecls);
    enterEnvironment(ast, ContextType::Block, QStringLiteral("%CatchBlock"));
    _context->isCatchBlock = true;
    QString caughtVar = ast->patternElement->bindingIdentifier.toString();
    if (caughtVar.isEmpty())
        caughtVar = QStringLiteral("@caught");
    _context->addLocalVar(caughtVar, Context::MemberType::VariableDefinition, VariableScope::Let);

    _context->caughtVariable = caughtVar;
    if (_context->isStrict &&
        (caughtVar == QLatin1String("eval") || caughtVar == QLatin1String("arguments"))) {
        _cg->throwSyntaxError(ast->identifierToken, QStringLiteral("Catch variable name may not be eval or arguments in strict mode"));
        return false;
    }
    Node::accept(ast->patternElement, this);
    // skip the block statement
    Node::accept(ast->statement->statements, this);
    return false;
}

void ScanFunctions::endVisit(Catch *)
{
    leaveEnvironment();
}

bool ScanFunctions::visit(WithStatement *ast)
{
    Node::accept(ast->expression, this);

    TemporaryBoolAssignment allowFuncDecls(_allowFuncDecls, _context->isStrict ? false : _allowFuncDecls);
    enterEnvironment(ast, ContextType::Block, QStringLiteral("%WithBlock"));
    _context->isWithBlock = true;

    if (_context->isStrict) {
        _cg->throwSyntaxError(ast->withToken, QStringLiteral("'with' statement is not allowed in strict mode"));
        return false;
    }
    Node::accept(ast->statement, this);

    return false;
}

void ScanFunctions::endVisit(WithStatement *)
{
    leaveEnvironment();
}

bool ScanFunctions::enterFunction(Node *ast, const QString &name, FormalParameterList *formals, StatementList *body, bool enterName)
{
    Context *outerContext = _context;
    enterEnvironment(ast, ContextType::Function, name);

    FunctionExpression *expr = AST::cast<FunctionExpression *>(ast);
    if (!expr)
        expr = AST::cast<FunctionDeclaration *>(ast);
    if (outerContext) {
        outerContext->hasNestedFunctions = true;
        // The identifier of a function expression cannot be referenced from the enclosing environment.
        if (enterName) {
            if (!outerContext->addLocalVar(name, Context::FunctionDefinition, VariableScope::Var, expr)) {
                _cg->throwSyntaxError(ast->firstSourceLocation(), QStringLiteral("Identifier %1 has already been declared").arg(name));
                return false;
            }
            outerContext->addLocalVar(name, Context::FunctionDefinition, VariableScope::Var, expr);
        }
        if (name == QLatin1String("arguments"))
            outerContext->usesArgumentsObject = Context::ArgumentsObjectNotUsed;
    }

    _context->name = name;
    if (formals && formals->containsName(QStringLiteral("arguments")))
        _context->usesArgumentsObject = Context::ArgumentsObjectNotUsed;
    if (expr) {
        if (expr->isArrowFunction)
            _context->isArrowFunction = true;
        else if (expr->isGenerator)
            _context->isGenerator = true;
    }


    if (!name.isEmpty() && (!formals || !formals->containsName(name)))
        _context->addLocalVar(name, Context::ThisFunctionName, VariableScope::Var);
    _context->formals = formals;

    if (body && !_context->isStrict)
        checkDirectivePrologue(body);

    bool isSimpleParameterList = formals && formals->isSimpleParameterList();

    _context->arguments = formals ? formals->formals() : QStringList();

    const QStringList boundNames = formals ? formals->boundNames() : QStringList();
    for (int i = 0; i < boundNames.size(); ++i) {
        const QString &arg = boundNames.at(i);
        if (_context->isStrict || !isSimpleParameterList) {
            bool duplicate = (boundNames.indexOf(arg, i + 1) != -1);
            if (duplicate) {
                _cg->throwSyntaxError(formals->firstSourceLocation(), QStringLiteral("Duplicate parameter name '%1' is not allowed.").arg(arg));
                return false;
            }
        }
        if (_context->isStrict) {
            if (arg == QLatin1String("eval") || arg == QLatin1String("arguments")) {
                _cg->throwSyntaxError(formals->firstSourceLocation(), QStringLiteral("'%1' cannot be used as parameter name in strict mode").arg(arg));
                return false;
            }
        }
        if (!_context->arguments.contains(arg))
            _context->addLocalVar(arg, Context::VariableDefinition, VariableScope::Var);
    }
    return true;
}

void ScanFunctions::calcEscapingVariables()
{
    Module *m = _cg->_module;

    for (Context *inner : qAsConst(m->contextMap)) {
        if (inner->contextType == ContextType::Block && inner->usesArgumentsObject == Context::ArgumentsObjectUsed) {
            Context *c = inner->parent;
            while (c->contextType == ContextType::Block)
                c = c->parent;
            c->usesArgumentsObject = Context::ArgumentsObjectUsed;
            inner->usesArgumentsObject = Context::ArgumentsObjectNotUsed;
        }
    }
    for (Context *inner : qAsConst(m->contextMap)) {
        if (!inner->parent || inner->usesArgumentsObject == Context::ArgumentsObjectUnknown)
            inner->usesArgumentsObject = Context::ArgumentsObjectNotUsed;
        if (inner->usesArgumentsObject == Context::ArgumentsObjectUsed) {
            QString arguments = QStringLiteral("arguments");
            inner->addLocalVar(arguments, Context::VariableDeclaration, AST::VariableScope::Var);
            if (!inner->isStrict) {
                inner->argumentsCanEscape = true;
                inner->requiresExecutionContext = true;
            }
        }
    }

    for (Context *inner : qAsConst(m->contextMap)) {
        for (const QString &var : qAsConst(inner->usedVariables)) {
            Context *c = inner;
            while (c) {
                Context *current = c;
                c = c->parent;
                if (current->isWithBlock || current->contextType != ContextType::Block)
                    break;
            }
            Q_ASSERT(c != inner);
            while (c) {
                Context::MemberMap::const_iterator it = c->members.find(var);
                if (it != c->members.end()) {
                    if (c->parent || it->isLexicallyScoped()) {
                        it->canEscape = true;
                        c->requiresExecutionContext = true;
                    }
                    break;
                }
                if (c->findArgument(var) != -1) {
                    c->argumentsCanEscape = true;
                    c->requiresExecutionContext = true;
                    break;
                }
                c = c->parent;
            }
        }
        if (inner->hasDirectEval) {
            inner->hasDirectEval = false;
            if (!inner->isStrict) {
                Context *c = inner;
                while (c->contextType == ContextType::Block) {
                    c = c->parent;
                }
                Q_ASSERT(c);
                c->hasDirectEval = true;
            }
            Context *c = inner;
            while (c) {
                c->allVarsEscape = true;
                c = c->parent;
            }
        }
        if (inner->usesThis) {
            inner->usesThis = false;
            if (!inner->isStrict) {
                Context *c = inner;
                while (c->contextType == ContextType::Block) {
                    c = c->parent;
                }
                Q_ASSERT(c);
                c->usesThis = true;
            }
        }
    }
    for (Context *c : qAsConst(m->contextMap)) {
        if (c->allVarsEscape && c->contextType == ContextType::Block && c->members.isEmpty())
            c->allVarsEscape = false;
        if (c->contextType == ContextType::Global || (!c->isStrict && c->contextType == ContextType::Eval) || m->debugMode)
            c->allVarsEscape = true;
        if (c->allVarsEscape) {
            if (c->parent) {
                c->requiresExecutionContext = true;
                c->argumentsCanEscape = true;
            } else {
                for (const auto &m : qAsConst(c->members)) {
                    if (m.isLexicallyScoped()) {
                        c->requiresExecutionContext = true;
                        break;
                    }
                }
            }
        }
        if (c->contextType == ContextType::Block && c->isCatchBlock) {
            c->requiresExecutionContext = true;
            auto m = c->members.find(c->caughtVariable);
            m->canEscape = true;
        }
        const QLatin1String exprForOn("expression for on");
        if (c->contextType == ContextType::Binding && c->name.length() > exprForOn.size() &&
            c->name.startsWith(exprForOn) && c->name.at(exprForOn.size()).isUpper())
            // we don't really need this for bindings, but we do for signal handlers, and in this case,
            // we don't know if the code is a signal handler or not.
            c->requiresExecutionContext = true;
        if (c->allVarsEscape) {
            for (auto &m : c->members)
                m.canEscape = true;
        }
    }

    static const bool showEscapingVars = qEnvironmentVariableIsSet("QV4_SHOW_ESCAPING_VARS");
    if (showEscapingVars) {
        qDebug() << "==== escaping variables ====";
        for (Context *c : qAsConst(m->contextMap)) {
            qDebug() << "Context" << c << c->name << "requiresExecutionContext" << c->requiresExecutionContext << "isStrict" << c->isStrict;
            qDebug() << "    parent:" << c->parent;
            if (c->argumentsCanEscape)
                qDebug() << "    Arguments escape";
            for (auto it = c->members.constBegin(); it != c->members.constEnd(); ++it) {
                qDebug() << "    " << it.key() << it.value().index << it.value().canEscape << "isLexicallyScoped:" << it.value().isLexicallyScoped();
            }
        }
    }
}