// Copyright [2020] <Copyright Kevin, kevin.lau.gd@gmail.com>

#include "AlgoTrade/StrategyEngine.h"

#include <cassert>
#include <set>
#include <vector>

#include <spdlog/spdlog.h>

#include "AlgoTrade/Strategy.h"
#include "Base/DataStruct.h"
#include "RiskManagement/NoSelfTrade.h"
#include "RiskManagement/VelocityLimit.h"

namespace ft {

StrategyEngine::StrategyEngine()
  : engine_(new EventEngine) {
  engine_->set_handler(EV_ACCOUNT, MEM_HANDLER(StrategyEngine::on_account));
  engine_->set_handler(EV_POSITION, MEM_HANDLER(StrategyEngine::on_position));
  engine_->set_handler(EV_ORDER, MEM_HANDLER(StrategyEngine::on_order));
  engine_->set_handler(EV_TRADE, MEM_HANDLER(StrategyEngine::on_trade));
  engine_->set_handler(EV_TICK, MEM_HANDLER(StrategyEngine::on_tick));
  engine_->set_handler(EV_MOUNT_STRATEGY, MEM_HANDLER(StrategyEngine::on_mount_strategy));
  engine_->set_handler(EV_UMOUNT_STRATEGY, MEM_HANDLER(StrategyEngine::on_unmount_strategy));
  engine_->set_handler(EV_SYNC, MEM_HANDLER(StrategyEngine::on_sync));

  risk_mgr_.add_rule(std::make_shared<NoSelfTradeRule>(&panel_));
  risk_mgr_.add_rule(std::make_shared<VelocityLimit>(1000, 10, 200));
}

StrategyEngine::~StrategyEngine() {
}

void StrategyEngine::close() {
  api_->logout();
  engine_->stop();
}

bool StrategyEngine::login(const LoginParams& params) {
  api_.reset(create_api("ctp", engine_.get()));
  if (!api_) {
    spdlog::error("[StrategyEngine::login] Unknown api");
    return false;
  }

  if (!api_->login(params)) {
    spdlog::error("[StrategyEngine::login] Failed to login");
    return false;
  }

  is_login_ = true;
  spdlog::info("[StrategyEngine::login] Login as {}", params.investor_id());

  engine_->run(false);

  if (!api_->query_account()) {
    spdlog::error("[StrategyEngine::login] Failed to query account");
    return false;
  }

  // query all positions
  is_process_pos_done_ = false;
  if (!api_->query_positions()) {
    spdlog::error("[StrategyEngine::login] Failed to query positions");
    return false;
  }

  engine_->post(EV_SYNC);
  while (!is_process_pos_done_)
    continue;

  for (auto& ticker : params.subscribed_list())
    tick_datahub_.emplace(ticker, TickDatabase(ticker));

  return true;
}

std::string StrategyEngine::send_order(const std::string& ticker, int volume,
                                       Direction direction, Offset offset,
                                       OrderType type, double price) {
  Order order(ticker, direction, offset, volume, type, price);
  order.status = OrderStatus::SUBMITTING;

  if (!risk_mgr_.check(&order))
    return "";

  order.order_id = api_->send_order(&order);
  if (order.order_id.empty()) {
    spdlog::error("[StrategyEngine] send_order. Ticker: {}, Volume: {}, Type: {}, Price: {:.2f}, "
                  "Direction: {}, Offset: {}",
                  ticker, volume, to_string(type), price,
                  to_string(direction), to_string(offset));
    return "";
  }

  panel_.new_order(&order);
  panel_.update_pos_pending(ticker, direction, offset, volume);

  spdlog::debug("[StrategyEngine] send_order. Ticker: {}, Volume: {}, Type: {}, Price: {:.2f}, "
                "Direction: {}, Offset: {}",
                ticker, volume, to_string(type), price,
                to_string(direction), to_string(offset));

  return order.order_id;
}

bool StrategyEngine::cancel_order(const std::string& order_id) {
  const Order* order = panel_.get_order_by_id(order_id);
  if (!order) {
    spdlog::error("[StrategyEngine] CancelOrder failed: order not found");
    return false;
  }

  if (!api_->cancel_order(order_id)) {
    spdlog::error("[StrategyEngine] cancel_order. Failed: unknown error");
    return false;
  }

  spdlog::debug("[StrategyEngine] cancel_order. OrderID: {}, Ticker: {}, LeftVolume: {}",
                order_id, order->ticker, order->volume - order->volume_traded);
  return true;
}

void StrategyEngine::mount_strategy(const std::string& ticker,
                                    Strategy* strategy) {
  strategy->set_ctx(new AlgoTradeContext(ticker, this));
  engine_->post(EV_MOUNT_STRATEGY, strategy);
}

void StrategyEngine::on_mount_strategy(cppex::Any* data) {
  auto* strategy = data->fetch<Strategy>().release();
  auto& list = strategies_[strategy->get_ctx()->this_ticker()];

  if (strategy->on_init(strategy->get_ctx()))
    list.emplace_back(strategy);
  else
    engine_->post(EV_MOUNT_STRATEGY, strategy);
}

void StrategyEngine::unmount_strategy(Strategy* strategy) {
  engine_->post(EV_UMOUNT_STRATEGY, strategy);
}

void StrategyEngine::on_unmount_strategy(cppex::Any* data) {
  auto* strategy = data->fetch<Strategy>().release();

  auto ctx = strategy->get_ctx();
  if (!ctx)
    return;

  auto iter = strategies_.find(ctx->this_ticker());
  if (iter == strategies_.end())
    return;

  auto& list = iter->second;
  for (auto iter = list.begin(); iter != list.end(); ++iter) {
    if (*iter == strategy) {
      strategy->on_exit(ctx);
      strategy->set_ctx(nullptr);
      list.erase(iter);
      return;
    }
  }
}

void StrategyEngine::on_sync(cppex::Any*) {
  is_process_pos_done_ = true;
}

void StrategyEngine::on_tick(cppex::Any* data) {
  auto* tick = data->fetch<TickData>().release();

  auto db_iter = tick_datahub_.find(tick->ticker);
  if (db_iter == tick_datahub_.end()) {
    auto res = tick_datahub_.emplace(tick->ticker, TickDatabase(tick->ticker));
    res.first->second.on_tick(tick);
  } else {
    db_iter->second.on_tick(tick);
  }

  auto candle_iter = candle_charts_.find(tick->ticker);
  if (candle_iter != candle_charts_.end())
    candle_iter->second.on_tick(tick);

  panel_.update_pos_pnl(tick->ticker, tick->last_price);

  auto iter = strategies_.find(tick->ticker);
  if (iter != strategies_.end()) {
    auto& strategy_list = iter->second;
    for (auto& strategy : strategy_list)
      strategy->on_tick(strategy->get_ctx());
  }
}

void StrategyEngine::on_position(cppex::Any* data) {
  if (is_process_pos_done_)
    return;

  const auto* position = data->cast<Position>();
  auto& lp = position->long_pos;
  auto& sp = position->short_pos;
  spdlog::info("[StrategyEngine] on_position. Query position success. Ticker: {}, "
               "Long Volume: {}, Long Price: {:.2f}, Long Frozen: {}, Long PNL: {}, "
               "Short Volume: {}, Short Price: {:.2f}, Short Frozen: {}, Short PNL: {}",
               position->ticker,
               lp.volume, lp.cost_price, lp.frozen, lp.pnl,
               sp.volume, sp.cost_price, sp.frozen, sp.pnl);

  if (lp.volume == 0 && lp.frozen == 0 && sp.volume == 0 && sp.frozen == 0)
    return;

  panel_.on_query_position(position);
}

void StrategyEngine::on_account(cppex::Any* data) {
  auto* account = data->cast<Account>();
  panel_.on_query_account(account);
  spdlog::info("[StrategyEngine] on_account. Account ID: {}, Balance: {}, Fronzen: {}",
               account->account_id, account->balance, account->frozen);
}

void StrategyEngine::on_trade(cppex::Any* data) {
  auto* trade = data->cast<Trade>();
  spdlog::debug("[StrategyEngine] on_trade. Ticker: {}, Order ID: {}, Trade ID: {}, "
                "Direction: {}, Offset: {}, Price: {:.2f}, Volume: {}",
                trade->ticker, trade->order_id, trade->trade_id,
                to_string(trade->direction), to_string(trade->offset),
                trade->price, trade->volume);

  panel_.new_trade(trade);
  panel_.update_pos_traded(trade->ticker, trade->direction, trade->offset,
                                  trade->volume, trade->price);
}

void StrategyEngine::on_order(cppex::Any* data) {
  const auto* order = data->cast<Order>();
  panel_.update_order(order);

  // TODO(kevin): 对策略发的单加个ID，只把订单回执返回给发该单的策略
  for (auto& [ticker, strategy_list] : strategies_) {
    for (auto strategy : strategy_list)
      strategy->on_order(strategy->get_ctx(), order);
  }
}

}  // namespace ft
