#include "RParserProject.h"
#include "QueryMessage.h"
#include "RTagsPlugin.h"
#include "Server.h"
#include "SourceInformation.h"
#include <rct/Connection.h>
#include <rct/Log.h>
#include <rct/StopWatch.h>

#include <QMutexLocker>

#include <cpppreprocessor.h>
#include <searchsymbols.h>
#include <symbolfinder.h>
#include <ASTPath.h>
#include <DependencyTable.h>
#include <cplusplus/SymbolVisitor.h>
#include <typeinfo>
#include <cxxabi.h>


using namespace CppTools;
using namespace CppTools::Internal;

static CPlusPlus::Overview overview;

static inline String fromQString(const QString& str)
{
    const QByteArray& utf8 = str.toUtf8();
    return String(utf8.constData(), utf8.size());
}

static inline String symbolName(const CPlusPlus::Symbol* symbol)
{
    String symbolName = fromQString(overview.prettyName(symbol->name()));
    // if (symbolName.isEmpty()) {
    //     String type;
    //     if (symbol->isNamespace()) {
    //         type = "namespace";
    //     } else if (symbol->isEnum()) {
    //         type = "enum";
    //     } else if (const CPlusPlus::Class *c = symbol->asClass())  {
    //         if (c->isUnion())
    //             type = "union";
    //         else if (c->isStruct())
    //             type = "struct";
    //         else
    //             type = "class";
    //     } else {
    //         type = "symbol";
    //     }
    //     symbolName = "<anonymous ";
    //     symbolName += type;
    //     symbolName += '>';
    // }
    return symbolName;
}

class RParserJob
{
public:
    RParserJob(const SourceInformation& i)
        : info(i)
    {
    }

    Path fileName() const { return info.sourceFile; }

    SourceInformation info;
};

class ReallyFindScopeAt: protected CPlusPlus::SymbolVisitor
{
    CPlusPlus::TranslationUnit *_unit;
    unsigned _line;
    unsigned _column;
    CPlusPlus::Scope *_scope;
    unsigned _foundStart;
    unsigned _foundEnd;

public:
    /** line and column should be 1-based */
    ReallyFindScopeAt(CPlusPlus::TranslationUnit *unit, unsigned line, unsigned column)
        : _unit(unit), _line(line), _column(column), _scope(0),
          _foundStart(0), _foundEnd(0)
    {
    }

    CPlusPlus::Scope *operator()(CPlusPlus::Symbol *symbol)
    {
        accept(symbol);
        return _scope;
    }

protected:
    bool process(CPlusPlus::Scope *symbol)
    {
        CPlusPlus::Scope *scope = symbol;

        for (unsigned i = 0; i < scope->memberCount(); ++i) {
            accept(scope->memberAt(i));
        }

        unsigned startLine, startColumn;
        _unit->getPosition(scope->startOffset(), &startLine, &startColumn);

        if (_line > startLine || (_line == startLine && _column >= startColumn)) {
            unsigned endLine, endColumn;
            _unit->getPosition(scope->endOffset(), &endLine, &endColumn);

            if (_line < endLine || (_line == endLine && _column < endColumn)) {
                if (!_scope || (scope->startOffset() >= _foundStart &&
                                scope->endOffset() <= _foundEnd)) {
                    _foundStart = scope->startOffset();
                    _foundEnd = scope->endOffset();
                    _scope = scope;
                }
            }
        }

        return false;
    }

    using CPlusPlus::SymbolVisitor::visit;

    virtual bool visit(CPlusPlus::UsingNamespaceDirective *) { return false; }
    virtual bool visit(CPlusPlus::UsingDeclaration *) { return false; }
    virtual bool visit(CPlusPlus::NamespaceAlias *) { return false; }
    virtual bool visit(CPlusPlus::Declaration *) { return false; }
    virtual bool visit(CPlusPlus::Argument *) { return false; }
    virtual bool visit(CPlusPlus::TypenameArgument *) { return false; }
    virtual bool visit(CPlusPlus::BaseClass *) { return false; }
    virtual bool visit(CPlusPlus::ForwardClassDeclaration *) { return false; }

    virtual bool visit(CPlusPlus::Enum *symbol)
    { return process(symbol); }

    virtual bool visit(CPlusPlus::Function *symbol)
    { return process(symbol); }

    virtual bool visit(CPlusPlus::Namespace *symbol)
    { return process(symbol); }

    virtual bool visit(CPlusPlus::Class *symbol)
    { return process(symbol); }

    virtual bool visit(CPlusPlus::Block *symbol)
    { return process(symbol); }

    // Objective-C
    virtual bool visit(CPlusPlus::ObjCBaseClass *) { return false; }
    virtual bool visit(CPlusPlus::ObjCBaseProtocol *) { return false; }
    virtual bool visit(CPlusPlus::ObjCForwardClassDeclaration *) { return false; }
    virtual bool visit(CPlusPlus::ObjCForwardProtocolDeclaration *) { return false; }
    virtual bool visit(CPlusPlus::ObjCPropertyDeclaration *) { return false; }

    virtual bool visit(CPlusPlus::ObjCClass *symbol)
    { return process(symbol); }

    virtual bool visit(CPlusPlus::ObjCProtocol *symbol)
    { return process(symbol); }

    virtual bool visit(CPlusPlus::ObjCMethod *symbol)
    { return process(symbol); }
};

class FindSymbols : public CPlusPlus::SymbolVisitor
{
public:
    enum Mode { Cursors, ListSymbols };

    FindSymbols(Mode m) : mode(m) { }

    bool preVisit(CPlusPlus::Symbol* symbol);

    void operator()(CPlusPlus::Symbol* symbol);

    Set<CPlusPlus::Symbol*> symbols() { return syms; }
    Map<String, RParserProject::RParserName> symbolNames() { return names; };

private:
    Mode mode;
    Set<CPlusPlus::Symbol*> syms;
    Map<String, RParserProject::RParserName> names;
};

void RParserProject::RParserName::merge(const RParserName& other)
{
    paths += other.paths;
    names += other.names;
}

enum QualifiedMode { All, Smart };
static inline QList<const CPlusPlus::Name*> rtagsQualified(const CPlusPlus::Symbol* symbol, QualifiedMode mode)
{
    if (mode == Smart && symbol->isNamespace())
        mode = All;
    QList<const CPlusPlus::Name*> name;
    do {
        name.prepend(symbol->name());
        symbol = symbol->enclosingScope();
    } while (symbol && (symbol->isClass() || (mode == All && symbol->isNamespace())));
    return name;
}

static String rtagsQualifiedName(const CPlusPlus::Symbol* symbol, QualifiedMode mode)
{
    return fromQString(overview.prettyName(rtagsQualified(symbol, mode)));
}

bool FindSymbols::preVisit(CPlusPlus::Symbol* symbol)
{
    if (mode == Cursors) {
        syms.insert(symbol);
    } else {
        RParserProject::RParserName cur;
        cur.paths.insert(Path(symbol->fileName()));

        QVector<int> seps;
        String symbolName;
        QList<const CPlusPlus::Name*> fullName = CPlusPlus::LookupContext::fullyQualifiedName(symbol);
        foreach(const CPlusPlus::Name* name, fullName) {
            if (!symbolName.isEmpty()) {
                symbolName.append("::");
                seps.append(symbolName.size());
            }
            symbolName.append(fromQString(overview.prettyName(name)));
        }
        if (symbolName.isEmpty())
            return true;

        cur.names.insert(symbolName);
        names[symbolName].merge(cur);

        foreach(int s, seps) {
            const String& sub = symbolName.mid(s);
            cur.names.insert(sub);
            names[sub].merge(cur);
        }
    }
    return true;
}

void FindSymbols::operator()(CPlusPlus::Symbol* symbol)
{
    syms.clear();
    names.clear();
    accept(symbol);
}

static inline bool nameMatch(CPlusPlus::Symbol* symbol, const String& name)
{
    String full;
    QList<const CPlusPlus::Name*> fullName = CPlusPlus::LookupContext::fullyQualifiedName(symbol);
    while (!fullName.isEmpty()) {
        const CPlusPlus::Name* n = fullName.takeLast();
        if (!full.isEmpty())
            full.prepend("::");
        full.prepend(fromQString(overview.prettyName(n)));
        if (full == name)
            return true;
    }
    return false;
}

static inline CPlusPlus::Symbol *canonicalSymbol(CPlusPlus::Scope *scope, const QString &code,
                                                 CPlusPlus::TypeOfExpression &typeOfExpression)
{
    const QList<CPlusPlus::LookupItem> results =
        typeOfExpression(code.toUtf8(), scope, CPlusPlus::TypeOfExpression::Preprocess);

    for (int i = results.size() - 1; i != -1; --i) {
        const CPlusPlus::LookupItem &r = results.at(i);
        CPlusPlus::Symbol *decl = r.declaration();

        if (! (decl && decl->enclosingScope()))
            break;

        if (CPlusPlus::Class *classScope = r.declaration()->enclosingScope()->asClass()) {
            const CPlusPlus::Identifier *declId = decl->identifier();
            const CPlusPlus::Identifier *classId = classScope->identifier();

            if (classId && classId->isEqualTo(declId))
                continue; // skip it, it's a ctor or a dtor.

            else if (CPlusPlus::Function *funTy = r.declaration()->type()->asFunctionType()) {
                if (funTy->isVirtual())
                    return r.declaration();
            }
        }
    }

    for (int i = 0; i < results.size(); ++i) {
        const CPlusPlus::LookupItem &r = results.at(i);

        if (r.declaration())
            return r.declaration();
    }

    return 0;
}

DocumentParser::DocumentParser(QPointer<CppModelManager> mgr,
                               RParserProject* parser,
                               QObject* parent)
    : QObject(parent), symbolCount(0), manager(mgr), rparser(parser)
{
}

DocumentParser::~DocumentParser()
{
    const CPlusPlus::Snapshot& snapshot = manager->snapshot();
    CPlusPlus::Snapshot::iterator it = snapshot.begin();
    const CPlusPlus::Snapshot::const_iterator end = snapshot.end();
    while (it != end) {
        it.value()->releaseSourceAndAST();
        ++it;
    }
}

QByteArray DocumentParser::debugScope(CPlusPlus::Scope* scope, const QByteArray& src)
{
    return src.mid(scope->startOffset(), scope->endOffset() - scope->startOffset());
}

static inline QList<CPlusPlus::Usage> findUsages(QPointer<CppModelManager> manager,
                                                 CPlusPlus::Symbol* symbol)
{
    const CPlusPlus::Identifier *symbolId = symbol->identifier();
    if (!symbolId) {
        error("no symbol id in findUsages");
        return QList<CPlusPlus::Usage>();
    }

    const CPlusPlus::Snapshot& snapshot = manager->snapshot();

    QList<CPlusPlus::Usage> usages;

    // ### Use QFuture for this?

    CPlusPlus::Snapshot::const_iterator snap = snapshot.begin();
    const CPlusPlus::Snapshot::const_iterator end = snapshot.end();
    while (snap != end) {
        CPlusPlus::Document::Ptr doc = snap.value();
        const CPlusPlus::Control* control = doc->control();
        if (control->findIdentifier(symbolId->chars(), symbolId->size())) {
            CPlusPlus::LookupContext lookup(doc, snapshot);
            CPlusPlus::FindUsages find(lookup);
            find(symbol);
            usages.append(find.usages());
        }
        ++snap;
    }

    return usages;
}

void DocumentParser::onDocumentUpdated(CPlusPlus::Document::Ptr doc)
{
    // seems I need to keep this around
    doc->keepSourceAndAST();

    // message out any diagnostics
    const QList<CPlusPlus::Document::DiagnosticMessage> diags = doc->diagnosticMessages();
    foreach(const CPlusPlus::Document::DiagnosticMessage& msg, diags) {
        warning("%s:%d:%d: %s", msg.fileName().toUtf8().constData(), msg.line(), msg.column(), msg.text().toUtf8().constData());
    }

    {
        QList<CPlusPlus::Document::Include> includes = doc->includes();
        if (!includes.isEmpty()) {
            QString srcFile = doc->fileName();
            QMutexLocker locker(&rparser->mutex);
            foreach(CPlusPlus::Document::Include include, includes) {
                rparser->headerToSource[include.fileName()] = srcFile;
            }
        }
    }

    const QFileInfo info(doc->fileName());
    const QString canonical = info.canonicalFilePath();

    CPlusPlus::TranslationUnit *translationUnit = doc->translationUnit();
    CPlusPlus::Parser parser(translationUnit);
    CPlusPlus::Namespace *globalNamespace = doc->globalNamespace();
    CPlusPlus::Bind bind(translationUnit);
    if (!translationUnit->ast())
        return; // nothing to do.

    if (CPlusPlus::TranslationUnitAST *ast = translationUnit->ast()->asTranslationUnit())
        bind(ast, globalNamespace);
    else if (CPlusPlus::StatementAST *ast = translationUnit->ast()->asStatement())
        bind(ast, globalNamespace);
    else if (CPlusPlus::ExpressionAST *ast = translationUnit->ast()->asExpression())
        bind(ast, globalNamespace);
    else if (CPlusPlus::DeclarationAST *ast = translationUnit->ast()->asDeclaration())
        bind(ast, globalNamespace);
    //error("bound %s with mgr %p", qPrintable(doc->fileName()), manager.data());
}

class RParserUnit
{
public:
    SourceInformation info;

    static QHash<QString, CPlusPlus::Document::Ptr> defineDocs;
    static CPlusPlus::Document::Ptr defineDocument(QPointer<CppModelManager> manager, const QString& name, const QStringList& defines);

    void reindex(QPointer<CppModelManager> manager);
};

QHash<QString, CPlusPlus::Document::Ptr> RParserUnit::defineDocs;

CPlusPlus::Document::Ptr RParserUnit::defineDocument(QPointer<CppModelManager> manager, const QString& name, const QStringList& defines)
{
    QHash<QString, CPlusPlus::Document::Ptr>::const_iterator defs = defineDocs.find(defines.join(QLatin1String(":")));
    if (defs != defineDocs.end())
        return defs.value();
    QString defsrc;
    foreach(const QString& def, defines) {
        const int eq = def.indexOf(QLatin1Char('='));
        if (eq == -1)
            defsrc += QLatin1String("#define ") + def;
        else
            defsrc += QLatin1String("#define ") + def.left(eq) + QLatin1Char(' ') + def.mid(eq + 1);
        defsrc += QLatin1Char('\n');
    }
    const CPlusPlus::Snapshot& snapshot = manager->snapshot();
    CPlusPlus::Document::Ptr doc = snapshot.preprocessedDocument(defsrc, QString("<rparserdefines_%1>").arg(name));
    assert(doc);
    defineDocs[defines.join(QLatin1String(":"))] = doc;
    return doc;
}

template<typename T>
static inline QStringList toQStringList(const T& t)
{
    QStringList list;
    typename T::const_iterator it = t.begin();
    const typename T::const_iterator end = t.end();
    while (it != end) {
        list << QString::fromStdString(*it);
        ++it;
    }
    return list;
}

void RParserUnit::reindex(QPointer<CppModelManager> manager)
{
    CppPreprocessor preprocessor(manager);

    const QString srcFile = QString::fromStdString(info.sourceFile);
    const QString srcPath = QString::fromStdString(info.sourceFile.parentDir());
    QList<CPlusPlus::Document::Include> includes;
    {
        CPlusPlus::Document::Ptr doc = manager->document(srcFile);
        if (doc)
            includes = doc->includes();
    }
    const bool hasIncludes = !includes.isEmpty();

#warning grab the include paths and defines from the system compiler here
    static QStringList globalDefines;
    if (globalDefines.isEmpty())
        globalDefines << QLatin1String("__GNUC__=4");

    static QStringList incs = QStringList()
        << QLatin1String("/usr/include")
        << QLatin1String("/usr/include/c++/4.6")
        << QLatin1String("/usr/lib/gcc/i686-linux-gnu/4.6/include")
        << QLatin1String("/usr/include/i386-linux-gnu")
        << srcPath;
    List<SourceInformation::Build>::const_iterator build = info.builds.begin();
    const List<SourceInformation::Build>::const_iterator end = info.builds.end();
    while (build != end) {
        //error() << "reindexing" << info.sourceFile << build->includePaths << build->defines;
        preprocessor.removeFromCache(srcFile);
        if (hasIncludes) {
            foreach(const CPlusPlus::Document::Include& include, includes) {
                preprocessor.removeFromCache(include.fileName());
            }
        }

        preprocessor.mergeEnvironment(defineDocument(manager, srcFile, toQStringList(build->defines) + globalDefines));
        preprocessor.setIncludePaths(toQStringList(build->includePaths) + incs);
        preprocessor.run(srcFile);
        preprocessor.resetEnvironment();
        ++build;
    }
}

RParserUnit* RParserProject::findUnit(const Path& path)
{
    Map<Path, RParserUnit*>::const_iterator unit = units.find(path);
    if (unit == units.end())
        return 0;
    return unit->second;
}

static inline Project::Cursor::Kind symbolKind(const CPlusPlus::Symbol* sym)
{
    if (sym->asEnum()) {
        //error("enum");
        return Project::Cursor::Enum;
    } else if (sym->asFunction()) {
        //error("function");
        return Project::Cursor::MemberFunctionDeclaration;
    } else if (sym->asNamespace()) {
        //error("namespace");
        return Project::Cursor::Namespace;
    } else if (sym->asTemplate()) {
        //error("template");
    } else if (sym->asNamespaceAlias()) {
        //error("namespaceAlias");
    } else if (sym->asForwardClassDeclaration()) {
        //error("forward class");
        return Project::Cursor::Class;
    } else if (sym->asClass()) {
        //error("class");
        return Project::Cursor::Class;
    } else if (sym->asUsingNamespaceDirective()) {
        //error("using 1");
    } else if (sym->asUsingDeclaration()) {
        //error("using 2");
    } else if (sym->asDeclaration()) {
        //error("decl");
        return Project::Cursor::Variable; // ### ???
    } else if (sym->asArgument()) {
        //error("arg");
        return Project::Cursor::Variable;
    } else if (sym->asTypenameArgument()) {
        //error("typename");
    } else if (sym->asBaseClass()) {
        //error("baseclass");
    } else if (sym->asQtPropertyDeclaration()) {
    } else if (sym->asQtEnum()){
    } else if (sym->asObjCBaseClass()) {
    } else if (sym->asObjCBaseProtocol()) {
    } else if (sym->asObjCClass()) {
    } else if (sym->asObjCForwardClassDeclaration()) {
    } else if (sym->asObjCProtocol()) {
    } else if (sym->asObjCForwardProtocolDeclaration()) {
    } else if (sym->asObjCMethod()) {
    } else if (sym->asObjCPropertyDeclaration()) {
    }
    return Project::Cursor::Invalid;
}

static inline Location makeLocation(CPlusPlus::Symbol* sym)
{
    const uint32_t fileId = Location::insertFile(Path::resolved(sym->fileName()));
    return Location(fileId, sym->line(), sym->column());
}

static inline Project::Cursor makeCursor(const CPlusPlus::Symbol* sym,
                                          const CPlusPlus::TranslationUnit* unit)
{
    Project::Cursor cursor;
    const uint32_t fileId = Location::insertFile(Path::resolved(sym->fileName()));
    cursor.location = Location(fileId, sym->line(), sym->column());
    const CPlusPlus::Token& token = unit->tokenAt(sym->sourceLocation());
    cursor.start = token.begin();
    cursor.end = token.end();
    cursor.kind = symbolKind(sym);
    cursor.symbolName = rtagsQualifiedName(sym, Smart);
    return cursor;
}

CPlusPlus::Symbol* RParserProject::findSymbol(CPlusPlus::Document::Ptr doc,
                                               const Location& srcLoc,
                                               FindSymbolMode mode,
                                               const QByteArray& src,
                                               CPlusPlus::LookupContext& lookup,
                                               Location& loc) const
{
    const unsigned line = srcLoc.line();
    const unsigned column = srcLoc.column();

    // First, try to find the symbol outright:
    CPlusPlus::Symbol* sym = 0;
    {
        CPlusPlus::Symbol* candidate = doc->lastVisibleSymbolAt(line, column);
        if (candidate) {
            const CPlusPlus::Identifier* id = candidate->identifier();
            if (id) {
                // ### fryktelig
                if (candidate->line() == line && candidate->column() <= column &&
                    candidate->column() + id->size() >= column) {
                    // yes
                    sym = candidate;
                    loc = makeLocation(sym);
                    debug("found outright");
                }
            }
        }
        // no
    }

    if (!sym) {
        // See if we can parse it:
        CPlusPlus::TypeOfExpression typeofExpression;
        typeofExpression.init(doc, manager->snapshot(), lookup.bindings());
        typeofExpression.setExpandTemplates(true);

        CPlusPlus::TranslationUnit* unit = doc->translationUnit();
        ReallyFindScopeAt really(unit, line, column);
        CPlusPlus::Scope* scope = really(doc->globalNamespace());
        if (!scope)
            scope = doc->globalNamespace();

        CPlusPlus::ASTPath path(doc);
        QList<CPlusPlus::AST*> asts = path(line, column);
        while (!asts.isEmpty()) {
            CPlusPlus::AST* ast = asts.takeLast();

            const int startIndex = ast->firstToken();
            int endIndex = ast->lastToken() - 1;
            while (endIndex >= 0) {
                unsigned el, ec;
                unit->getTokenStartPosition(endIndex, &el, &ec, 0);
                if (el < line || (el == line && ec <= column))
                    break;
                --endIndex;
            }

            assert(startIndex <= endIndex && endIndex >= 0);

            if (startIndex > 0) {
                // check if our previous token is an accessor token
                bool ok = true;
                const CPlusPlus::Token& prev = unit->tokenAt(startIndex - 1);
                switch (prev.kind()) {
                case CPlusPlus::T_COLON_COLON:
                case CPlusPlus::T_DOT:
                case CPlusPlus::T_ARROW:
                case CPlusPlus::T_DOT_STAR:
                case CPlusPlus::T_ARROW_STAR:
                    // yes, we need to look at our next AST
                    ok = false;
                    break;
                default:
                    break;
                }
                if (!ok)
                    continue;
            }
            const CPlusPlus::Token& start = unit->tokenAt(startIndex);
            const CPlusPlus::Token& last = unit->tokenAt(endIndex);
            const QByteArray expression = src.mid(start.begin(), last.end() - start.begin());

            debug("trying expr '%.40s' in scope %p", qPrintable(expression), scope);

            sym = canonicalSymbol(scope, expression, typeofExpression);
            if (sym) {
                unsigned startLine, startColumn;
                const CPlusPlus::StringLiteral* file;
                unit->getTokenStartPosition(startIndex, &startLine, &startColumn, &file);

                //unsigned endLine, endColumn;
                //unit->getTokenEndPosition(ast->lastToken() - 1, &endLine, &endColumn, 0);
                const uint32_t fileId = Location::fileId(Path::resolved(file->chars()));
                loc = Location(fileId, startLine, startColumn);

                warning() << "got it at" << loc;
                break;
            }
        }
    }

    if (!sym)
        return 0;

    if (CPlusPlus::Function* func = sym->type()->asFunctionType()) {
        // if we find a definition that's different from the declaration then replace
        CppTools::SymbolFinder finder;
        CPlusPlus::Symbol* definition = finder.findMatchingDefinition(sym, manager->snapshot(), true);
        if (!definition) {
            definition = finder.findMatchingDefinition(sym, manager->snapshot(), false);
        }
        if (definition) {
            if (sym != definition) {
                if (mode == Definition || mode == Swap)
                    sym = definition;
            } else if (mode != Definition) {
                QList<CPlusPlus::Declaration*> decls = finder.findMatchingDeclaration(lookup, func);
                if (!decls.isEmpty()) {
                    // ### take the first one I guess?
                    sym = decls.first();
                }
            }
        }
    } else {
        // check if we are a forward class declaration
        if (CPlusPlus::ForwardClassDeclaration* fwd = sym->asForwardClassDeclaration()) {
            // we are, try to find our real declaration
            CppTools::SymbolFinder finder;
            CPlusPlus::Class* cls = finder.findMatchingClassDeclaration(fwd, manager->snapshot());
            if (cls)
                sym = cls;
        }
    }

    return sym;
}

RParserProject::RParserProject(const Path &path)
    : Project(path), state(Starting), parser(0), appargc(0), app(new QApplication(appargc, 0))
{
    start();
    moveToThread(this);
}

RParserProject::~RParserProject()
{
    Map<Path, RParserUnit*>::const_iterator unit = units.begin();
    const Map<Path, RParserUnit*>::const_iterator end = units.end();
    while (unit != end) {
        delete unit->second;
        ++unit;
    }
    delete parser;
    delete app;
}

void RParserProject::run()
{
    manager = new CppModelManager;
    parser = new DocumentParser(manager, this);
    QObject::connect(manager.data(), SIGNAL(documentUpdated(CPlusPlus::Document::Ptr)),
                     parser, SLOT(onDocumentUpdated(CPlusPlus::Document::Ptr)));

    QMutexLocker locker(&mutex);
    assert(state == Starting || state == Indexing);
    if (jobs.isEmpty())
        changeState(Idle);
    locker.unlock();

    for (;;) {
        locker.relock();
        while (jobs.isEmpty()) {
            assert(state == Idle);
            jobsAvailable.wait(&mutex);
        }

        Set<Path> indexed;
        int taken = 0;
        int localFiles;

        StopWatch timer;

        assert(!jobs.isEmpty());
        changeState(Indexing);
        while (!jobs.isEmpty()) {
            RParserJob* job = jobs.dequeue();
            ++taken;
            locker.unlock();
            processJob(job);
            locker.relock();

            CPlusPlus::Document::Ptr doc = manager->document(QString::fromStdString(job->fileName()));
            assert(doc);
            assert(!job->fileName().isEmpty());
            indexed.insert(job->fileName());
            localFiles = 1;
            QList<CPlusPlus::Document::Include> includes = doc->includes();
            foreach(const CPlusPlus::Document::Include& include, includes) {
                // ### this really shouldn't happen but it does
                if (include.fileName().isEmpty())
                    continue;
                ++localFiles;
                indexed.insert(fromQString(include.fileName()));
            }

            error("[%3d%%] %d/%d %s %s, Files: %d",
                  static_cast<int>(round(taken / static_cast<double>(jobs.size() + taken) * 100.0)),
                  taken, jobs.size() + taken, String::formatTime(time(0), String::Time).constData(),
                  job->fileName().toTilde().constData(), localFiles);
            if (jobs.isEmpty()) {
                error() << "Parsed" << taken << "files in" << timer.elapsed() << "ms";
                startSaveTimer();
            }
        }

        changeState(CollectingNames);
        assert(jobs.isEmpty());
        locker.unlock();
        collectNames(indexed);
        locker.relock();

        if (!jobs.isEmpty()) {
            locker.unlock();
            continue;
        }

        changeState(Idle);
        locker.unlock();
    }
}

static inline const char* stateName(RParserProject::State st)
{
    struct states {
        RParserProject::State state;
        const char* name;
    } static s[] = { { RParserProject::Starting, "starting" },
                     { RParserProject::Indexing, "indexing" },
                     { RParserProject::CollectingNames, "collectingnames" },
                     { RParserProject::Idle, "idle" } };
    for (unsigned int i = 0; i < sizeof(s); ++i) {
        if (s[i].state == st)
            return s[i].name;
    }
    return 0;
}

// needs to be called with mutex locked
void RParserProject::changeState(State st)
{
    if (state == st)
        return;
    warning() << "rparser thread state changed from " << stateName(state) << " to " << stateName(st);
    state = st;
    wait.wakeAll();
}

// needs to be called with mutex locked
void RParserProject::waitForState(WaitMode m, State st) const
{
    for (;;) {
        if (m == GreaterOrEqual && state >= st)
            break;
        if (m == Equal && state == st)
            break;
        wait.wait(&mutex);
    }
}

void RParserProject::status(const String &query, Connection *conn, unsigned queryFlags) const
{
}

class DumpAST : public CPlusPlus::ASTVisitor
{
public:
    DumpAST(CPlusPlus::TranslationUnit *unit, Connection *conn)
        : CPlusPlus::ASTVisitor(unit), mDepth(0), mConn(conn)
    {}

protected:
    virtual bool preVisit(CPlusPlus::AST *ast)
    {
        const char *id = typeid(*ast).name();
        char *cppId = abi::__cxa_demangle(id, 0, 0, 0);
        id = cppId;
        String fill(mDepth * 2, ' ');
        String context;
        for (unsigned idx = ast->firstToken(); idx<ast->lastToken(); ++idx) {
            const char *str = spell(idx);
            if (!context.isEmpty()) {
                char last = context.last();
                if (last == ',') {
                    context += ' ';
                } else if (isalnum(last) && isalnum(*str)) {
                    context += ' ';
                } else if (*str == '{' || *str == '}') {
                    context += ' ';
                }
            }
            context.append(str);
        }

        mConn->write<128>("%s%s: %s", fill.constData(), id, context.constData());
        free(cppId);
        ++mDepth;
        return true;
    }

    void postVisit(CPlusPlus::AST *)
    {
        --mDepth;
    }
private:
    int mDepth;
    Connection *mConn;
};


void RParserProject::dump(const SourceInformation &sourceInformation, Connection *conn) const
{
    CPlusPlus::Document::Ptr doc = manager->document(QString::fromStdString(sourceInformation.sourceFile));
    if (!doc) {
        conn->write<64>("Don't seem to have %s indexed", sourceInformation.sourceFile.constData());
        return;
    }
    DumpAST dump(doc->translationUnit(), conn);
    dump.accept(doc->translationUnit()->ast());
}

void RParserProject::processJob(RParserJob* job)
{
    const Path& fileName = job->info.sourceFile;
    //error() << "  indexing" << fileName;
    RParserUnit* unit = findUnit(fileName);
    if (!unit) {
        unit = new RParserUnit;
        unit->info = job->info;
        units[fileName] = unit;
    }
    unit->reindex(manager);
}

void RParserProject::dirty(const Set<Path>& files)
{
#warning implement me
}

inline void RParserProject::dirtyFiles(const Set<Path>& files)
{
    Map<String, RParserName>::iterator name = names.begin();
    while (name != names.end()) {
        if ((name->second.paths - files).isEmpty())
            names.erase(name++);
        else
            ++name;
    }
}

void RParserProject::mergeNames(const Map<String, RParserName>& lnames)
{
    Map<String, RParserName>::const_iterator name = lnames.begin();
    const Map<String, RParserName>::const_iterator end = lnames.end();
    while (name != end) {
        names[name->first].merge(name->second);
        ++name;
    }
}

void RParserProject::collectNames(const Set<Path>& files)
{
    dirtyFiles(files);

    Set<Path>::const_iterator file = files.begin();
    const Set<Path>::const_iterator end = files.end();
    while (file != end) {
        CPlusPlus::Document::Ptr doc = manager->document(QString::fromStdString(*file));
        if (!doc) {
            error() << "No document for" << *file << "in collectNames";
            ++file;
            continue;
        }

        CPlusPlus::Namespace* globalNamespace = doc->globalNamespace();
        if (globalNamespace) {
            FindSymbols find(FindSymbols::ListSymbols);
            find(globalNamespace);
            mergeNames(find.symbolNames());
        }

        const String fileName(file->fileName());
        RParserName rname;
        rname.names.insert(fileName);
        rname.paths.insert(*file);
        names[fileName].merge(rname);

        ++file;
    }
}

int RParserProject::symbolCount(const Path& file)
{
    CPlusPlus::Document::Ptr doc = manager->document(QString::fromStdString(file));
    if (!doc)
        return -1;
    // ### do something better here
    return doc->globalSymbolCount();
}

void RParserProject::index(const SourceInformation &sourceInformation, Type)
{
    QMutexLocker locker(&mutex);
    jobs.enqueue(new RParserJob(sourceInformation));
    state = Indexing; // ### a bit of a hack
    jobsAvailable.wakeOne();
    //waitForState(GreaterOrEqual, CollectingNames);
    //return symbolCount(sourceInformation.sourceFile);
}

Project::Cursor RParserProject::cursor(const Location &location) const
{
    QMutexLocker locker(&mutex);
    waitForState(GreaterOrEqual, CollectingNames);

    CPlusPlus::Document::Ptr doc = manager->document(QString::fromStdString(location.path()));
    if (!doc)
        return Cursor();
    const QByteArray& src = doc->utf8Source();

    Cursor cursor;

    CPlusPlus::Document::Ptr altDoc;
    {
        Map<QString, QString>::const_iterator src = headerToSource.find(QString::fromStdString(location.path()));
        if (src != headerToSource.end()) {
            altDoc = manager->document(src->second);
        }
    }

    CPlusPlus::LookupContext lookup(altDoc ? altDoc : doc, manager->snapshot());
    CPlusPlus::Symbol* sym = findSymbol(doc, location, Swap, src, lookup, cursor.location);
    if (!sym) {
        // look for includes
        QList<CPlusPlus::Document::Include> includes = doc->includes();
        foreach(const CPlusPlus::Document::Include& include, includes) {
            if (include.line() == static_cast<unsigned int>(location.line())) {
                // yes
                const uint32_t fileId = Location::insertFile(Path::resolved(fromQString(include.fileName())));
                cursor.target = cursor.location = Location(fileId, 1, 1);
                cursor.kind = Cursor::File;
                return cursor;
            }
        }
        error() << "no symbol whatsoever for" << location;
        return Cursor();
    }

    cursor.target = makeLocation(sym);
    if (cursor.location == cursor.target) {
        // declaration
        cursor.kind = symbolKind(sym);
    } else {
        // possible reference
        cursor.kind = Cursor::Reference;
    }
    cursor.symbolName = symbolName(sym);

    warning() << "got a symbol, tried" << location << "ended up with target" << cursor.target;
    return cursor;
}

void RParserProject::references(const Location& location, unsigned flags,
                                 const List<Path> &pathFilters, Connection *conn) const
{
    QMutexLocker locker(&mutex);
    waitForState(GreaterOrEqual, CollectingNames);

    CPlusPlus::Document::Ptr doc = manager->document(QString::fromStdString(location.path()));
    if (!doc)
        return;
    const QByteArray& src = doc->utf8Source();

    Cursor cursor;

    CPlusPlus::Document::Ptr altDoc;
    {
        Map<QString, QString>::const_iterator src = headerToSource.find(QString::fromStdString(location.path()));
        if (src != headerToSource.end()) {
            altDoc = manager->document(src->second);
        }
    }

    CPlusPlus::LookupContext lookup(altDoc ? altDoc : doc, manager->snapshot());
    CPlusPlus::Symbol* sym = findSymbol(doc, location, Declaration, src, lookup, cursor.location);
    if (!sym) {
        return;
    }

    QList<CPlusPlus::Usage> usages = findUsages(manager, sym);
    const bool wantContext = !(flags & QueryMessage::NoContext);
    const bool wantVirtuals = flags & QueryMessage::FindVirtuals;
    const bool wantAll = flags & QueryMessage::AllReferences;

    const Set<Path> paths = pathFilters.toSet();
    const bool pass = paths.isEmpty();

    foreach(const CPlusPlus::Usage& usage, usages) {
        if (!pass && !paths.contains(fromQString(usage.path)))
            continue;

        CPlusPlus::Document::Ptr doc = manager->document(usage.path);
        Cursor::Kind kind = Cursor::Reference;
        if (doc) {
            CPlusPlus::Symbol* refsym = doc->lastVisibleSymbolAt(usage.line, usage.col + 1);
            if (refsym
                && refsym->line() == static_cast<unsigned>(usage.line)
                && refsym->column() == static_cast<unsigned>(usage.col + 1)) {
                if (wantVirtuals && !wantAll) {
                    if (CPlusPlus::Function *funTy = refsym->type()->asFunctionType()) {
                        if (funTy->isVirtual() || funTy->isPureVirtual())
                            kind = Cursor::MemberFunctionDeclaration;
                        else
                            continue;
                    } else {
                        continue;
                    }
                } else if (wantAll) {
                    kind = symbolKind(refsym);
                } else {
                    continue;
                }
            }
        }
        if (kind == Cursor::Reference && (wantVirtuals && !wantAll))
            continue;
        //error() << "adding ref" << fromQString(usage.path) << usage.line << usage.col;
        if (wantContext) {
            conn->write<256>("%s:%d:%d %c\t%s", qPrintable(usage.path), usage.line, usage.col + 1,
                             Project::Cursor::kindToChar(kind), qPrintable(usage.lineText));
        } else {
            conn->write<256>("%s:%d:%d", qPrintable(usage.path), usage.line, usage.col + 1);
        }
    }
    conn->write("`");
}

Set<Path> RParserProject::files(int mode) const
{
    Set<Path> result;

    QMutexLocker locker(&mutex);
    waitForState(GreaterOrEqual, CollectingNames);

    const bool wantHeaders = (mode & HeaderFiles);
    const bool wantSources = (mode & SourceFiles);

    const CPlusPlus::Snapshot& snapshot = manager->snapshot();
    CPlusPlus::Snapshot::const_iterator snap = snapshot.begin();
    const CPlusPlus::Snapshot::const_iterator end = snapshot.end();
    while (snap != end) {
        CPlusPlus::Document::Ptr doc = snap.value();
        assert(doc);
        if (wantSources)
            result.insert(fromQString(doc->fileName()));
        if (wantHeaders) {
            QList<CPlusPlus::Document::Include> includes = doc->includes();
            foreach(const CPlusPlus::Document::Include& include, includes) {
                result.insert(fromQString(include.fileName()));
            }
        }

        ++snap;
    }

    return result;
}

Set<Path> RParserProject::dependencies(const Path &path, DependencyMode mode) const
{
    Set<Path> result;

    QMutexLocker locker(&mutex);
    waitForState(GreaterOrEqual, CollectingNames);

    // ### perhaps keep this around
    CPlusPlus::DependencyTable table;
    table.build(manager->snapshot());

    if (mode == DependsOnArg) {
        const QStringList deps = table.filesDependingOn(QString::fromStdString(path));
        foreach(const QString dep, deps) {
            result.insert(fromQString(dep));
        }
    } else {
        assert(mode == ArgDependsOn);
        const QString qpath = QString::fromStdString(path);

        const QHash<QString, QStringList>& t = table.dependencyTable();
        QHash<QString, QStringList>::const_iterator it = t.begin();
        const QHash<QString, QStringList>::const_iterator end = t.end();
        while (it != end) {
            if (it.value().contains(qpath))
                result.insert(fromQString(it.key()));
            ++it;
        }
    }

    return result;
}

Set<String> RParserProject::listSymbols(const String &string, const List<Path> &pathFilter) const
{
    QMutexLocker locker(&mutex);
    waitForState(GreaterOrEqual, Idle);

    Set<String> ret;
    Set<Path> paths = pathFilter.toSet();
    const bool pass = paths.isEmpty();

    Map<String, RParserName>::const_iterator name = names.lower_bound(string);
    const Map<String, RParserName>::const_iterator end = names.end();
    while (name != end && name->first.startsWith(string)) {
        if (pass || paths.intersects(name->second.paths))
            ret += name->second.names;
        ++name;
    }
    return ret;
}

Set<Project::Cursor> RParserProject::findCursors(const String &string, const List<Path> &pathFilter) const
{
    QMutexLocker locker(&mutex);
    waitForState(GreaterOrEqual, Idle);

    Set<Path> cand, paths = pathFilter.toSet();
    {
        const bool pass = paths.isEmpty();
        Map<String, RParserName>::const_iterator name = names.find(string);
        const Map<String, RParserName>::const_iterator end = names.end();
        if (name != end) {
            if (pass)
                cand += name->second.paths;
            else
                cand += paths.intersected(name->second.paths);
            ++name;
        }
    }

    Set<Cursor> cursors;
    {
        Set<Path>::const_iterator path = cand.begin();
        const Set<Path>::const_iterator end = cand.end();
        while (path != end) {
            CPlusPlus::Document::Ptr doc = manager->document(QString::fromStdString(*path));
            if (!doc) {
                error() << "No document for" << *path << "in findCursors";
                ++path;
                continue;
            }

            CPlusPlus::TranslationUnit* unit = doc->translationUnit();
            CPlusPlus::Namespace* globalNamespace = doc->globalNamespace();
            if (globalNamespace) {
                FindSymbols find(FindSymbols::Cursors);
                find(globalNamespace);
                Set<CPlusPlus::Symbol*> syms = find.symbols();
                foreach(CPlusPlus::Symbol* sym, syms) {
                    if (nameMatch(sym, string))
                        cursors.insert(makeCursor(sym, unit));
                }
            }

            if (path->endsWith(string)) { // file name, add custom target for the file
                const uint32_t fileId = Location::fileId(*path);
                Cursor fileCursor;
                fileCursor.kind = Cursor::File;
                fileCursor.location = fileCursor.target = Location(fileId, 1, 1);
                fileCursor.symbolName = *path;
                cursors.insert(fileCursor);
            }

            ++path;
        }
    }
    return cursors;
}

Set<Project::Cursor> RParserProject::cursors(const Path &path) const
{
    QMutexLocker locker(&mutex);
    waitForState(GreaterOrEqual, CollectingNames);

    CPlusPlus::Document::Ptr doc = manager->document(QString::fromStdString(path));
    if (!doc)
        return Set<Cursor>();
    Set<Cursor> cursors;

    CPlusPlus::TranslationUnit* unit = doc->translationUnit();
    CPlusPlus::Namespace* globalNamespace = doc->globalNamespace();
    if (globalNamespace) {
        FindSymbols find(FindSymbols::Cursors);
        find(globalNamespace);
        Set<CPlusPlus::Symbol*> syms = find.symbols();
        foreach(const CPlusPlus::Symbol* sym, syms) {
            if (!sym->line())
                continue;
            cursors.insert(makeCursor(sym, unit));
        }
    }

    return cursors;
}

bool RParserProject::codeCompleteAt(const Location &location, const String &source,
                                     Connection *conn)
{
    error() << "Got code complete" << location << source;
    return false;
}

String RParserProject::fixits(const Path &/*path*/) const
{
    return String();
}

bool RParserProject::isIndexing() const
{
    QMutexLocker locker(&mutex);
    return state == Indexing;
}

void RParserProject::remove(const Path &sourceFile)
{
    QMutexLocker locker(&mutex);
    waitForState(GreaterOrEqual, Idle);
    const QString qfile = QString::fromStdString(sourceFile);
    {
        CPlusPlus::Document::Ptr doc = manager->document(qfile);
        if (doc)
            doc->releaseSourceAndAST();
    }
    manager->removeFromSnapshot(qfile);
}

bool RParserProject::save(Serializer &serializer)
{
    if (!Server::saveFileIds())
        return false;
    serializer << sourceInfos();
    return true;
}

bool RParserProject::restore(Deserializer &deserializer)
{
    if (!Server::loadFileIds())
        return false;

    SourceInformationMap sources;
    deserializer >> sources;
    setSourceInfos(sources);

    SourceInformationMap::const_iterator source = sources.begin();
    const SourceInformationMap::const_iterator end = sources.end();
    while (source != end) {
        index(source->second, Restore);
        ++source;
    }

    return true;
}

class RParserProjectPlugin : public RTagsPlugin
{
public:
    virtual shared_ptr<Project> createProject(const Path &path)
    {
        return shared_ptr<Project>(new RParserProject(path));
    }
};

extern "C" RTagsPlugin* createInstance()
{
    return new RParserProjectPlugin;
}
