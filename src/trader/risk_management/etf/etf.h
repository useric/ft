// Copyright [2020] <Copyright Kevin, kevin.lau.gd@gmail.com>

#ifndef FT_SRC_TRADER_RISK_MANAGEMENT_ETF_ETF_H_
#define FT_SRC_TRADER_RISK_MANAGEMENT_ETF_ETF_H_

#include <unordered_map>

#include "ft/base/trade_msg.h"

namespace ft {

enum ReplaceType {
  FORBIDDEN = 0,
  OPTIONAL = 1,
  MUST = 2,
  RECOMPUTE = 3,
};

struct ComponentStock {
  const Contract* contract;
  const Contract* etf_contract;
  uint32_t replace_type;
  int volume;
  int replace_amount;
};

struct ETF {
  const Contract* contract;
  bool purchase_allowed;
  bool redeem_allowed;
  int unit;
  double max_cash_ratio;
  double cash_component;
  double must_cash_substitution;

  std::unordered_map<uint32_t, ComponentStock> components;
};

}  // namespace ft

#endif  // FT_SRC_TRADING_SERVER_RISK_MANAGEMENT_ETF_ETF_H_
