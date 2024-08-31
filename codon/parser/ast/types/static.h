// Copyright (C) 2022-2024 Exaloop Inc. <https://exaloop.io>

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "codon/parser/ast/types/class.h"

namespace codon::ast::types {

struct StaticType : public ClassType {
  explicit StaticType(Cache *, const std::string &);

public:
  TypePtr generalize(int atLevel) override;
  TypePtr instantiate(int atLevel, int *unboundCount,
                      std::unordered_map<int, TypePtr> *cache) override;
  bool canRealize() const override;
  bool isInstantiated() const override;
  std::string realizedName() const override;
  virtual Expr *getStaticExpr() const = 0;
  virtual Type *getNonStaticType() const;
  StaticType *getStatic() override { return this; }
};

struct IntStaticType : public StaticType {
  int64_t value;

public:
  explicit IntStaticType(Cache *cache, int64_t);

public:
  int unify(Type *typ, Unification *undo) override;

public:
  std::string debugString(char mode) const override;
  Expr *getStaticExpr() const override;

  IntStaticType *getIntStatic() override { return this; }
};

struct StrStaticType : public StaticType {
  std::string value;

public:
  explicit StrStaticType(Cache *cache, std::string);

public:
  int unify(Type *typ, Unification *undo) override;

public:
  std::string debugString(char mode) const override;
  Expr *getStaticExpr() const override;

  StrStaticType *getStrStatic() override { return this; }
};

struct BoolStaticType : public StaticType {
  bool value;

public:
  explicit BoolStaticType(Cache *cache, bool);

public:
  int unify(Type *typ, Unification *undo) override;

public:
  std::string debugString(char mode) const override;
  Expr *getStaticExpr() const override;

  BoolStaticType *getBoolStatic() override { return this; }
};

using StaticTypePtr = std::shared_ptr<StaticType>;
using IntStaticTypePtr = std::shared_ptr<IntStaticType>;
using StrStaticTypePtr = std::shared_ptr<StrStaticType>;

} // namespace codon::ast::types
