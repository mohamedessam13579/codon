// Copyright (C) 2022-2023 Exaloop Inc. <https://exaloop.io>

#include <memory>
#include <string>
#include <vector>

#include "codon/parser/ast/types/class.h"
#include "codon/parser/visitors/typecheck/typecheck.h"

namespace codon::ast::types {

ClassType::ClassType(Cache *cache, std::string name, std::string niceName,
                     std::vector<Generic> generics, std::vector<Generic> hiddenGenerics)
    : Type(cache), name(std::move(name)), niceName(std::move(niceName)),
      generics(std::move(generics)), hiddenGenerics(std::move(hiddenGenerics)) {}
ClassType::ClassType(const ClassTypePtr &base)
    : Type(base), name(base->name), niceName(base->niceName), generics(base->generics),
      hiddenGenerics(base->hiddenGenerics), isTuple(base->isTuple) {}

int ClassType::unify(Type *typ, Unification *us) {
  if (auto tc = typ->getClass()) {
    if (name == "int" && tc->name == "Int")
      return tc->unify(this, us);
    if (tc->name == "int" && name == "Int") {
      auto t64 = std::make_shared<IntStaticType>(cache, 64);
      return generics[0].type->unify(t64.get(), us);
    }
    // Check names.
    if (name != tc->name)
      return -1;
    // Check generics.
    int s1 = 3, s = 0;
    if (generics.size() != tc->generics.size())
      return -1;
    for (int i = 0; i < generics.size(); i++) {
      if ((s = generics[i].type->unify(tc->generics[i].type.get(), us)) == -1) {
        return -1;
      }
      s1 += s;
    }
    return s1;
  } else if (auto tl = typ->getLink()) {
    return tl->unify(this, us);
  } else {
    return -1;
  }
}

TypePtr ClassType::generalize(int atLevel) {
  auto g = generics, hg = hiddenGenerics;
  for (auto &t : g)
    t.type = t.type ? t.type->generalize(atLevel) : nullptr;
  for (auto &t : hg)
    t.type = t.type ? t.type->generalize(atLevel) : nullptr;
  auto c = std::make_shared<ClassType>(cache, name, niceName, g, hg);
  c->isTuple = isTuple;
  c->setSrcInfo(getSrcInfo());
  return c;
}

TypePtr ClassType::instantiate(int atLevel, int *unboundCount,
                               std::unordered_map<int, TypePtr> *cache) {
  auto g = generics, hg = hiddenGenerics;
  for (auto &t : g)
    t.type = t.type ? t.type->instantiate(atLevel, unboundCount, cache) : nullptr;
  for (auto &t : hg)
    t.type = t.type ? t.type->instantiate(atLevel, unboundCount, cache) : nullptr;
  auto c = std::make_shared<ClassType>(this->cache, name, niceName, g, hg);
  c->isTuple = isTuple;
  c->setSrcInfo(getSrcInfo());
  return c;
}

bool ClassType::hasUnbounds(bool includeGenerics) const {
  for (auto &t : generics)
    if (t.type && t.type->hasUnbounds(includeGenerics))
      return true;
  for (auto &t : hiddenGenerics)
    if (t.type && t.type->hasUnbounds(includeGenerics))
      return true;
  return false;
}

std::vector<TypePtr> ClassType::getUnbounds() const {
  std::vector<TypePtr> u;
  for (auto &t : generics)
    if (t.type) {
      auto tu = t.type->getUnbounds();
      u.insert(u.begin(), tu.begin(), tu.end());
    }
  for (auto &t : hiddenGenerics)
    if (t.type) {
      auto tu = t.type->getUnbounds();
      u.insert(u.begin(), tu.begin(), tu.end());
    }
  return u;
}

bool ClassType::canRealize() const {
  if (name == "type") {
    if (!hasUnbounds())
      return true; // always true!
  }
  return std::all_of(generics.begin(), generics.end(),
                     [](auto &t) { return !t.type || t.type->canRealize(); }) &&
         std::all_of(hiddenGenerics.begin(), hiddenGenerics.end(),
                     [](auto &t) { return !t.type || t.type->canRealize(); });
}

bool ClassType::isInstantiated() const {
  return std::all_of(generics.begin(), generics.end(),
                     [](auto &t) { return !t.type || t.type->isInstantiated(); }) &&
         std::all_of(hiddenGenerics.begin(), hiddenGenerics.end(),
                     [](auto &t) { return !t.type || t.type->isInstantiated(); });
}

std::shared_ptr<ClassType> ClassType::getHeterogenousTuple() {
  seqassert(canRealize(), "{} not realizable", toString());
  seqassert(startswith(name, TYPE_TUPLE), "{} not a tuple", toString());
  if (generics.size() > 1) {
    std::string first = generics[0].type->realizedName();
    for (int i = 1; i < generics.size(); i++)
      if (generics[i].type->realizedName() != first)
        return getClass();
  }
  return nullptr;
}

std::string ClassType::debugString(char mode) const {
  if (name == "Partial" && generics[0].type->canRealize() && mode != 2) {
    auto func = getPartialFunc();
    std::vector<std::string> gs;
    for (auto &a : generics[2].type->getClass()->generics)
      gs.push_back(a.type->debugString(mode));
    std::vector<std::string> as;
    int i = 0, gi = 0;
    auto known = getPartialMask();
    for (; i < known.size(); i++)
      if (func->ast->args[i].status == Param::Normal) {
        if (!known[i])
          as.emplace_back("...");
        else
          as.emplace_back(gs[gi++]);
      }
    auto fnname = func->ast->name;
    if (mode == 0) {
      fnname = cache->rev(func->ast->name);
      // fnname = fmt::format("{}.{}", func->funcParent->debugString(mode), fnname);
    } else if (mode == 2) {
      fnname = func->debugString(mode);
    }
    return fmt::format("{}[{}{}]", fnname, join(as, ","),
                       //  mode == 2 ? fmt::format(";{}", join(gs, ",")) :
                       "");
  }

  std::vector<std::string> gs;
  for (auto &a : generics)
    if (!a.name.empty())
      gs.push_back(a.type->debugString(mode));
  if ((mode == 2) && !hiddenGenerics.empty()) {
    for (auto &a : hiddenGenerics)
      if (!a.name.empty())
        gs.push_back("-" + a.type->debugString(mode));
  }
  // Special formatting for Functions and Tuples
  auto n = mode == 0 ? niceName : name;
  if (startswith(n, TYPE_TUPLE))
    n = "Tuple";
  return fmt::format("{}{}", n, gs.empty() ? "" : fmt::format("[{}]", join(gs, ",")));
}

std::string ClassType::realizedName() const {
  if (!_rn.empty())
    return _rn;

  std::string s;
  if (name == "Partial" && generics[0].type->canRealize() && false) {
    auto func = getPartialFunc();
    std::vector<std::string> gs;
    gs.push_back(func->ast->name);
    for (auto &a : func->generics)
      if (!a.name.empty())
        gs.push_back(a.type->realizedName());
    s = join(gs, ",");
    s = fmt::format("{}{}", name, s.empty() ? "" : fmt::format("[{}]", s));
  } else {
    std::vector<std::string> gs;
    for (auto &a : generics)
      if (!a.name.empty()) {
        if (!a.isStatic && a.type->getStatic()) {
          gs.push_back(a.type->getStatic()->name);
        } else {
          gs.push_back(a.type->realizedName());
        }
      }
    s = join(gs, ",");
    s = fmt::format("{}{}", name, s.empty() ? "" : fmt::format("[{}]", s));
  }
  return s;
}

std::shared_ptr<ClassType> ClassType::getPartial() {
  return (name == "Partial") ? std::static_pointer_cast<ClassType>(shared_from_this())
                             : nullptr;
}

std::shared_ptr<FuncType> ClassType::getPartialFunc() const {
  seqassert(name == "Partial" && generics[0].type->canRealize(), "not a partial");
  auto n = generics[0].type->getStrStatic()->value;
  auto f = in(cache->functions, n);
  seqassert(f, "cannot locate '{}'", n);
  return f->type;
}

std::vector<char> ClassType::getPartialMask() const {
  seqassert(name == "Partial" && generics[1].type->canRealize(), "not a partial");
  auto n = generics[1].type->getStrStatic()->value;
  std::vector<char> r(n.size(), 0);
  for (size_t i = 0; i < n.size(); i++)
    if (n[i] == '1')
      r[i] = 1;
  return r;
}

} // namespace codon::ast::types
