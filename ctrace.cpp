// ctrace — instrumenteur source-à-source pour tracer l'exécution d'un programme C.
//
//   ctrace a.c [options clang...] | gcc -x c - -o a.out
//
// Pour chaque instruction du fichier principal, le code généré affiche :
//   a.c:12    code exécuté;            // variables lues, valeurs AVANT
//             => variables modifiées, valeurs APRÈS
// ainsi que l'entrée dans chaque fonction (avec ses paramètres) et le return.
//
// Principe : Clang construit l'AST, un Rewriter insère des appels au runtime
// (ctrace_runtime.inc) sans jamais ajouter de saut de ligne dans le code
// utilisateur, de sorte que les numéros de ligne restent exacts (#line).

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Lex/Lexer.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "ctrace_runtime.inc"

#ifndef CTRACE_RESOURCE_DIR
#define CTRACE_RESOURCE_DIR ""
#endif

using namespace clang;

namespace {

// Nom du fichier tel que passé en ligne de commande (affiché dans la trace).
std::string DisplayName;

// ---------------------------------------------------------------------------
// Utilitaires texte
// ---------------------------------------------------------------------------

// Littéral C : échappe \ " ? et les caractères de contrôle.
std::string cstr(const std::string &S) {
  std::string R = "\"";
  for (unsigned char C : S) {
    switch (C) {
    case '\\': R += "\\\\"; break;
    case '"': R += "\\\""; break;
    case '?': R += "\\?"; break; // évite les trigraphes
    case '\n': R += "\\n"; break;
    case '\t': R += "\\t"; break;
    default:
      if (C < 32) { char B[8]; snprintf(B, sizeof B, "\\%03o", C); R += B; }
      else R += (char)C;
    }
  }
  return R + "\"";
}

// Compacte les blancs et tronque, pour afficher une instruction sur une ligne.
std::string oneLine(const std::string &S, size_t Max = 120) {
  std::string R;
  bool Space = false;
  for (char C : S) {
    if (C == ' ' || C == '\t' || C == '\n' || C == '\r') { Space = !R.empty(); continue; }
    if (Space) { R += ' '; Space = false; }
    R += C;
  }
  if (R.size() > Max) R = R.substr(0, Max - 3) + "...";
  return R;
}

std::string join(const std::vector<std::string> &V) {
  std::string R;
  for (const auto &S : V) R += ", " + S;
  return R;
}

// ---------------------------------------------------------------------------
// Collecte des variables lues / écrites dans une instruction (via l'AST)
// ---------------------------------------------------------------------------

class Collector : public RecursiveASTVisitor<Collector> {
public:
  Collector(ASTContext &C) : Ctx(C), SM(C.getSourceManager()) {}

  std::vector<const VarDecl *> Reads;      // variables référencées
  std::vector<const VarDecl *> WriteVars;  // variables modifiées
  std::vector<const Expr *> WriteExprs;    // a[i], p->x, *p modifiés

  bool VisitDeclRefExpr(DeclRefExpr *E) {
    if (PureWrites.count(E)) return true; // cible d'un "=" : pas une lecture
    if (auto *V = dyn_cast<VarDecl>(E->getDecl()))
      if (interesting(V)) addUnique(Reads, V);
    return true;
  }
  bool VisitBinaryOperator(BinaryOperator *B) {
    if (B->isAssignmentOp()) addWrite(B->getLHS()); // inclut +=, -=, ...
    if (B->getOpcode() == BO_Assign) {
      // x = ..., s.f = ... : la variable n'est pas lue, inutile (et source de
      // warning "uninitialized") de l'afficher dans l'état avant.
      Expr *L = B->getLHS()->IgnoreParenImpCasts();
      while (auto *M = dyn_cast<MemberExpr>(L)) {
        if (M->isArrow()) break;
        L = M->getBase()->IgnoreParenImpCasts();
      }
      if (auto *DR = dyn_cast<DeclRefExpr>(L)) PureWrites.insert(DR);
    }
    if (B->isLogicalOp()) guard(B->getRHS());       // évalué conditionnellement
    return true;
  }
  bool VisitConditionalOperator(ConditionalOperator *C) {
    guard(C->getTrueExpr());
    guard(C->getFalseExpr());
    return true;
  }
  bool VisitUnaryOperator(UnaryOperator *U) {
    if (U->isIncrementDecrementOp()) addWrite(U->getSubExpr());
    return true;
  }
  bool VisitCallExpr(CallExpr *CE) {
    // Un tableau passé à une fonction (strcpy(buf, ...)) peut être modifié,
    // sauf si le paramètre correspondant est un pointeur vers const.
    const FunctionDecl *Callee = CE->getDirectCallee();
    for (unsigned I = 0; I < CE->getNumArgs(); ++I) {
      // Argument &x (scanf, set(&x, ...)) ou tableau (strcpy(buf, ...))
      Expr *X = CE->getArg(I)->IgnoreParenImpCasts();
      if (auto *U = dyn_cast<UnaryOperator>(X); U && U->getOpcode() == UO_AddrOf)
        X = U->getSubExpr();
      else if (!isa<DeclRefExpr>(X) || !X->getType()->isArrayType())
        continue;
      if (Callee && I < Callee->getNumParams()) {
        QualType PT = Callee->getParamDecl(I)->getType();
        if (PT->isPointerType() && PT->getPointeeType().isConstQualified()) continue;
      }
      addWrite(X);
    }
    return true;
  }

private:
  ASTContext &Ctx;
  SourceManager &SM;
  std::vector<std::pair<SourceLocation, SourceLocation>> Guarded;
  std::set<const DeclRefExpr *> PureWrites;

  bool interesting(const VarDecl *V) {
    // Seulement les variables déclarées dans le fichier tracé
    // (pas stdout, stderr, optarg...).
    if (!SM.isInMainFile(SM.getExpansionLoc(V->getLocation()))) return false;
    return !V->getName().starts_with("__ct_");
  }
  static void addUnique(std::vector<const VarDecl *> &V, const VarDecl *X) {
    for (auto Y : V) if (Y == X) return;
    V.push_back(X);
  }
  void guard(Expr *E) {
    if (!E) return;
    Guarded.push_back({SM.getExpansionLoc(E->getBeginLoc()),
                       SM.getExpansionLoc(E->getEndLoc())});
  }
  bool isGuarded(const Expr *E) {
    SourceLocation B = SM.getExpansionLoc(E->getBeginLoc());
    SourceLocation End = SM.getExpansionLoc(E->getEndLoc());
    for (auto &G : Guarded)
      if (!SM.isBeforeInTranslationUnit(B, G.first) &&
          !SM.isBeforeInTranslationUnit(G.second, End))
        return true;
    return false;
  }
  void addWrite(Expr *E) {
    E = E->IgnoreParenImpCasts();
    if (auto *DR = dyn_cast<DeclRefExpr>(E)) {
      if (auto *V = dyn_cast<VarDecl>(DR->getDecl()))
        if (interesting(V)) addUnique(WriteVars, V);
      return;
    }
    bool LValueAccess = isa<MemberExpr>(E) || isa<ArraySubscriptExpr>(E);
    if (auto *U = dyn_cast<UnaryOperator>(E))
      LValueAccess = U->getOpcode() == UO_Deref;
    // On ne réévalue après coup que des expressions sans effet de bord et
    // évaluées inconditionnellement (pas de p->x derrière un "p &&").
    if (LValueAccess && !E->HasSideEffects(Ctx) && !isGuarded(E))
      WriteExprs.push_back(E);
  }
};

// ---------------------------------------------------------------------------
// Instrumentation
// ---------------------------------------------------------------------------

class Instrumenter {
public:
  Instrumenter(Rewriter &R, ASTContext &C)
      : R(R), Ctx(C), SM(C.getSourceManager()), LO(C.getLangOpts()),
        File(DisplayName) {
    // largeur fixe pour les numéros de ligne : la trace reste alignée
    StringRef Buf = SM.getBufferData(SM.getMainFileID());
    LineDigits = std::to_string(Buf.count('\n') + 1).size();
  }

  void instrumentFunction(FunctionDecl *FD) {
    auto *Body = dyn_cast<CompoundStmt>(FD->getBody());
    if (!Body) return;
    CurFn = FD;
    FnName = FD->getNameAsString();
    HasRetVal = false;

    instrumentCompound(Body);

    // Entrée de fonction : déclaration du temporaire de retour + paramètres.
    QualType RT = FD->getReturnType();
    std::string Entry;
    if (HasRetVal)
      Entry += "__typeof__(" + typeName(RT) + ") __ct_r; ";
    Entry += "__ct_enter(" + cstr(loc(FD->getLocation())) + ", " + cstr(FnName) + ")";
    for (ParmVarDecl *P : FD->parameters()) {
      std::string N = P->getNameAsString();
      if (N.empty()) continue;
      std::vector<std::string> V;
      emitValue(V, N, N, P->getType());
      Entry += join(V);
    }
    Entry += ", __ct_args();";
    R.InsertTextAfterToken(Body->getLBracLoc(), " " + Entry);

    // Sortie par la fin du corps (fonction void, main sans return).
    R.InsertTextBefore(Body->getRBracLoc(),
                       "__ct_ret(" + cstr(loc(Body->getRBracLoc())) + ", " +
                           cstr(FnName) + "), __ct_leave(); ");
  }

private:
  Rewriter &R;
  ASTContext &Ctx;
  SourceManager &SM;
  const LangOptions &LO;
  std::string File;
  size_t LineDigits = 1;
  FunctionDecl *CurFn = nullptr;
  std::string FnName;
  bool HasRetVal = false;

  // --- positions et texte -------------------------------------------------

  std::string loc(SourceLocation L) {
    std::string N = std::to_string(SM.getExpansionLineNumber(L));
    return File + ":" + N + std::string(LineDigits - std::min(LineDigits, N.size()), ' ');
  }
  CharSourceRange fileRange(SourceRange SR) {
    return Lexer::makeFileCharRange(CharSourceRange::getTokenRange(SR), SM, LO);
  }
  std::string text(SourceRange SR) {
    CharSourceRange FR = fileRange(SR);
    if (FR.isInvalid()) return "?";
    return Lexer::getSourceText(FR, SM, LO).str();
  }
  bool inMain(SourceLocation L) { return SM.isInMainFile(SM.getExpansionLoc(L)); }
  SourceLocation beginOf(Stmt *S) { return SM.getExpansionLoc(S->getBeginLoc()); }

  // Position juste après le ';' qui termine l'instruction.
  SourceLocation afterSemi(Stmt *S) {
    SourceLocation Last = SM.getExpansionRange(S->getEndLoc()).getEnd();
    Token Tok;
    if (!Lexer::getRawToken(Last, Tok, SM, LO, true) && Tok.is(tok::semi))
      return Last.getLocWithOffset(1); // DeclStmt : le ';' fait partie de la plage
    return Lexer::findLocationAfterToken(Last, tok::semi, SM, LO, false);
  }

  std::string typeName(QualType T) {
    return T.getUnqualifiedType().getAsString(Ctx.getPrintingPolicy());
  }

  // --- affichage d'une valeur selon son type (décidé à la compilation) ----

  void emitValue(std::vector<std::string> &Out, const std::string &Name,
                 const std::string &E, QualType T, int Level = 0) {
    T = T.getCanonicalType().getUnqualifiedType();
    std::string N = cstr(Name);
    if (T->isBooleanType()) {
      Out.push_back("__ct_b(" + N + ", (int)(" + E + "))");
    } else if (T->isSpecificBuiltinType(BuiltinType::Char_S) ||
               T->isSpecificBuiltinType(BuiltinType::Char_U)) {
      Out.push_back("__ct_c(" + N + ", (int)(" + E + "))");
    } else if (T->isEnumeralType() || T->isSignedIntegerType()) {
      Out.push_back("__ct_i(" + N + ", (long long)(" + E + "))");
    } else if (T->isUnsignedIntegerType()) {
      Out.push_back("__ct_u(" + N + ", (unsigned long long)(" + E + "))");
    } else if (T->isRealFloatingType()) {
      Out.push_back("__ct_f(" + N + ", (long double)(" + E + "))");
    } else if (T->isPointerType()) {
      Out.push_back("__ct_p(" + N + ", (const void *)(" + E + "))");
    } else if (const ConstantArrayType *CAT = Ctx.getAsConstantArrayType(T)) {
      QualType ET = CAT->getElementType().getCanonicalType();
      uint64_t Count = CAT->getSize().getZExtValue();
      if (ET->isSpecificBuiltinType(BuiltinType::Char_S) ||
          ET->isSpecificBuiltinType(BuiltinType::Char_U)) {
        Out.push_back("__ct_s(" + N + ", (const char *)(" + E + "), sizeof(" + E + "))");
        return;
      }
      int Kind = -1;
      if (ET->isRealFloatingType()) Kind = 2;
      else if (ET->isPointerType()) Kind = 3;
      else if (ET->isEnumeralType() || ET->isSignedIntegerType()) Kind = 0;
      else if (ET->isIntegerType()) Kind = 1;
      if (Kind < 0) return;
      Out.push_back("__ct_arr(" + N + ", (const void *)(" + E + "), " +
                    std::to_string(Count) + "UL, sizeof((" + E + ")[0]), " +
                    std::to_string(Kind) + ")");
    } else if (const RecordType *RT = T->getAs<RecordType>()) {
      if (Level > 0) return; // un seul niveau de struct
      const RecordDecl *RD = RT->getDecl()->getDefinition();
      if (!RD) return;
      // valeur retournée (sans nom) : " = {x=1, y=2}" plutôt que " .x=1 .y=2"
      bool Braces = Name.empty();
      if (Braces) Out.push_back("__ct_rec()");
      for (const FieldDecl *F : RD->fields()) {
        std::string FN = F->getNameAsString();
        if (FN.empty()) continue;
        emitValue(Out, Braces ? FN : Name + "." + FN, "(" + E + ")." + FN,
                  F->getType(), Level + 1);
        if (RD->isUnion()) break; // union : seulement le premier membre
      }
      if (Braces) Out.push_back("__ct_rec_end()");
    }
  }

  // "__ct_pre(loc, "..."), x=.., y=.., __ct_code()" : code + état AVANT
  std::string buildPre(SourceLocation L, const std::vector<const VarDecl *> &Reads,
                       const std::set<const VarDecl *> &Exclude,
                       const std::string &Code) {
    std::string S = "__ct_pre(" + cstr(loc(L)) + ", " + cstr(Code) + ")";
    for (const VarDecl *V : Reads) {
      if (Exclude.count(V)) continue;
      std::vector<std::string> Vals;
      std::string N = V->getNameAsString();
      emitValue(Vals, N, N, V->getType());
      S += join(Vals);
    }
    return S + ", __ct_code()";
  }

  // ", __ct_post(), x=.., __ct_end()" : état APRÈS (vide si rien n'est modifié)
  std::string buildPost(const std::vector<const VarDecl *> &Vars,
                        const std::vector<const Expr *> &Exprs) {
    std::vector<std::string> Vals;
    std::set<std::string> Seen;
    for (const VarDecl *V : Vars) {
      std::string N = V->getNameAsString();
      if (Seen.insert(N).second) emitValue(Vals, N, N, V->getType());
    }
    for (const Expr *E : Exprs) {
      std::string T = oneLine(text(E->getSourceRange()), 60);
      if (T == "?" || !Seen.insert(T).second) continue;
      emitValue(Vals, T, T, E->getType());
    }
    if (Vals.empty()) return "";
    return ", __ct_post()" + join(Vals) + ", __ct_end()";
  }

  // --- parcours des instructions -----------------------------------------

  static bool isControl(Stmt *S) {
    return isa<IfStmt>(S) || isa<WhileStmt>(S) || isa<ForStmt>(S) ||
           isa<DoStmt>(S) || isa<SwitchStmt>(S);
  }

  void instrumentCompound(CompoundStmt *CS) {
    for (Stmt *S : CS->body()) instrumentInList(S);
  }

  // Instruction dans une liste (bloc { }, après un label ou un case).
  void instrumentInList(Stmt *S) {
    if (!S || isa<NullStmt>(S)) return;
    if (auto *C = dyn_cast<CompoundStmt>(S)) return instrumentCompound(C);
    if (auto *L = dyn_cast<LabelStmt>(S)) return instrumentInList(L->getSubStmt());
    if (auto *SC = dyn_cast<SwitchCase>(S)) return instrumentInList(SC->getSubStmt());
    if (isControl(S)) return instrumentControl(S);
    instrumentSimple(S);
  }

  // Corps d'un if/for/while : ajoute des accolades si c'est une instruction seule.
  void instrumentBody(Stmt *S) {
    if (!S) return;
    if (isa<CompoundStmt>(S) || isa<NullStmt>(S) || isControl(S) ||
        isa<LabelStmt>(S) || isa<SwitchCase>(S))
      return instrumentInList(S);
    SourceLocation B = beginOf(S), E = afterSemi(S);
    if (!inMain(B) || E.isInvalid()) return;
    R.InsertTextAfter(B, "{ ");
    instrumentSimple(S);
    R.InsertTextAfter(E, " }");
  }

  void instrumentControl(Stmt *S) {
    if (auto *I = dyn_cast<IfStmt>(S)) {
      wrapCond(I->getCond(), SourceRange(I->getBeginLoc(), I->getRParenLoc()), true);
      instrumentBody(I->getThen());
      instrumentBody(I->getElse());
    } else if (auto *W = dyn_cast<WhileStmt>(S)) {
      wrapCond(W->getCond(), SourceRange(W->getBeginLoc(), W->getRParenLoc()), true);
      instrumentBody(W->getBody());
    } else if (auto *D = dyn_cast<DoStmt>(S)) {
      instrumentBody(D->getBody());
      wrapCond(D->getCond(), SourceRange(D->getWhileLoc(), D->getRParenLoc()), true);
    } else if (auto *F = dyn_cast<ForStmt>(S)) {
      if (F->getCond())
        wrapCond(F->getCond(), SourceRange(F->getBeginLoc(), F->getRParenLoc()), true);
      if (F->getInc()) wrapInc(F->getInc());
      instrumentBody(F->getBody());
    } else if (auto *Sw = dyn_cast<SwitchStmt>(S)) {
      wrapCond(Sw->getCond(), SourceRange(Sw->getBeginLoc(), Sw->getRParenLoc()), false);
      instrumentBody(Sw->getBody());
    }
  }

  // if (c)  ->  if ((pre..., __ct_cv = !!(c), post..., __ct_cond(__ct_cv)))
  void wrapCond(Expr *C, SourceRange Header, bool Boolean) {
    if (!C || !inMain(C->getBeginLoc())) return;
    CharSourceRange FR = fileRange(C->getSourceRange());
    if (FR.isInvalid()) return;
    Collector Col(Ctx);
    Col.TraverseStmt(C);
    std::string Pre = buildPre(C->getBeginLoc(), Col.Reads, {}, oneLine(text(Header)));
    std::string Head, Tail;
    if (Boolean) {
      std::string Post = buildPost(Col.WriteVars, Col.WriteExprs);
      Head = "(" + Pre + ", __ct_cv = !!(";
      Tail = ")" + Post + ", __ct_cond(__ct_cv))";
    } else {
      Head = "(" + Pre + ", (";
      Tail = "))";
    }
    R.InsertTextAfter(FR.getBegin(), Head);
    R.InsertTextBefore(FR.getEnd(), Tail);
  }

  // for (...; ...; i++)  ->  for (...; ...; (pre..., (i++), post...))
  void wrapInc(Expr *Inc) {
    if (!inMain(Inc->getBeginLoc())) return;
    CharSourceRange FR = fileRange(Inc->getSourceRange());
    if (FR.isInvalid()) return;
    Collector Col(Ctx);
    Col.TraverseStmt(Inc);
    std::string Pre = buildPre(Inc->getBeginLoc(), Col.Reads, {}, oneLine(text(Inc->getSourceRange())));
    R.InsertTextAfter(FR.getBegin(), "(" + Pre + ", (");
    R.InsertTextBefore(FR.getEnd(), ")" + buildPost(Col.WriteVars, Col.WriteExprs) + ")");
  }

  void instrumentSimple(Stmt *S) {
    SourceLocation B = beginOf(S);
    if (!inMain(B)) return;
    std::string Code = oneLine(text(S->getSourceRange()));
    if (Code.empty() || Code.back() != ';') Code += ";";

    if (auto *RS = dyn_cast<ReturnStmt>(S)) return instrumentReturn(RS, B, Code);

    Collector Col(Ctx);
    Col.TraverseStmt(S);

    std::set<const VarDecl *> Declared;
    std::vector<const VarDecl *> WriteVars;
    if (auto *DS = dyn_cast<DeclStmt>(S)) {
      for (Decl *D : DS->decls())
        if (auto *V = dyn_cast<VarDecl>(D)) {
          Declared.insert(V);
          if (V->hasInit()) WriteVars.push_back(V);
        }
      if (WriteVars.empty()) return; // "int x;" : rien d'intéressant à tracer
    }
    for (const VarDecl *V : Col.WriteVars) WriteVars.push_back(V);

    R.InsertTextAfter(B, buildPre(S->getBeginLoc(), Col.Reads, Declared, Code) + "; ");

    std::string Post = buildPost(WriteVars, Col.WriteExprs);
    SourceLocation E = afterSemi(S);
    if (!Post.empty() && E.isValid())
      R.InsertTextAfter(E, " " + Post.substr(2) + ";"); // retire le ", " initial
  }

  void instrumentReturn(ReturnStmt *RS, SourceLocation B, const std::string &Code) {
    Expr *RV = RS->getRetValue();
    Collector Col(Ctx);
    if (RV) Col.TraverseStmt(RV);
    std::string Pre = buildPre(RS->getBeginLoc(), Col.Reads, {}, Code);
    std::string Ret = "__ct_ret(" + cstr(loc(RS->getBeginLoc())) + ", " + cstr(FnName) + ")";
    QualType RT = CurFn->getReturnType();

    if (RV && !RT->isVoidType()) {
      // return e;  ->  return (pre..., __ct_r = (e), <- f = v, __ct_r);
      CharSourceRange FR = fileRange(RV->getSourceRange());
      if (FR.isInvalid()) return;
      HasRetVal = true;
      std::vector<std::string> Vals;
      emitValue(Vals, "", "__ct_r", RT);
      R.InsertTextAfter(FR.getBegin(), "(" + Pre + ", __ct_r = (");
      R.InsertTextBefore(FR.getEnd(), ")" + Post(Col) + ", " + Ret + join(Vals) +
                                          ", __ct_leave(), __ct_r)");
    } else {
      // return;  ->  pre...; <- f; return;
      R.InsertTextAfter(B, Pre + ", " + Ret + ", __ct_leave(); ");
    }
  }

  std::string Post(Collector &Col) { return buildPost(Col.WriteVars, Col.WriteExprs); }
};

// ---------------------------------------------------------------------------
// Plomberie Clang
// ---------------------------------------------------------------------------

class CTraceConsumer : public ASTConsumer {
public:
  explicit CTraceConsumer(Rewriter &R) : R(R) {}
  void HandleTranslationUnit(ASTContext &Ctx) override {
    SourceManager &SM = Ctx.getSourceManager();
    Instrumenter I(R, Ctx);
    for (Decl *D : Ctx.getTranslationUnitDecl()->decls()) {
      auto *FD = dyn_cast<FunctionDecl>(D);
      if (!FD || !FD->doesThisDeclarationHaveABody()) continue;
      if (!SM.isInMainFile(SM.getExpansionLoc(FD->getLocation()))) continue;
      I.instrumentFunction(FD);
    }
  }

private:
  Rewriter &R;
};

class CTraceAction : public ASTFrontendAction {
public:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef) override {
    R.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<CTraceConsumer>(R);
  }
  void EndSourceFileAction() override {
    if (getCompilerInstance().getDiagnostics().hasErrorOccurred()) return;
    SourceManager &SM = R.getSourceMgr();
    FileID Main = SM.getMainFileID();
    llvm::raw_ostream &OS = llvm::outs();
    OS << CTraceRuntime << "#line 1 " << cstr(DisplayName) << "\n";
    R.getEditBuffer(Main).write(OS);
    OS.flush();
  }

private:
  Rewriter R;
};

} // namespace

int main(int argc, const char **argv) {
  if (argc < 2 || std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
    llvm::errs() << "usage: ctrace fichier.c [options clang: -I.. -D.. -std=..]\n"
                    "       ctrace a.c | gcc -x c - -o a.out\n"
                    "La trace est écrite sur stderr, ou dans $CTRACE_OUT si défini.\n";
    return 2;
  }
  DisplayName = argv[1];
  std::vector<std::string> Args = {"-xc", "-std=gnu11"};
  if (*CTRACE_RESOURCE_DIR) Args.push_back("-resource-dir=" CTRACE_RESOURCE_DIR);
  for (int i = 2; i < argc; ++i) Args.push_back(argv[i]);

  tooling::FixedCompilationDatabase DB(".", Args);
  tooling::ClangTool Tool(DB, {argv[1]});
  return Tool.run(tooling::newFrontendActionFactory<CTraceAction>().get());
}
