// Copyright (C) 2022-2023 Exaloop Inc. <https://exaloop.io>

#include "expr.h"

#include <memory>
#include <string>
#include <vector>

#include "codon/parser/ast.h"
#include "codon/parser/cache.h"
#include "codon/parser/visitors/visitor.h"

#define ACCEPT_IMPL(T, X)                                                              \
  std::shared_ptr<Node> T::clone(bool c) const {                                       \
    return std::make_shared<T>(*this, c);                                              \
  }                                                                                    \
  void T::accept(X &visitor) { visitor.visit(this); }

using fmt::format;
using namespace codon::error;

namespace codon::ast {

Expr::Expr()
    : type(nullptr), isTypeExpr(false), staticValue(StaticValue::NOT_STATIC),
      done(false), attributes(0), origExpr(nullptr) {}
Expr::Expr(const Expr &expr, bool clean) : Expr(expr) {
  if (clean) {
    type = nullptr;
    done = false;
    // attributes = 0;
  }
}
void Expr::validate() const {}
types::TypePtr Expr::getType() const { return type; }
void Expr::setType(types::TypePtr t) { this->type = std::move(t); }
bool Expr::isType() const { return isTypeExpr; }
void Expr::markType() { isTypeExpr = true; }
std::string Expr::wrapType(const std::string &sexpr) const {
  auto is = sexpr;
  if (done)
    is.insert(findStar(is), "*");
  auto s = format("({}{})", is,
                  type && !done ? format(" #:type \"{}\"", type->debugString(2)) : "");
  // if (hasAttr(ExprAttr::SequenceItem)) s += "%";
  return s;
}
bool Expr::isStatic() const { return staticValue.type != StaticValue::NOT_STATIC; }
bool Expr::hasAttr(int attr) const { return (attributes & (1 << attr)); }
void Expr::setAttr(int attr) { attributes |= (1 << attr); }
std::string Expr::getTypeName() {
  if (getId()) {
    return getId()->value;
  } else {
    auto i = dynamic_cast<InstantiateExpr *>(this);
    seqassertn(i && i->typeExpr->getId(), "bad MRO");
    return i->typeExpr->getId()->value;
  }
}

StaticValue::StaticValue(StaticValue::Type t) : value(), type(t), evaluated(false) {}
StaticValue::StaticValue(int64_t i) : value(i), type(INT), evaluated(true) {}
StaticValue::StaticValue(std::string s)
    : value(std::move(s)), type(STRING), evaluated(true) {}
bool StaticValue::operator==(const StaticValue &s) const {
  if (type != s.type || s.evaluated != evaluated)
    return false;
  return !s.evaluated || value == s.value;
}
std::string StaticValue::toString(int) const {
  if (type == StaticValue::NOT_STATIC)
    return "";
  if (!evaluated)
    return type == StaticValue::STRING ? "str" : "int";
  return type == StaticValue::STRING ? "'" + escape(std::get<std::string>(value)) + "'"
                                     : std::to_string(std::get<int64_t>(value));
}
int64_t StaticValue::getInt() const {
  seqassertn(type == StaticValue::INT, "not an int");
  return std::get<int64_t>(value);
}
std::string StaticValue::getString() const {
  seqassertn(type == StaticValue::STRING, "not a string");
  return std::get<std::string>(value);
}

Param::Param(std::string name, ExprPtr type, ExprPtr defaultValue, int status)
    : name(std::move(name)), type(std::move(type)),
      defaultValue(std::move(defaultValue)) {
  if (status == 0 && this->type &&
      (this->type->isId("type") || this->type->isId(TYPE_TYPEVAR) ||
       (this->type->getIndex() && this->type->getIndex()->expr->isId(TYPE_TYPEVAR)) ||
       getStaticGeneric(this->type.get())))
    this->status = Generic;
  else
    this->status = (status == 0 ? Normal : (status == 1 ? Generic : HiddenGeneric));
}
Param::Param(const SrcInfo &info, std::string name, ExprPtr type, ExprPtr defaultValue,
             int status)
    : Param(name, type, defaultValue, status) {
  setSrcInfo(info);
}
std::string Param::toString(int indent) const {
  return format("({}{}{}{})", name, type ? " #:type " + type->toString(indent) : "",
                defaultValue ? " #:default " + defaultValue->toString(indent) : "",
                status != Param::Normal ? " #:generic" : "");
}
Param Param::clone(bool clean) const {
  return Param(name, ast::clone(type, clean), ast::clone(defaultValue, clean), status);
}

NoneExpr::NoneExpr() : Expr() {}
NoneExpr::NoneExpr(const NoneExpr &expr, bool clean) : Expr(expr, clean) {}
std::string NoneExpr::toString(int) const { return wrapType("none"); }
ACCEPT_IMPL(NoneExpr, ASTVisitor);

BoolExpr::BoolExpr(bool value) : Expr(), value(value) {
  staticValue = StaticValue(value);
}
BoolExpr::BoolExpr(const BoolExpr &expr, bool clean)
    : Expr(expr, clean), value(expr.value) {}
std::string BoolExpr::toString(int) const {
  return wrapType(format("bool {}", int(value)));
}
ACCEPT_IMPL(BoolExpr, ASTVisitor);

IntExpr::IntExpr(int64_t intValue) : Expr(), value(std::to_string(intValue)) {
  this->intValue = std::make_unique<int64_t>(intValue);
  staticValue = StaticValue(intValue);
}
IntExpr::IntExpr(const std::string &value, std::string suffix)
    : Expr(), value(), suffix(std::move(suffix)) {
  for (auto c : value)
    if (c != '_')
      this->value += c;
  try {
    if (startswith(this->value, "0b") || startswith(this->value, "0B"))
      intValue =
          std::make_unique<int64_t>(std::stoull(this->value.substr(2), nullptr, 2));
    else
      intValue = std::make_unique<int64_t>(std::stoull(this->value, nullptr, 0));
    staticValue = StaticValue(*intValue);
  } catch (std::out_of_range &) {
    intValue = nullptr;
  }
}
IntExpr::IntExpr(const IntExpr &expr, bool clean)
    : Expr(expr, clean), value(expr.value), suffix(expr.suffix) {
  intValue = expr.intValue ? std::make_unique<int64_t>(*(expr.intValue)) : nullptr;
}
std::string IntExpr::toString(int) const {
  return wrapType(format("int {}{}", value,
                         suffix.empty() ? "" : format(" #:suffix \"{}\"", suffix)));
}
ACCEPT_IMPL(IntExpr, ASTVisitor);

FloatExpr::FloatExpr(double floatValue)
    : Expr(), value(fmt::format("{:g}", floatValue)) {
  this->floatValue = std::make_unique<double>(floatValue);
}
FloatExpr::FloatExpr(const std::string &value, std::string suffix)
    : Expr(), value(value), suffix(std::move(suffix)) {
  try {
    floatValue = std::make_unique<double>(std::stod(value));
  } catch (std::out_of_range &) {
    floatValue = nullptr;
  }
}
FloatExpr::FloatExpr(const FloatExpr &expr, bool clean)
    : Expr(expr, clean), value(expr.value), suffix(expr.suffix) {
  floatValue = expr.floatValue ? std::make_unique<double>(*(expr.floatValue)) : nullptr;
}
std::string FloatExpr::toString(int) const {
  return wrapType(format("float {}{}", value,
                         suffix.empty() ? "" : format(" #:suffix \"{}\"", suffix)));
}
ACCEPT_IMPL(FloatExpr, ASTVisitor);

StringExpr::StringExpr(std::vector<std::pair<std::string, std::string>> s)
    : Expr(), strings(std::move(s)) {
  if (strings.size() == 1 && strings.back().second.empty())
    staticValue = StaticValue(strings.back().first);
}
StringExpr::StringExpr(std::string value, std::string prefix)
    : StringExpr(std::vector<std::pair<std::string, std::string>>{{value, prefix}}) {}
StringExpr::StringExpr(const StringExpr &expr, bool clean)
    : Expr(expr, clean), strings(expr.strings) {}
std::string StringExpr::toString(int) const {
  std::vector<std::string> s;
  for (auto &vp : strings)
    s.push_back(format("\"{}\"{}", escape(vp.first),
                       vp.second.empty() ? "" : format(" #:prefix \"{}\"", vp.second)));
  return wrapType(format("string ({})", join(s)));
}
std::string StringExpr::getValue() const {
  seqassert(!strings.empty(), "invalid StringExpr");
  return strings[0].first;
}
ACCEPT_IMPL(StringExpr, ASTVisitor);

IdExpr::IdExpr(std::string value) : Expr(), value(std::move(value)) {}
IdExpr::IdExpr(const IdExpr &expr, bool clean) : Expr(expr, clean), value(expr.value) {}
std::string IdExpr::toString(int) const {
  return !type ? format("'{}", value) : wrapType(format("'{}", value));
}
ACCEPT_IMPL(IdExpr, ASTVisitor);

StarExpr::StarExpr(ExprPtr what) : Expr(), what(std::move(what)) {}
StarExpr::StarExpr(const StarExpr &expr, bool clean)
    : Expr(expr, clean), what(ast::clone(expr.what, clean)) {}
std::string StarExpr::toString(int indent) const {
  return wrapType(format("star {}", what->toString(indent)));
}
ACCEPT_IMPL(StarExpr, ASTVisitor);

KeywordStarExpr::KeywordStarExpr(ExprPtr what) : Expr(), what(std::move(what)) {}
KeywordStarExpr::KeywordStarExpr(const KeywordStarExpr &expr, bool clean)
    : Expr(expr, clean), what(ast::clone(expr.what, clean)) {}
std::string KeywordStarExpr::toString(int indent) const {
  return wrapType(format("kwstar {}", what->toString(indent)));
}
ACCEPT_IMPL(KeywordStarExpr, ASTVisitor);

TupleExpr::TupleExpr(std::vector<ExprPtr> items) : Expr(), items(std::move(items)) {}
TupleExpr::TupleExpr(const TupleExpr &expr, bool clean)
    : Expr(expr, clean), items(ast::clone(expr.items, clean)) {}
std::string TupleExpr::toString(int) const {
  return wrapType(format("tuple {}", combine(items)));
}
ACCEPT_IMPL(TupleExpr, ASTVisitor);

ListExpr::ListExpr(std::vector<ExprPtr> items) : Expr(), items(std::move(items)) {}
ListExpr::ListExpr(const ListExpr &expr, bool clean)
    : Expr(expr, clean), items(ast::clone(expr.items, clean)) {}
std::string ListExpr::toString(int) const {
  return wrapType(!items.empty() ? format("list {}", combine(items)) : "list");
}
ACCEPT_IMPL(ListExpr, ASTVisitor);

SetExpr::SetExpr(std::vector<ExprPtr> items) : Expr(), items(std::move(items)) {}
SetExpr::SetExpr(const SetExpr &expr, bool clean)
    : Expr(expr, clean), items(ast::clone(expr.items, clean)) {}
std::string SetExpr::toString(int) const {
  return wrapType(!items.empty() ? format("set {}", combine(items)) : "set");
}
ACCEPT_IMPL(SetExpr, ASTVisitor);

DictExpr::DictExpr(std::vector<ExprPtr> items) : Expr(), items(std::move(items)) {
  for (auto &i : items) {
    auto t = i->getTuple();
    seqassertn(t && t->items.size() == 2, "dictionary items are invalid");
  }
}
DictExpr::DictExpr(const DictExpr &expr, bool clean)
    : Expr(expr, clean), items(ast::clone(expr.items, clean)) {}
std::string DictExpr::toString(int) const {
  return wrapType(!items.empty() ? format("dict {}", combine(items)) : "set");
}
ACCEPT_IMPL(DictExpr, ASTVisitor);

GeneratorExpr::GeneratorExpr(GeneratorExpr::GeneratorKind kind, ExprPtr expr,
                             std::vector<StmtPtr> loops)
    : Expr(), kind(kind) {
  seqassert(!loops.empty() && loops[0]->getFor(), "bad generator constructor");
  loops.push_back(
      std::make_shared<SuiteStmt>(std::make_shared<ExprStmt>(std::move(expr))));
  formCompleteStmt(loops);
}
GeneratorExpr::GeneratorExpr(ExprPtr key, ExprPtr expr, std::vector<StmtPtr> loops)
    : Expr(), kind(GeneratorExpr::DictGenerator) {
  seqassert(!loops.empty() && loops[0]->getFor(), "bad generator constructor");
  ExprPtr t = std::make_shared<TupleExpr>(
      std::vector<ExprPtr>{std::move(key), std::move(expr)});
  loops.push_back(std::make_shared<SuiteStmt>(std::make_shared<ExprStmt>(t)));
  formCompleteStmt(loops);
}
GeneratorExpr::GeneratorExpr(const GeneratorExpr &expr, bool clean)
    : Expr(expr, clean), kind(expr.kind), loops(ast::clone(expr.loops, clean)) {}
std::string GeneratorExpr::toString(int indent) const {
  auto pad = indent >= 0 ? ("\n" + std::string(indent + 2 * INDENT_SIZE, ' ')) : " ";
  std::string prefix;
  if (kind == GeneratorKind::ListGenerator)
    prefix = "list-";
  if (kind == GeneratorKind::SetGenerator)
    prefix = "set-";
  if (kind == GeneratorKind::DictGenerator)
    prefix = "dict-";
  auto l = loops->toString(indent >= 0 ? indent + 2 * INDENT_SIZE : -1);
  return wrapType(format("{}gen {}", prefix, l));
}
ACCEPT_IMPL(GeneratorExpr, ASTVisitor);
ExprPtr GeneratorExpr::getFinalExpr() {
  auto s = *(getFinalStmt());
  if (s->getExpr())
    return s->getExpr()->expr;
  return nullptr;
}
int GeneratorExpr::loopCount() const {
  int cnt = 0;
  for (StmtPtr i = loops;;) {
    if (auto sf = i->getFor()) {
      i = sf->suite;
      cnt++;
    } else if (auto si = i->getIf()) {
      i = si->ifSuite;
      cnt++;
    } else if (auto ss = i->getSuite()) {
      if (ss->stmts.empty())
        break;
      i = ss->stmts.back();
    } else
      break;
  }
  return cnt;
}
void GeneratorExpr::setFinalExpr(ExprPtr expr) {
  *(getFinalStmt()) = std::make_shared<ExprStmt>(expr);
}
void GeneratorExpr::setFinalStmt(StmtPtr stmt) { *(getFinalStmt()) = stmt; }
std::shared_ptr<Stmt> GeneratorExpr::getFinalSuite() const { return loops; }
StmtPtr *GeneratorExpr::getFinalStmt() {
  for (StmtPtr *i = &loops;;) {
    if (auto sf = (*i)->getFor())
      i = &(sf->suite);
    else if (auto si = (*i)->getIf())
      i = &(si->ifSuite);
    else if (auto ss = (*i)->getSuite()) {
      if (ss->stmts.empty())
        return i;
      i = &(ss->stmts.back());
    } else
      return i;
  }
  seqassert(false, "bad generator");
  return nullptr;
}
void GeneratorExpr::formCompleteStmt(const std::vector<StmtPtr> &loops) {
  StmtPtr final = nullptr;
  for (size_t i = loops.size(); i-- > 0;) {
    if (auto si = loops[i]->getIf())
      si->ifSuite = final;
    else if (auto sf = loops[i]->getFor())
      sf->suite = final;
    final = loops[i];
  }
  this->loops = loops[0];
}
// StmtPtr &GeneratorExpr::getFinalStmt(StmtPtr &s) {
//   if (auto i = s->getIf())
//     return getFinalStmt(i->ifSuite);
//   if (auto f = s->getFor())
//     return getFinalStmt(f->suite);
//   return s;
// }
// StmtPtr &GeneratorExpr::getFinalStmt() { return getFinalStmt(loops); }

IfExpr::IfExpr(ExprPtr cond, ExprPtr ifexpr, ExprPtr elsexpr)
    : Expr(), cond(std::move(cond)), ifexpr(std::move(ifexpr)),
      elsexpr(std::move(elsexpr)) {}
IfExpr::IfExpr(const IfExpr &expr, bool clean)
    : Expr(expr, clean), cond(ast::clone(expr.cond, clean)),
      ifexpr(ast::clone(expr.ifexpr, clean)), elsexpr(ast::clone(expr.elsexpr, clean)) {
}
std::string IfExpr::toString(int indent) const {
  return wrapType(format("if-expr {} {} {}", cond->toString(indent),
                         ifexpr->toString(indent), elsexpr->toString(indent)));
}
ACCEPT_IMPL(IfExpr, ASTVisitor);

UnaryExpr::UnaryExpr(std::string op, ExprPtr expr)
    : Expr(), op(std::move(op)), expr(std::move(expr)) {}
UnaryExpr::UnaryExpr(const UnaryExpr &expr, bool clean)
    : Expr(expr, clean), op(expr.op), expr(ast::clone(expr.expr, clean)) {}
std::string UnaryExpr::toString(int indent) const {
  return wrapType(format("unary \"{}\" {}", op, expr->toString(indent)));
}
ACCEPT_IMPL(UnaryExpr, ASTVisitor);

BinaryExpr::BinaryExpr(ExprPtr lexpr, std::string op, ExprPtr rexpr, bool inPlace)
    : Expr(), op(std::move(op)), lexpr(std::move(lexpr)), rexpr(std::move(rexpr)),
      inPlace(inPlace) {}
BinaryExpr::BinaryExpr(const BinaryExpr &expr, bool clean)
    : Expr(expr, clean), op(expr.op), lexpr(ast::clone(expr.lexpr, clean)),
      rexpr(ast::clone(expr.rexpr, clean)), inPlace(expr.inPlace) {}
std::string BinaryExpr::toString(int indent) const {
  return wrapType(format("binary \"{}\" {} {}{}", op, lexpr->toString(indent),
                         rexpr->toString(indent), inPlace ? " #:in-place" : ""));
}
ACCEPT_IMPL(BinaryExpr, ASTVisitor);

ChainBinaryExpr::ChainBinaryExpr(std::vector<std::pair<std::string, ExprPtr>> exprs)
    : Expr(), exprs(std::move(exprs)) {}
ChainBinaryExpr::ChainBinaryExpr(const ChainBinaryExpr &expr, bool clean)
    : Expr(expr, clean) {
  for (auto &e : expr.exprs)
    exprs.emplace_back(make_pair(e.first, ast::clone(e.second, clean)));
}
std::string ChainBinaryExpr::toString(int indent) const {
  std::vector<std::string> s;
  for (auto &i : exprs)
    s.push_back(format("({} \"{}\")", i.first, i.second->toString(indent)));
  return wrapType(format("chain {}", join(s, " ")));
}
ACCEPT_IMPL(ChainBinaryExpr, ASTVisitor);

PipeExpr::Pipe PipeExpr::Pipe::clone(bool clean) const {
  return {op, ast::clone(expr, clean)};
}

PipeExpr::PipeExpr(std::vector<PipeExpr::Pipe> items)
    : Expr(), items(std::move(items)) {
  for (auto &i : this->items) {
    if (auto call = i.expr->getCall()) {
      for (auto &a : call->args)
        if (auto el = a.value->getEllipsis())
          el->mode = EllipsisExpr::PIPE;
    }
  }
}
PipeExpr::PipeExpr(const PipeExpr &expr, bool clean)
    : Expr(expr, clean), items(ast::clone(expr.items, clean)), inTypes(expr.inTypes) {}
void PipeExpr::validate() const {}
std::string PipeExpr::toString(int indent) const {
  std::vector<std::string> s;
  for (auto &i : items)
    s.push_back(format("({} \"{}\")", i.expr->toString(indent), i.op));
  return wrapType(format("pipe {}", join(s, " ")));
}
ACCEPT_IMPL(PipeExpr, ASTVisitor);

IndexExpr::IndexExpr(ExprPtr expr, ExprPtr index)
    : Expr(), expr(std::move(expr)), index(std::move(index)) {}
IndexExpr::IndexExpr(const IndexExpr &expr, bool clean)
    : Expr(expr, clean), expr(ast::clone(expr.expr, clean)),
      index(ast::clone(expr.index, clean)) {}
std::string IndexExpr::toString(int indent) const {
  return wrapType(
      format("index {} {}", expr->toString(indent), index->toString(indent)));
}
ACCEPT_IMPL(IndexExpr, ASTVisitor);

CallExpr::Arg CallExpr::Arg::clone(bool clean) const {
  return {name, ast::clone(value, clean)};
}
CallExpr::Arg::Arg(const SrcInfo &info, const std::string &name, ExprPtr value)
    : name(name), value(value) {
  setSrcInfo(info);
}
CallExpr::Arg::Arg(const std::string &name, ExprPtr value) : name(name), value(value) {
  if (value)
    setSrcInfo(value->getSrcInfo());
}
CallExpr::Arg::Arg(ExprPtr value) : CallExpr::Arg("", value) {}

CallExpr::CallExpr(const CallExpr &expr, bool clean)
    : Expr(expr, clean), expr(ast::clone(expr.expr, clean)),
      args(ast::clone(expr.args, clean)), ordered(expr.ordered) {}
CallExpr::CallExpr(ExprPtr expr, std::vector<CallExpr::Arg> args)
    : Expr(), expr(std::move(expr)), args(std::move(args)), ordered(false) {
  validate();
}
CallExpr::CallExpr(ExprPtr expr, std::vector<ExprPtr> args)
    : expr(std::move(expr)), ordered(false) {
  for (auto &a : args)
    if (a)
      this->args.push_back({"", std::move(a)});
  validate();
}
void CallExpr::validate() const {
  bool namesStarted = false, foundEllipsis = false;
  for (auto &a : args) {
    if (a.name.empty() && namesStarted &&
        !(CAST(a.value, KeywordStarExpr) || a.value->getEllipsis()))
      E(Error::CALL_NAME_ORDER, a.value);
    if (!a.name.empty() && (a.value->getStar() || CAST(a.value, KeywordStarExpr)))
      E(Error::CALL_NAME_STAR, a.value);
    if (a.value->getEllipsis() && foundEllipsis)
      E(Error::CALL_ELLIPSIS, a.value);
    foundEllipsis |= bool(a.value->getEllipsis());
    namesStarted |= !a.name.empty();
  }
}
std::string CallExpr::toString(int indent) const {
  std::vector<std::string> s;
  auto pad = indent >= 0 ? ("\n" + std::string(indent + 2 * INDENT_SIZE, ' ')) : " ";
  for (auto &i : args) {
    if (i.name.empty())
      s.emplace_back(pad + format("#:name '{}", i.name));
    s.emplace_back(pad +
                   i.value->toString(indent >= 0 ? indent + 2 * INDENT_SIZE : -1));
  }
  return wrapType(format("call {}{}", expr->toString(indent), fmt::join(s, "")));
}
ACCEPT_IMPL(CallExpr, ASTVisitor);

DotExpr::DotExpr(ExprPtr expr, std::string member)
    : Expr(), expr(std::move(expr)), member(std::move(member)) {}
DotExpr::DotExpr(const std::string &left, std::string member)
    : Expr(), expr(std::make_shared<IdExpr>(left)), member(std::move(member)) {}
DotExpr::DotExpr(const DotExpr &expr, bool clean)
    : Expr(expr, clean), expr(ast::clone(expr.expr, clean)), member(expr.member) {}
std::string DotExpr::toString(int indent) const {
  return wrapType(format("dot {} '{}", expr->toString(indent), member));
}
ACCEPT_IMPL(DotExpr, ASTVisitor);

SliceExpr::SliceExpr(ExprPtr start, ExprPtr stop, ExprPtr step)
    : Expr(), start(std::move(start)), stop(std::move(stop)), step(std::move(step)) {}
SliceExpr::SliceExpr(const SliceExpr &expr, bool clean)
    : Expr(expr, clean), start(ast::clone(expr.start, clean)),
      stop(ast::clone(expr.stop, clean)), step(ast::clone(expr.step, clean)) {}
std::string SliceExpr::toString(int indent) const {
  return wrapType(format("slice{}{}{}",
                         start ? format(" #:start {}", start->toString(indent)) : "",
                         stop ? format(" #:end {}", stop->toString(indent)) : "",
                         step ? format(" #:step {}", step->toString(indent)) : ""));
}
ACCEPT_IMPL(SliceExpr, ASTVisitor);

EllipsisExpr::EllipsisExpr(EllipsisType mode) : Expr(), mode(mode) {}
EllipsisExpr::EllipsisExpr(const EllipsisExpr &expr, bool clean)
    : Expr(expr, clean), mode(expr.mode) {}
std::string EllipsisExpr::toString(int) const {
  return wrapType(format(
      "ellipsis{}", mode == PIPE ? " #:pipe" : (mode == PARTIAL ? "#:partial" : "")));
}
ACCEPT_IMPL(EllipsisExpr, ASTVisitor);

LambdaExpr::LambdaExpr(std::vector<std::string> vars, ExprPtr expr)
    : Expr(), vars(std::move(vars)), expr(std::move(expr)) {}
LambdaExpr::LambdaExpr(const LambdaExpr &expr, bool clean)
    : Expr(expr, clean), vars(expr.vars), expr(ast::clone(expr.expr, clean)) {}
std::string LambdaExpr::toString(int indent) const {
  return wrapType(format("lambda ({}) {}", join(vars, " "), expr->toString(indent)));
}
ACCEPT_IMPL(LambdaExpr, ASTVisitor);

YieldExpr::YieldExpr() : Expr() {}
YieldExpr::YieldExpr(const YieldExpr &expr, bool clean) : Expr(expr, clean) {}
std::string YieldExpr::toString(int) const { return "yield-expr"; }
ACCEPT_IMPL(YieldExpr, ASTVisitor);

AssignExpr::AssignExpr(ExprPtr var, ExprPtr expr)
    : Expr(), var(std::move(var)), expr(std::move(expr)) {}
AssignExpr::AssignExpr(const AssignExpr &expr, bool clean)
    : Expr(expr, clean), var(ast::clone(expr.var, clean)),
      expr(ast::clone(expr.expr, clean)) {}
std::string AssignExpr::toString(int indent) const {
  return wrapType(
      format("assign-expr '{} {}", var->toString(indent), expr->toString(indent)));
}
ACCEPT_IMPL(AssignExpr, ASTVisitor);

RangeExpr::RangeExpr(ExprPtr start, ExprPtr stop)
    : Expr(), start(std::move(start)), stop(std::move(stop)) {}
RangeExpr::RangeExpr(const RangeExpr &expr, bool clean)
    : Expr(expr, clean), start(ast::clone(expr.start, clean)),
      stop(ast::clone(expr.stop, clean)) {}
std::string RangeExpr::toString(int indent) const {
  return wrapType(
      format("range {} {}", start->toString(indent), stop->toString(indent)));
}
ACCEPT_IMPL(RangeExpr, ASTVisitor);

StmtExpr::StmtExpr(std::vector<std::shared_ptr<Stmt>> stmts, ExprPtr expr)
    : Expr(), stmts(std::move(stmts)), expr(std::move(expr)) {}
StmtExpr::StmtExpr(std::shared_ptr<Stmt> stmt, ExprPtr expr)
    : Expr(), expr(std::move(expr)) {
  stmts.push_back(std::move(stmt));
}
StmtExpr::StmtExpr(std::shared_ptr<Stmt> stmt, std::shared_ptr<Stmt> stmt2,
                   ExprPtr expr)
    : Expr(), expr(std::move(expr)) {
  stmts.push_back(std::move(stmt));
  stmts.push_back(std::move(stmt2));
}
StmtExpr::StmtExpr(const StmtExpr &expr, bool clean)
    : Expr(expr, clean), stmts(ast::clone(expr.stmts, clean)),
      expr(ast::clone(expr.expr, clean)) {}
std::string StmtExpr::toString(int indent) const {
  auto pad = indent >= 0 ? ("\n" + std::string(indent + 2 * INDENT_SIZE, ' ')) : " ";
  std::vector<std::string> s;
  for (auto &i : stmts)
    s.emplace_back(pad + i->toString(indent >= 0 ? indent + 2 * INDENT_SIZE : -1));
  return wrapType(
      format("stmt-expr {} ({})", expr->toString(indent), fmt::join(s, "")));
}
ACCEPT_IMPL(StmtExpr, ASTVisitor);

InstantiateExpr::InstantiateExpr(ExprPtr typeExpr, std::vector<ExprPtr> typeParams)
    : Expr(), typeExpr(std::move(typeExpr)), typeParams(std::move(typeParams)) {}
InstantiateExpr::InstantiateExpr(ExprPtr typeExpr, ExprPtr typeParam)
    : Expr(), typeExpr(std::move(typeExpr)) {
  typeParams.push_back(std::move(typeParam));
}
InstantiateExpr::InstantiateExpr(const InstantiateExpr &expr, bool clean)
    : Expr(expr, clean), typeExpr(ast::clone(expr.typeExpr, clean)),
      typeParams(ast::clone(expr.typeParams, clean)) {}
std::string InstantiateExpr::toString(int indent) const {
  return wrapType(
      format("instantiate {} {}", typeExpr->toString(indent), combine(typeParams)));
}
ACCEPT_IMPL(InstantiateExpr, ASTVisitor);

StaticValue::Type getStaticGeneric(Expr *e) {
  if (e && e->getIndex() && e->getIndex()->expr->isId("Static")) {
    if (e->getIndex()->index && e->getIndex()->index->isId("str"))
      return StaticValue::Type::STRING;
    if (e->getIndex()->index && e->getIndex()->index->isId("int"))
      return StaticValue::Type::INT;
    return StaticValue::Type::NOT_SUPPORTED;
  }
  return StaticValue::Type::NOT_STATIC;
}

} // namespace codon::ast
