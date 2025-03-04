// Copyright [2020] <Copyright Kevin, kevin.lau.gd@gmail.com>

#include "trader/risk_management/etf/arbitrage_manager.h"

#include "trader/risk_management/etf/etf_table.h"

namespace ft {

bool ArbitrageManager::Init(RiskRuleParams* params) {
  if (params->config->arg0.empty() || params->config->arg1.empty()) return false;

  account_ = params->account;
  pos_calculator_ = params->pos_calculator;
  order_map_ = params->order_map;

  return EtfTable::Init(params->config->arg0, params->config->arg1);
}

int ArbitrageManager::CheckOrderRequest(const Order* order) { return NO_ERROR; }

}  // namespace ft
