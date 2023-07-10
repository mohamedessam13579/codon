// Copyright (C) 2022-2023 Exaloop Inc. <https://exaloop.io>

#pragma once

#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "codon/parser/ast.h"
#include "codon/parser/common.h"
#include "codon/parser/visitors/typecheck/ctx.h"
#include "codon/parser/visitors/visitor.h"

namespace codon::ast {

/**
 * Visitor that infers expression types and performs type-guided transformations.
 *
 * -> Note: this stage *modifies* the provided AST. Clone it before simplification
 *    if you need it intact.
 */
class TypecheckVisitor : public CallbackASTVisitor<ExprPtr, StmtPtr> {
  /// Shared simplification context.
  std::shared_ptr<TypeContext> ctx;
  /// Statements to prepend before the current statement.
  std::shared_ptr<std::vector<StmtPtr>> prependStmts = nullptr;
  std::shared_ptr<std::vector<StmtPtr>> preamble = nullptr;

  /// Each new expression is stored here (as @c visit does not return anything) and
  /// later returned by a @c transform call.
  ExprPtr resultExpr;
  /// Each new statement is stored here (as @c visit does not return anything) and
  /// later returned by a @c transform call.
  StmtPtr resultStmt;

public:
  // static StmtPtr apply(Cache *cache, const StmtPtr &stmts);
  static StmtPtr
  apply(Cache *cache, const StmtPtr &node, const std::string &file,
        const std::unordered_map<std::string, std::string> &defines = {},
        const std::unordered_map<std::string, std::string> &earlyDefines = {},
        bool barebones = false);
  static StmtPtr apply(const std::shared_ptr<TypeContext> &cache, const StmtPtr &node,
                       const std::string &file = "<internal>");

private:
  static void loadStdLibrary(Cache *, const std::shared_ptr<std::vector<StmtPtr>> &,
                             const std::unordered_map<std::string, std::string> &,
                             bool);

public:
  explicit TypecheckVisitor(
      std::shared_ptr<TypeContext> ctx,
      const std::shared_ptr<std::vector<StmtPtr>> &preamble = nullptr,
      const std::shared_ptr<std::vector<StmtPtr>> &stmts = nullptr);

public: // Convenience transformators
  ExprPtr transform(ExprPtr &e) override;
  ExprPtr transform(const ExprPtr &expr) override {
    auto e = expr;
    return transform(e);
  }
  ExprPtr transform(ExprPtr &expr, bool allowTypes);
  ExprPtr transform(ExprPtr &&expr, bool allowTypes) {
    return transform(expr, allowTypes);
  }
  StmtPtr transform(StmtPtr &s) override;
  StmtPtr transform(const StmtPtr &stmt) override {
    auto s = stmt;
    return transform(s);
  }
  ExprPtr transformType(ExprPtr &expr, bool allowTypeOf = true);
  ExprPtr transformType(const ExprPtr &expr, bool allowTypeOf = true) {
    auto e = expr;
    return transformType(e, allowTypeOf);
  }
  StmtPtr transformConditionalScope(StmtPtr &stmt);

private:
  /// Enter a conditional block.
  void enterConditionalBlock();
  /// Leave a conditional block. Populate stmts (if set) with the declarations of
  /// newly added identifiers that dominate the children blocks.
  void leaveConditionalBlock();
  void leaveConditionalBlock(StmtPtr &);

private:
  void defaultVisit(Expr *e) override;
  void defaultVisit(Stmt *s) override;

private: // Node typechecking rules
  /* Basic type expressions (basic.cpp) */
  void visit(NoneExpr *) override;
  void visit(BoolExpr *) override;
  void visit(IntExpr *) override;
  ExprPtr transformInt(IntExpr *);
  void visit(FloatExpr *) override;
  ExprPtr transformFloat(FloatExpr *);
  void visit(StringExpr *) override;
  ExprPtr transformFString(const std::string &);

  /* Identifier access expressions (access.cpp) */
  void visit(IdExpr *) override;
  TypeContext::Item findDominatingBinding(const std::string &, TypeContext *);
  bool checkCapture(const TypeContext::Item &);
  void visit(DotExpr *) override;
  std::pair<size_t, TypeContext::Item> getImport(const std::vector<std::string> &);
  ExprPtr transformDot(DotExpr *, std::vector<CallExpr::Arg> * = nullptr);
  ExprPtr getClassMember(DotExpr *, std::vector<CallExpr::Arg> *);
  types::TypePtr findSpecialMember(const std::string &);
  types::FuncTypePtr getBestOverload(Expr *, std::vector<CallExpr::Arg> *);
  types::FuncTypePtr getDispatch(const std::string &);

  /* Collection and comprehension expressions (collections.cpp) */
  void visit(TupleExpr *) override;
  void visit(ListExpr *) override;
  void visit(SetExpr *) override;
  void visit(DictExpr *) override;
  ExprPtr transformComprehension(const std::string &, const std::string &,
                                 std::vector<ExprPtr> &);
  void visit(GeneratorExpr *) override;
  void visit(DictGeneratorExpr *) override;
  StmtPtr transformGeneratorBody(const std::vector<GeneratorBody> &, SuiteStmt *&);

  /* Conditional expression and statements (cond.cpp) */
  void visit(IfExpr *) override;
  void visit(IfStmt *) override;
  void visit(MatchStmt *) override;
  StmtPtr transformPattern(const ExprPtr &, ExprPtr, StmtPtr);

  /* Operators (op.cpp) */
  void visit(UnaryExpr *) override;
  ExprPtr evaluateStaticUnary(UnaryExpr *);
  void visit(BinaryExpr *) override;
  ExprPtr evaluateStaticBinary(BinaryExpr *);
  ExprPtr transformBinarySimple(BinaryExpr *);
  ExprPtr transformBinaryIs(BinaryExpr *);
  std::pair<std::string, std::string> getMagic(const std::string &);
  ExprPtr transformBinaryInplaceMagic(BinaryExpr *, bool);
  ExprPtr transformBinaryMagic(BinaryExpr *);
  void visit(ChainBinaryExpr *) override;
  void visit(PipeExpr *) override;
  void visit(IndexExpr *) override;
  std::pair<bool, ExprPtr> transformStaticTupleIndex(const types::ClassTypePtr &,
                                                     const ExprPtr &, const ExprPtr &);
  int64_t translateIndex(int64_t, int64_t, bool = false);
  int64_t sliceAdjustIndices(int64_t, int64_t *, int64_t *, int64_t);
  void visit(InstantiateExpr *) override;
  void visit(SliceExpr *) override;

  /* Calls (call.cpp) */
  void visit(PrintStmt *) override;
  /// Holds partial call information for a CallExpr.
  struct PartialCallData {
    bool isPartial = false;                   // true if the call is partial
    std::string var;                          // set if calling a partial type itself
    std::vector<char> known = {};             // mask of known arguments
    ExprPtr args = nullptr, kwArgs = nullptr; // partial *args/**kwargs expressions
  };
  void visit(StarExpr *) override;
  void visit(KeywordStarExpr *) override;
  void visit(EllipsisExpr *) override;
  void visit(CallExpr *) override;
  bool transformCallArgs(std::vector<CallExpr::Arg> &);
  std::pair<types::FuncTypePtr, ExprPtr> getCalleeFn(CallExpr *, PartialCallData &);
  ExprPtr callReorderArguments(types::FuncTypePtr, CallExpr *, PartialCallData &);
  bool typecheckCallArgs(const types::FuncTypePtr &, std::vector<CallExpr::Arg> &);
  std::pair<bool, ExprPtr> transformSpecialCall(CallExpr *);
  ExprPtr transformTupleGenerator(CallExpr *);
  ExprPtr transformNamedTuple(CallExpr *);
  ExprPtr transformFunctoolsPartial(CallExpr *);
  ExprPtr transformSuperF(CallExpr *);
  ExprPtr transformSuper();
  ExprPtr transformPtr(CallExpr *);
  ExprPtr transformArray(CallExpr *);
  ExprPtr transformIsInstance(CallExpr *);
  ExprPtr transformStaticLen(CallExpr *);
  ExprPtr transformHasAttr(CallExpr *);
  ExprPtr transformGetAttr(CallExpr *);
  ExprPtr transformSetAttr(CallExpr *);
  ExprPtr transformCompileError(CallExpr *);
  ExprPtr transformTupleFn(CallExpr *);
  ExprPtr transformTypeFn(CallExpr *);
  ExprPtr transformRealizedFn(CallExpr *);
  ExprPtr transformStaticPrintFn(CallExpr *);
  ExprPtr transformHasRttiFn(CallExpr *);
  std::pair<bool, ExprPtr> transformInternalStaticFn(CallExpr *);
  std::vector<types::ClassTypePtr> getSuperTypes(const types::ClassTypePtr &);
  void addFunctionGenerics(const types::FuncType *t);
  std::string generatePartialStub(const std::vector<char> &, types::FuncType *);

  /* Assignments (assign.cpp) */
  void visit(AssignExpr *) override;
  void visit(AssignStmt *) override;
  void transformUpdate(AssignStmt *);
  StmtPtr transformAssignment(ExprPtr, ExprPtr, ExprPtr = nullptr, bool = false);
  void unpackAssignments(const ExprPtr &, ExprPtr, std::vector<StmtPtr> &);
  void visit(DelStmt *) override;
  void visit(AssignMemberStmt *) override;
  std::pair<bool, ExprPtr> transformInplaceUpdate(AssignStmt *);

  /* Imports (import.cpp) */
  void visit(ImportStmt *) override;
  StmtPtr transformSpecialImport(ImportStmt *);
  std::vector<std::string> getImportPath(Expr *, size_t = 0);
  StmtPtr transformCImport(const std::string &, const std::vector<Param> &,
                           const Expr *, const std::string &);
  StmtPtr transformCVarImport(const std::string &, const Expr *, const std::string &);
  StmtPtr transformCDLLImport(const Expr *, const std::string &,
                              const std::vector<Param> &, const Expr *,
                              const std::string &, bool);
  StmtPtr transformPythonImport(Expr *, const std::vector<Param> &, Expr *,
                                const std::string &);
  StmtPtr transformNewImport(const ImportFile &);

  /* Loops (loops.cpp) */
  void visit(BreakStmt *) override;
  void visit(ContinueStmt *) override;
  void visit(WhileStmt *) override;
  void visit(ForStmt *) override;
  ExprPtr transformForDecorator(const ExprPtr &);
  StmtPtr transformHeterogenousTupleFor(ForStmt *);
  StmtPtr transformStaticForLoop(ForStmt *);

  /* Errors and exceptions (error.cpp) */
  void visit(AssertStmt *) override;
  void visit(TryStmt *) override;
  void visit(ThrowStmt *) override;
  void visit(WithStmt *) override;

  /* Functions (function.cpp) */
  void visit(YieldExpr *) override;
  void visit(ReturnStmt *) override;
  void visit(YieldStmt *) override;
  void visit(YieldFromStmt *) override;
  void visit(LambdaExpr *) override;
  void visit(GlobalStmt *) override;
  void visit(FunctionStmt *) override;
  ExprPtr makeAnonFn(std::vector<StmtPtr>, const std::vector<std::string> & = {});
  StmtPtr transformPythonDefinition(const std::string &, const std::vector<Param> &,
                                    const Expr *, Stmt *);
  StmtPtr transformLLVMDefinition(Stmt *);
  std::pair<bool, std::string> getDecorator(const ExprPtr &);
  ExprPtr partializeFunction(const types::FuncTypePtr &);
  std::shared_ptr<types::RecordType> getFuncTypeBase(size_t);

  /* Classes (class.cpp) */
  void visit(ClassStmt *) override;
  std::vector<ClassStmt *> parseBaseClasses(std::vector<ExprPtr> &,
                                            std::vector<Param> &, const Attr &,
                                            const std::string &, const ExprPtr &,
                                            types::ClassTypePtr &);
  std::pair<StmtPtr, FunctionStmt *> autoDeduceMembers(ClassStmt *,
                                                       std::vector<Param> &);
  std::vector<StmtPtr> getClassMethods(const StmtPtr &s);
  void transformNestedClasses(ClassStmt *, std::vector<StmtPtr> &,
                              std::vector<StmtPtr> &, std::vector<StmtPtr> &);
  StmtPtr codegenMagic(const std::string &, const ExprPtr &, const std::vector<Param> &,
                       bool);
  std::string generateTuple(size_t, const std::string & = TYPE_TUPLE,
                            std::vector<std::string> = {}, bool = true);

  /* The rest (typecheck.cpp) */
  void visit(SuiteStmt *) override;
  void visit(ExprStmt *) override;
  void visit(StmtExpr *) override;
  void visit(CommentStmt *stmt) override;
  void visit(CustomStmt *) override;

public:
  /* Type inference (infer.cpp) */
  types::TypePtr unify(types::TypePtr &a, const types::TypePtr &b);
  types::TypePtr unify(types::TypePtr &&a, const types::TypePtr &b) {
    auto x = a;
    return unify(x, b);
  }

private:
  StmtPtr inferTypes(StmtPtr, bool isToplevel = false);
  types::TypePtr realize(types::TypePtr);
  types::TypePtr realizeFunc(types::FuncType *, bool = false);
  types::TypePtr realizeType(types::ClassType *);
  std::shared_ptr<FunctionStmt> generateSpecialAst(types::FuncType *);
  size_t getRealizationID(types::ClassType *, types::FuncType *);
  codon::ir::types::Type *makeIRType(types::ClassType *);
  codon::ir::Func *
  makeIRFunction(const std::shared_ptr<Cache::Function::FunctionRealization> &);

  types::TypePtr getClassGeneric(const types::ClassTypePtr &, int = 0);
  std::string getClassStaticStr(const types::ClassTypePtr &, int = 0);
  int64_t getClassStaticInt(const types::ClassTypePtr &, int = 0);

private:
  types::FuncTypePtr findBestMethod(const types::ClassTypePtr &typ,
                                    const std::string &member,
                                    const std::vector<types::TypePtr> &args);
  types::FuncTypePtr findBestMethod(const types::ClassTypePtr &typ,
                                    const std::string &member,
                                    const std::vector<ExprPtr> &args);
  types::FuncTypePtr
  findBestMethod(const types::ClassTypePtr &typ, const std::string &member,
                 const std::vector<std::pair<std::string, types::TypePtr>> &args);
  int canCall(const types::FuncTypePtr &, const std::vector<CallExpr::Arg> &);
  std::vector<types::FuncTypePtr>
  findMatchingMethods(const types::ClassTypePtr &typ,
                      const std::vector<types::FuncTypePtr> &methods,
                      const std::vector<CallExpr::Arg> &args);
  bool wrapExpr(ExprPtr &expr, const types::TypePtr &expectedType,
                const types::FuncTypePtr &callee = nullptr, bool allowUnwrap = true);
  ExprPtr castToSuperClass(ExprPtr expr, types::ClassTypePtr superTyp, bool = false);
  StmtPtr prepareVTables();

public:
  bool isTuple(const std::string &s) const { return startswith(s, TYPE_TUPLE); }
  std::shared_ptr<TypeContext> getCtx() const { return ctx; }

  friend class Cache;
  friend class TypeContext;
  friend class types::CallableTrait;
  friend class types::UnionType;

private: // Helpers
  std::shared_ptr<std::vector<std::pair<std::string, types::TypePtr>>>
      unpackTupleTypes(ExprPtr);
  std::pair<bool, std::vector<std::shared_ptr<codon::SrcObject>>>
  transformStaticLoopCall(
      const std::vector<std::string> &, const ExprPtr &,
      const std::function<std::shared_ptr<codon::SrcObject>(StmtPtr)> &);
};

class NameVisitor : public CallbackASTVisitor<ExprPtr, StmtPtr> {
  TypecheckVisitor *tv;
  ExprPtr resultExpr = nullptr;
  StmtPtr resultStmt = nullptr;

public:
  NameVisitor(TypecheckVisitor *tv) : tv(tv) {}
  static void apply(TypecheckVisitor *tv, std::vector<StmtPtr> &v);
  static void apply(TypecheckVisitor *tv, StmtPtr &s);
  static void apply(TypecheckVisitor *tv, ExprPtr &s);
  ExprPtr transform(const std::shared_ptr<Expr> &expr) override;
  ExprPtr transform(std::shared_ptr<Expr> &expr) override;
  StmtPtr transform(const std::shared_ptr<Stmt> &stmt) override;
  StmtPtr transform(std::shared_ptr<Stmt> &stmt) override;
  void visit(IdExpr *expr) override;
  void visit(AssignStmt *stmt) override;
  void visit(TryStmt *stmt) override;
  void visit(ForStmt *stmt) override;
  void visit(FunctionStmt *stmt) override;
};

// class Name2Visitor : public CallbackASTVisitor<ExprPtr, StmtPtr> {
//   TypecheckVisitor *tv;
//   ExprPtr resultExpr = nullptr;
//   StmtPtr resultStmt = nullptr;

// public:
//   Name2Visitor() {}
//   static void apply(TypecheckVisitor *tv, std::vector<StmtPtr> &v);
//   static void apply(TypecheckVisitor *tv, StmtPtr &s);
//   static void apply(TypecheckVisitor *tv, ExprPtr &s);
//   ExprPtr transform(const std::shared_ptr<Expr> &expr) override;
//   ExprPtr transform(std::shared_ptr<Expr> &expr) override;
//   StmtPtr transform(const std::shared_ptr<Stmt> &stmt) override;
//   StmtPtr transform(std::shared_ptr<Stmt> &stmt) override;

//   void visit(IdExpr *expr) {
//     auto name = expr->value;
//     if (add) {
//       if (in(local, name))
//         error;
//       local.insert(name);
//     } else {
//       if (!in(local, name))
//         seen.insert(name);
//     }
//   }
//   void visit(AssignStmt *stmt) override {
//     add = true;
//     transform(stmt->lhs);
//     add = false;
//     transform(stmt->rhs);
//     transform(stmt->type);
//   }
//   void visit(TryStmt *stmt) {

//   }
//   void visit(ForStmt *stmt) {

//   }
//   void visit(FunctionStmt *stmt) {

//   }
// };

} // namespace codon::ast
