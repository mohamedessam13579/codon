// Copyright (C) 2022-2024 Exaloop Inc. <https://exaloop.io>

#include <string>
#include <tuple>

#include "codon/parser/ast.h"
#include "codon/parser/common.h"
#include "codon/parser/match.h"
#include "codon/parser/peg/peg.h"
#include "codon/parser/visitors/typecheck/typecheck.h"

using fmt::format;
using namespace codon::error;
namespace codon::ast {

using namespace types;
using namespace matcher;

/// Ensure that `break` is in a loop.
/// Transform if a loop break variable is available
/// (e.g., a break within loop-else block).
/// @example
///   `break` -> `no_break = False; break`

void TypecheckVisitor::visit(BreakStmt *stmt) {
  if (!ctx->getBase()->getLoop())
    E(Error::EXPECTED_LOOP, stmt, "break");
  ctx->getBase()->getLoop()->flat = false;
  if (!ctx->getBase()->getLoop()->breakVar.empty()) {
    resultStmt =
        N<SuiteStmt>(transform(N<AssignStmt>(
                         N<IdExpr>(ctx->getBase()->getLoop()->breakVar),
                         N<BoolExpr>(false), nullptr, AssignStmt::UpdateMode::Update)),
                     N<BreakStmt>());
  } else {
    stmt->setDone();
    if (!ctx->staticLoops.back().empty()) {
      auto a = N<AssignStmt>(N<IdExpr>(ctx->staticLoops.back()), N<BoolExpr>(false));
      a->setUpdate();
      resultStmt = transform(N<SuiteStmt>(a, stmt));
    }
  }
}

/// Ensure that `continue` is in a loop
void TypecheckVisitor::visit(ContinueStmt *stmt) {
  if (!ctx->getBase()->getLoop())
    E(Error::EXPECTED_LOOP, stmt, "continue");
  ctx->getBase()->getLoop()->flat = false;

  stmt->setDone();
  if (!ctx->staticLoops.back().empty()) {
    resultStmt = N<BreakStmt>();
    resultStmt->setDone();
  }
}

/// Transform a while loop.
/// @example
///   `while cond: ...`           ->  `while cond.__bool__(): ...`
///   `while cond: ... else: ...` -> ```no_break = True
///                                     while cond.__bool__():
///                                       ...
///                                     if no_break: ...```
void TypecheckVisitor::visit(WhileStmt *stmt) {
  // Check for while-else clause
  std::string breakVar;
  if (stmt->getElse() && stmt->getElse()->firstInBlock()) {
    // no_break = True
    breakVar = getTemporaryVar("no_break");
    prependStmts->push_back(
        transform(N<AssignStmt>(N<IdExpr>(breakVar), N<BoolExpr>(true))));
  }

  ctx->staticLoops.push_back(stmt->gotoVar.empty() ? "" : stmt->gotoVar);
  ctx->getBase()->loops.emplace_back(breakVar);
  stmt->cond = transform(stmt->getCond());
  if (stmt->getCond()->getClassType() && !stmt->getCond()->getType()->is("bool"))
    stmt->cond = transform(N<CallExpr>(N<DotExpr>(stmt->getCond(), "__bool__")));

  ctx->blockLevel++;
  stmt->suite = SuiteStmt::wrap(transform(stmt->getSuite()));
  ctx->blockLevel--;
  ctx->staticLoops.pop_back();

  // Complete while-else clause
  if (stmt->getElse() && stmt->getElse()->firstInBlock()) {
    auto es = stmt->getElse();
    stmt->elseSuite = nullptr;
    resultStmt = transform(N<SuiteStmt>(stmt, N<IfStmt>(N<IdExpr>(breakVar), es)));
  }
  ctx->getBase()->loops.pop_back();

  if (stmt->getCond()->isDone() && stmt->getSuite()->isDone())
    stmt->setDone();
}

/// Typecheck for statements. Wrap the iterator expression with `__iter__` if needed.
/// See @c transformHeterogenousTupleFor for iterating heterogenous tuples.
void TypecheckVisitor::visit(ForStmt *stmt) {
  stmt->decorator = transformForDecorator(stmt->getDecorator());

  std::string breakVar;
  // Needs in-advance transformation to prevent name clashes with the iterator variable
  stmt->iter = transform(stmt->getIter());

  // Check for for-else clause
  Stmt *assign = nullptr;
  if (stmt->getElse() && stmt->getElse()->firstInBlock()) {
    breakVar = getTemporaryVar("no_break");
    assign = transform(N<AssignStmt>(N<IdExpr>(breakVar), N<BoolExpr>(true)));
  }

  // Extract the iterator type of the for
  auto iterType = extractClassType(stmt->getIter());
  if (!iterType)
    return; // wait until the iterator is known

  auto [delay, staticLoop] = transformStaticForLoop(stmt);
  if (delay)
    return;
  if (staticLoop) {
    resultStmt = staticLoop;
    return;
  }

  // Case: iterating a non-generator. Wrap with `__iter__`
  if (iterType->name != "Generator" && !stmt->isWrapped()) {
    stmt->iter = transform(N<CallExpr>(N<DotExpr>(stmt->getIter(), "__iter__")));
    iterType = extractClassType(stmt->getIter());
    stmt->wrapped = true;
  }

  ctx->getBase()->loops.emplace_back(breakVar);
  auto var = cast<IdExpr>(stmt->getVar());
  seqassert(var, "corrupt for variable: {}", *(stmt->getVar()));

  if (!stmt->hasAttribute(Attr::ExprDominated) &&
      !stmt->hasAttribute(Attr::ExprDominatedUsed)) {
    ctx->addVar(var->getValue(), ctx->generateCanonicalName(var->getValue()),
                instantiateUnbound());
  } else if (stmt->hasAttribute(Attr::ExprDominatedUsed)) {
    stmt->eraseAttribute(Attr::ExprDominatedUsed);
    stmt->setAttribute(Attr::ExprDominated);
    stmt->suite = N<SuiteStmt>(
        N<AssignStmt>(N<IdExpr>(format("{}{}", var->getValue(), VAR_USED_SUFFIX)),
                      N<BoolExpr>(true), nullptr, AssignStmt::UpdateMode::Update),
        stmt->getSuite());
  }
  stmt->var = transform(stmt->getVar());

  // Unify iterator variable and the iterator type
  if (iterType && iterType->name != "Generator")
    E(Error::EXPECTED_GENERATOR, stmt->getIter());
  if (iterType)
    unify(stmt->getVar()->getType(), extractClassGeneric(iterType));

  ctx->staticLoops.emplace_back();
  ctx->blockLevel++;
  stmt->suite = SuiteStmt::wrap(transform(stmt->getSuite()));
  ctx->blockLevel--;
  ctx->staticLoops.pop_back();

  if (ctx->getBase()->getLoop()->flat)
    stmt->flat = true;

  // Complete for-else clause
  if (stmt->getElse() && stmt->getElse()->firstInBlock()) {
    auto es = stmt->getElse();
    stmt->elseSuite = nullptr;
    resultStmt =
        transform(N<SuiteStmt>(assign, stmt, N<IfStmt>(N<IdExpr>(breakVar), es)));
    stmt->elseSuite = nullptr;
  }

  ctx->getBase()->loops.pop_back();

  if (stmt->getIter()->isDone() && stmt->getSuite()->isDone())
    stmt->setDone();
}

/// Transform and check for OpenMP decorator.
/// @example
///   `@par(num_threads=2, openmp="schedule(static)")` ->
///   `for_par(num_threads=2, schedule="static")`
Expr *TypecheckVisitor::transformForDecorator(Expr *decorator) {
  if (!decorator)
    return nullptr;
  Expr *callee = decorator;
  if (auto c = cast<CallExpr>(callee))
    callee = c->getExpr();
  auto ci = cast<IdExpr>(transform(callee));
  if (!ci || !startswith(ci->getValue(), "std.openmp.for_par.0")) {
    E(Error::LOOP_DECORATOR, decorator);
  }

  std::vector<CallArg> args;
  std::string openmp;
  std::vector<CallArg> omp;
  if (auto c = cast<CallExpr>(decorator))
    for (auto &a : *c) {
      if (a.getName() == "openmp" ||
          (a.getName().empty() && openmp.empty() && cast<StringExpr>(a.getExpr()))) {
        omp = parseOpenMP(ctx->cache, cast<StringExpr>(a.getExpr())->getValue(),
                          a.value->getSrcInfo());
      } else {
        args.emplace_back(a.getName(), transform(a.getExpr()));
      }
    }
  for (auto &a : omp)
    args.emplace_back(a.getName(), transform(a.getExpr()));
  return transform(N<CallExpr>(transform(N<IdExpr>("for_par")), args));
}

/// Handle static for constructs.
/// @example
///   `for i in statictuple(1, x): <suite>` ->
///   ```loop = True
///      while loop:
///        while loop:
///          i: Static[int] = 1; <suite>; break
///        while loop:
///          i = x; <suite>; break
///        loop = False   # also set to False on break
/// If a loop is flat, while wrappers are removed.
/// A separate suite is generated for each static iteration.
std::pair<bool, Stmt *> TypecheckVisitor::transformStaticForLoop(ForStmt *stmt) {
  auto loopVar = getTemporaryVar("loop");
  auto suite = clean_clone(stmt->getSuite());
  auto [ok, delay, preamble, items] = transformStaticLoopCall(
      stmt->getVar(), &suite, stmt->getIter(), [&](Stmt *assigns) {
        Stmt *ret = nullptr;
        if (!stmt->flat) {
          auto brk = N<BreakStmt>();
          brk->setDone(); // Avoid transforming this one to continue
          // var [: Static] := expr; suite...
          auto loop = N<WhileStmt>(N<IdExpr>(loopVar),
                                   N<SuiteStmt>(assigns, clone(suite), brk));
          loop->gotoVar = loopVar;
          ret = loop;
        } else {
          ret = N<SuiteStmt>(assigns, clone(stmt->getSuite()));
        }
        return ret;
      });
  if (!ok)
    return {false, nullptr};
  if (delay)
    return {true, nullptr};

  // Close the loop
  auto block = N<SuiteStmt>();
  block->addStmt(preamble);
  for (auto &i : items)
    block->addStmt(cast<Stmt>(i));
  Stmt *loop = nullptr;
  if (!stmt->flat) {
    ctx->blockLevel++;
    auto a = N<AssignStmt>(N<IdExpr>(loopVar), N<BoolExpr>(false));
    a->setUpdate();
    block->addStmt(a);
    loop = transform(N<SuiteStmt>(N<AssignStmt>(N<IdExpr>(loopVar), N<BoolExpr>(true)),
                                  N<WhileStmt>(N<IdExpr>(loopVar), block)));
    ctx->blockLevel--;
  } else {
    loop = transform(block);
  }
  return {false, loop};
}

std::tuple<bool, bool, Stmt *, std::vector<ASTNode *>>
TypecheckVisitor::transformStaticLoopCall(Expr *varExpr, SuiteStmt **varSuite,
                                          Expr *iter,
                                          const std::function<ASTNode *(Stmt *)> &wrap,
                                          bool allowNonHeterogenous) {
  if (!iter->getClassType())
    return {true, true, nullptr, {}};

  seqassert(cast<IdExpr>(varExpr), "bad varExpr");
  std::function<int(Stmt **, const std::function<void(Stmt **)> &)> iterFn;
  iterFn = [&iterFn](Stmt **s, const std::function<void(Stmt **)> &fn) -> int {
    if (!s)
      return 0;
    if (auto su = cast<SuiteStmt>(*s)) {
      int i = 0;
      for (auto &si : *su) {
        i += iterFn(&si, fn);
      }
      return i;
    } else {
      fn(s);
      return 1;
    }
  };
  std::vector<std::string> vars{cast<IdExpr>(varExpr)->getValue()};
  iterFn((Stmt **)varSuite, [&](Stmt **s) {
    // Handle iteration var transformations (for i, j in x -> for _ in x: (i, j = _;
    // ...))
    IdExpr *var = nullptr;
    if (match(*s, M<AssignStmt>(MVar<IdExpr>(var), M<IndexExpr>(M<IdExpr>(vars[0]), M_),
                                M_, M_))) {
      vars.push_back(var->getValue());
      *s = nullptr;
    }
  });
  if (vars.size() > 1)
    vars.erase(vars.begin());
  if (vars.empty())
    return {false, false, nullptr, {}};

  Stmt *preamble = nullptr;
  auto fn =
      cast<CallExpr>(iter) ? cast<IdExpr>(cast<CallExpr>(iter)->getExpr()) : nullptr;
  std::vector<Stmt *> block;
  if (fn && startswith(fn->getValue(), "statictuple")) {
    block = populateStaticTupleLoop(iter, vars);
  } else if (fn &&
             startswith(fn->getValue(), "std.internal.types.range.staticrange.0:1")) {
    block = populateSimpleStaticRangeLoop(iter, vars);
  } else if (fn &&
             startswith(fn->getValue(), "std.internal.types.range.staticrange.0")) {
    block = populateStaticRangeLoop(iter, vars);
  } else if (fn && startswith(fn->getValue(), "std.internal.static.fn_overloads.0")) {
    block = populateStaticFnOverloadsLoop(iter, vars);
  } else if (fn &&
             startswith(fn->getValue(), "std.internal.builtin.staticenumerate.0")) {
    block = populateStaticEnumerateLoop(iter, vars);
  } else if (fn && startswith(fn->getValue(), "std.internal.internal.vars.0")) {
    block = populateStaticVarsLoop(iter, vars);
  } else if (fn && startswith(fn->getValue(), "std.internal.static.vars_types.0")) {
    block = populateStaticVarTypesLoop(iter, vars);
  } else {
    bool maybeHeterogenous = iter->getType()->is(TYPE_TUPLE);
    if (maybeHeterogenous) {
      if (!iter->getType()->canRealize())
        return {true, true, nullptr, {}}; // wait until the tuple is fully realizable
      if (!iter->getClassType()->getHeterogenousTuple() && !allowNonHeterogenous)
        return {false, false, nullptr, {}};
      block = populateStaticHeterogenousTupleLoop(iter, vars);
      preamble = block.back();
      block.pop_back();
    } else {
      return {false, false, nullptr, {}};
    }
  }
  std::vector<ASTNode *> wrapBlock;
  wrapBlock.reserve(block.size());
  for (auto b : block) {
    wrapBlock.push_back(wrap(b));
  }
  return {true, false, preamble, wrapBlock};
}

} // namespace codon::ast
