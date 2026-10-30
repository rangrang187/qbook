#include "CoreTrial.h"
#include "Measure.h"
#include "Workloads.h"
#include "Orderbook.h"
#include "support/OrderbookInspector.h"

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace qbook::bench {
namespace {
using Key = std::pair<qlib::side, std::int64_t>;
struct ExpectedBook {
  std::map<std::int64_t, qlib::order> orders;
  std::map<Key, std::deque<std::int64_t>> fifo;
  std::map<Key, std::int64_t> l2;
  std::int64_t event_ns = 0, recv_ns = 0;
};
struct CorePayload {
  CaseSpec spec;
  std::vector<qlib::feed_event> initial, events;
  std::vector<ExpectedBook> initial_expected, expected;
  std::array<std::uint64_t, 10> expected_reasons{};
  std::uint64_t expected_applied = 0;
};
struct CoreTrial { std::vector<std::unique_ptr<Orderbook>> books; };
void Require(bool condition, std::string_view reason) { if (!condition) throw std::runtime_error(std::string(reason)); }
auto Fields(const qlib::order& v) {
  return std::tie(v.seq,v.order_id,v.price,v.qty,v.new_qty,v.instrument_id,v.side,v.type,v.action,v.event_ns,v.recv_ns);
}
qlib::order NewOrder(std::int64_t id, qlib::side side, std::int64_t price,
                     std::int64_t qty, std::uint32_t instrument, std::int64_t seq) {
  qlib::order result{}; result.seq=seq; result.order_id=id; result.price=price; result.qty=qty;
  result.instrument_id=instrument; result.side=side; result.type=qlib::order_type::limit;
  result.action=qlib::order_action::add; result.event_ns=seq+1000; result.recv_ns=seq+2000;
  return result;
}
void Touch(ExpectedBook& book, const qlib::feed_event& event) {
  std::visit([&](const auto& v){book.event_ns=v.event_ns; book.recv_ns=v.recv_ns;}, event);
}
void ExpectedAdd(ExpectedBook& book, const qlib::order& value) {
  Require(book.orders.emplace(value.order_id,value).second,"reference duplicate order");
  book.fifo[{value.side,value.price}].push_back(value.order_id);
}
void ExpectedErase(ExpectedBook& book, std::int64_t id) {
  const auto value=book.orders.at(id); auto& fifo=book.fifo.at({value.side,value.price});
  const auto pos=std::find(fifo.begin(),fifo.end(),id); Require(pos!=fifo.end(),"reference missing FIFO id");
  fifo.erase(pos); if(fifo.empty()) book.fifo.erase({value.side,value.price}); book.orders.erase(id);
}
void ExpectedMove(ExpectedBook& book,std::int64_t id,std::int64_t price,std::int64_t qty) {
  auto value=book.orders.at(id); ExpectedErase(book,id); value.price=price; value.qty=qty; ExpectedAdd(book,value);
}
qlib::book ExpectedSnapshot(const ExpectedBook& book,std::uint32_t instrument,BookMode mode) {
  qlib::book result{}; result.instrument_id=instrument;result.event_ns=book.event_ns;result.recv_ns=book.recv_ns;
  std::map<Key,std::int64_t> levels=book.l2;
  if(mode==BookMode::Mbo) for(const auto& [id,v]:book.orders) levels[{v.side,v.price}]+=v.qty;
  for(const auto& [key,qty]:levels) {
    if(key.first==qlib::side::buy && key.second>result.bid_price[0]) {result.bid_price[0]=key.second;result.bid_qty[0]=qty;}
    if(key.first==qlib::side::sell && (result.ask_price[0]==0 || key.second<result.ask_price[0])) {result.ask_price[0]=key.second;result.ask_qty[0]=qty;}
  }
  return result;
}
bool SameBook(const qlib::book& a,const qlib::book& b) {
  return std::tie(a.event_ns,a.recv_ns,a.instrument_id,a.bid_price[0],a.bid_qty[0],a.ask_price[0],a.ask_qty[0])==
         std::tie(b.event_ns,b.recv_ns,b.instrument_id,b.bid_price[0],b.bid_qty[0],b.ask_price[0],b.ask_qty[0]);
}
void ValidateBooks(const CoreTrial& trial,const std::vector<ExpectedBook>& expected,BookMode mode) {
  using Inspector=qbook::testing::OrderbookInspector;
  for(std::size_t i=0;i<expected.size();++i) {
    const auto& actual=*trial.books[i]; const auto& want=expected[i];
    const auto state=Inspector::SnapshotState(actual);
    Require(state.nodes.size()==want.orders.size(),"core order count differs");
    for(const auto& [id,value]:want.orders) {
      const auto found=state.nodes.find(id);Require(found!=state.nodes.end(),"core missing expected order");
      Require(Fields(*found->second.address)==Fields(value),"core order scalar fields differ");
    }
    std::map<Key,std::int64_t> totals=want.l2;
    if(mode==BookMode::Mbo) for(const auto& [id,v]:want.orders) totals[{v.side,v.price}]+=v.qty;
    Require(state.bids.size()+state.asks.size()==totals.size(),"core level count differs");
    for(auto side:{qlib::side::buy,qlib::side::sell}) for(const auto& level:(side==qlib::side::buy?state.bids:state.asks)) {
      const Key key{side,level.price}; Require(totals.contains(key)&&totals.at(key)==level.qty,"core level total differs");
      std::deque<std::int64_t> ids;
      for(auto* node=level.head;node;node=node->next) ids.push_back(node->order_id);
      if(mode==BookMode::Mbo) Require(ids==want.fifo.at(key),"core FIFO differs");
      else Require(ids.empty()&&level.tail==nullptr,"L2 must not create fictitious orders");
    }
    Require(SameBook(actual.OnSnapshot(),ExpectedSnapshot(want,static_cast<std::uint32_t>(i+1),mode)),"core snapshot/time differs");
  }
}
ApplyResult Apply(Orderbook& book,const qlib::feed_event& event) {
  return std::visit(qlib::overloaded{
    [&](const qlib::order& e){return book.OnOrder(e);},[&](const qlib::trade& e){return book.OnTrade(e);},
    [&](const qlib::cancel& e){return book.OnCancel(e);},[&](const qlib::level& e){return book.OnLevel(e);}},event);
}
std::uint32_t Instrument(const qlib::feed_event& event) {return std::visit([](const auto& v){return v.instrument_id;},event);}
CoreTrial PrepareTrial(const CorePayload& p) {
  CoreTrial trial; const auto mode=p.spec.mode=="Mbo"?BookMode::Mbo:BookMode::L2;
  for(std::uint64_t i=0;i<p.spec.instruments;++i) trial.books.push_back(std::make_unique<Orderbook>(static_cast<std::uint32_t>(i+1),mode));
  for(const auto& event:p.initial) Require(Apply(*trial.books.at(Instrument(event)-1),event).status==ApplyStatus::Applied,"core preparation unexpectedly ignored");
  ValidateBooks(trial,p.initial_expected,mode); return trial;
}

CorePayload Generate(const CaseSpec& s,std::uint64_t seed) {
  CorePayload p; p.spec=s;p.expected.resize(s.instruments);
  const bool l2=s.mode=="L2"; const auto& op=s.operation;
  Require(l2==op.starts_with("core.l2.") || op=="core.snapshot","core operation/mode mismatch");
  const bool snapshot=op=="core.snapshot";
  if(!l2 && op!="core.add.growth" && !snapshot) Require(s.initial_orders>0,"core needs existing orders");
  if(s.initial_orders>0 || l2) Require(s.levels>0,"nonempty core needs levels");
  if(op=="core.trade.partial"||op=="core.trade.full") Require(s.initial_orders%2==0,"trade needs matched buy/sell pairs");
  if(op=="core.cancel.full"||op=="core.modify.new_price") Require(s.events_per_trial<=s.initial_orders,"finite operation would reuse an exhausted id");
  if(op=="core.trade.full") Require(s.events_per_trial<=s.initial_orders/2,"trade would reuse deleted pairs");
  if(op=="core.l2.delete") Require(s.events_per_trial<=s.levels,"L2 delete would reuse missing levels");
  if(op=="core.modify.existing_price") Require(s.levels>=2,"existing-price move needs two levels");
  const auto base_qty=static_cast<std::int64_t>(CheckedAdd(100, s.events_per_trial));
  std::int64_t seq=1;
  if(l2) {
    Require(s.initial_orders==0,"L2 initial_orders must be zero");
    for(std::uint64_t instrument=1;instrument<=s.instruments;++instrument) for(std::uint64_t i=0;i<s.levels;++i) {
      const auto side=i%2?qlib::side::sell:qlib::side::buy;const auto price=static_cast<std::int64_t>(100+i);
      qlib::level value{seq,price,100,seq+1000,static_cast<std::uint32_t>(instrument),side,seq+2000};++seq;
      p.initial.emplace_back(value);auto& expected=p.expected[instrument-1];expected.l2[{side,price}]=100;Touch(expected,p.initial.back());
    }
  } else {
    for(std::uint64_t i=0;i<s.initial_orders;++i) {
      const auto instrument=static_cast<std::uint32_t>((i/2)%s.instruments+1);
      const auto side=i%2?qlib::side::sell:qlib::side::buy;
      const auto price=static_cast<std::int64_t>(100+(i/2)%std::max<std::uint64_t>(s.levels,1));
      auto value=NewOrder(static_cast<std::int64_t>(i+1),side,price,base_qty,instrument,seq++);
      p.initial.emplace_back(value);ExpectedAdd(p.expected[instrument-1],value);Touch(p.expected[instrument-1],p.initial.back());
    }
    // 锚定单不参加被测事件，保证两个改价目标整个批次都存在。
    if(op=="core.modify.existing_price") for(std::uint64_t instrument=1;instrument<=s.instruments;++instrument)
      for(auto side:{qlib::side::buy,qlib::side::sell}) for(std::int64_t price:{100,101}) {
        auto value=NewOrder(static_cast<std::int64_t>(s.initial_orders+1+p.initial.size()),side,price,100,static_cast<std::uint32_t>(instrument),seq++);
        p.initial.emplace_back(value);ExpectedAdd(p.expected[instrument-1],value);Touch(p.expected[instrument-1],p.initial.back());
      }
  }
  p.initial_expected=p.expected;
  if(snapshot) return p;
  p.events.reserve(s.events_per_trial);
  // 固定模加访问顺序；seed 改变起点，不依赖 std::shuffle 的库实现。
  const auto offset=s.initial_orders?seed%s.initial_orders:0;
  for(std::uint64_t i=0;i<s.events_per_trial;++i) {
    IgnoreReason reason=IgnoreReason::None;
    if(op=="core.add.growth") {
      const auto instrument=static_cast<std::uint32_t>(i%s.instruments+1);
      auto value=NewOrder(static_cast<std::int64_t>(s.initial_orders+i+1),i%2?qlib::side::sell:qlib::side::buy,
          static_cast<std::int64_t>(100+i%std::max<std::uint64_t>(s.levels,1)),100,instrument,seq++);
      p.events.emplace_back(value);ExpectedAdd(p.expected[instrument-1],value);
    } else if(l2) {
      const auto index=i%s.levels;const auto instrument=static_cast<std::uint32_t>((i/s.levels)%s.instruments+1);
      const auto side=index%2?qlib::side::sell:qlib::side::buy;
      const auto price=static_cast<std::int64_t>(op=="core.l2.insert"?100+s.levels+i:100+index);
      auto& expected=p.expected[instrument-1];
      if(op=="core.l2.trade") {p.events.emplace_back(qlib::trade{seq,0,0,price,1,seq+1000,instrument,side,seq+2000});reason=IgnoreReason::L2Trade;}
      else {
        const auto qty=op=="core.l2.delete"?0:(op=="core.l2.insert"?100:(expected.l2.at({side,price})==100?101:100));
        p.events.emplace_back(qlib::level{seq,price,qty,seq+1000,instrument,side,seq+2000});
        if(qty) expected.l2[{side,price}]=qty;else expected.l2.erase({side,price});
      }++seq;
    } else if(op=="core.trade.partial"||op=="core.trade.full") {
      const auto pair=(i+seed%(s.initial_orders/2))%(s.initial_orders/2);
      const auto buy=static_cast<std::int64_t>(pair*2+1),sell=buy+1;
      const auto instrument=static_cast<std::uint32_t>(pair%s.instruments+1);
      auto& expected=p.expected[instrument-1];
      const auto qty=op=="core.trade.full"?base_qty:1;
      p.events.emplace_back(qlib::trade{seq,buy,sell,100,qty,seq+1000,instrument,qlib::side::sell,seq+2000});++seq;
      if(op=="core.trade.full") {ExpectedErase(expected,buy);ExpectedErase(expected,sell);}
      else {--expected.orders.at(buy).qty;--expected.orders.at(sell).qty;}
    } else {
      const auto id=static_cast<std::int64_t>((i+offset)%s.initial_orders+1);
      const auto instrument=static_cast<std::uint32_t>(((id-1)/2)%s.instruments+1);
      auto& expected=p.expected[instrument-1];
      if(op=="core.ignored.unknown") {
        p.events.emplace_back(qlib::cancel{seq,static_cast<std::int64_t>(s.initial_orders+100000+i),1,instrument,qlib::side::buy,seq+1000,seq+2000});reason=IgnoreReason::UnknownOrder;++seq;
      } else {
        const auto previous=expected.orders.at(id);
        if(op=="core.cancel.partial"||op=="core.cancel.full") {
          const auto qty=op=="core.cancel.full"?previous.qty:1;
          p.events.emplace_back(qlib::cancel{seq,id,qty,instrument,previous.side,seq+1000,seq+2000});++seq;
          if(op=="core.cancel.full") ExpectedErase(expected,id);else --expected.orders.at(id).qty;
        } else {
          auto event=NewOrder(id,previous.side,previous.price,777,instrument,seq++);
          if(op=="core.ignored.duplicate") {event.qty=previous.qty;reason=IgnoreReason::DuplicateId;}
          else {
            event.action=qlib::order_action::modify;event.new_qty=previous.qty==base_qty?base_qty+1:base_qty;
            if(op=="core.ignored.side_mismatch") {event.side=previous.side==qlib::side::buy?qlib::side::sell:qlib::side::buy;reason=IgnoreReason::SideMismatch;}
            else if(op=="core.modify.same_price") expected.orders.at(id).qty=event.new_qty;
            else if(op=="core.modify.existing_price"||op=="core.modify.new_price") {
              // 新价始终严格高于初态最大价格，并且每个事件唯一。
              event.price=op=="core.modify.new_price"?static_cast<std::int64_t>(100+s.levels+i):(previous.price==100?101:100);
              ExpectedMove(expected,id,event.price,event.new_qty);
            } else throw std::logic_error("unsupported core operation");
          }
          p.events.emplace_back(event);
        }
      }
    }
    auto& expected=p.expected.at(Instrument(p.events.back())-1);Touch(expected,p.events.back());
    ++p.expected_reasons.at(static_cast<std::size_t>(reason));if(reason==IgnoreReason::None)++p.expected_applied;
  }
  return p;
}
}  // namespace

const std::vector<std::string>& CoreOperations() {
  static const std::vector<std::string> operations{"core.add.growth","core.modify.same_price","core.modify.existing_price","core.modify.new_price",
    "core.cancel.partial","core.cancel.full","core.trade.partial","core.trade.full","core.l2.insert","core.l2.replace","core.l2.delete","core.l2.trade",
    "core.ignored.duplicate","core.ignored.unknown","core.ignored.side_mismatch","core.snapshot"};
  return operations;
}

PreparedBenchmark PrepareCoreBenchmark(const CaseSpec& s,const ExperimentSpec& experiment) {
  auto payload=std::make_shared<const CorePayload>(Generate(s,experiment.seed));
  const bool snapshot=s.operation=="core.snapshot";
  const auto hash=HashWorkload(s,experiment.seed,payload->initial,payload->events);
  PreparedBenchmark prepared{s,hash,{{"prepare","none","construct fresh books, apply initial state and independently validate"},
      {snapshot?"snapshot":"apply",snapshot?"snapshots":"operations","prepared batch loop only; classification and checksum included"},
      {"validate","none","independent complete orders, FIFO, levels, times and snapshots"},
      {"destroy","none","release this trial's orderbooks"}},"batch-boundaries-and-local-classification",{}};
  if(snapshot&&s.instruments>1) prepared.phases.push_back({"snapshot-round","snapshot_rounds","same snapshot batch duration; one round calls all instruments"});
  prepared.run=[payload,hash,snapshot,seed=experiment.seed](const TrialContext&) {
    const auto& s=payload->spec;const auto mode=s.mode=="Mbo"?BookMode::Mbo:BookMode::L2;
    const auto begin=Clock::now();auto trial=PrepareTrial(*payload);const auto ready=Clock::now();
    std::array<std::uint64_t,10> reasons{};std::uint64_t applied=0;
    std::vector<qlib::book> last_snapshots(s.instruments);
    auto measured=MeasureBatch(snapshot?"snapshot":"apply",snapshot?"snapshots":"operations",[&]{
      BatchObservation result{};
      if(snapshot) {
        for(std::uint64_t round=0;round<s.events_per_trial;++round) for(std::size_t j=0;j<trial.books.size();++j) {
          auto value=trial.books[j]->OnSnapshot();last_snapshots[j]=value;
          result.checksum^=static_cast<std::uint64_t>(value.bid_qty[0])+static_cast<std::uint64_t>(value.ask_qty[0])+result.work_count;
          ++result.work_count;
        }
      } else {
        // 同类内核在循环外选择；批内直接调用对应生产接口。
        auto typed=[&]<class T>() {
          for(const auto& event:payload->events) {
            const auto& value=std::get<T>(event);
            auto& book=*trial.books.at(value.instrument_id-1);
            ApplyResult result_value;
            if constexpr(std::is_same_v<T,qlib::order>) result_value=book.OnOrder(value);
            else if constexpr(std::is_same_v<T,qlib::trade>) result_value=book.OnTrade(value);
            else if constexpr(std::is_same_v<T,qlib::cancel>) result_value=book.OnCancel(value);
            else result_value=book.OnLevel(value);
            ++reasons.at(static_cast<std::size_t>(result_value.reason));
            applied+=result_value.status==ApplyStatus::Applied;
            result.checksum=(result.checksum*131)^static_cast<std::uint8_t>(result_value.reason)^static_cast<std::uint8_t>(result_value.status);
            ++result.work_count;
          }
        };
        if(s.operation.starts_with("core.trade.")||s.operation=="core.l2.trade") typed.template operator()<qlib::trade>();
        else if(s.operation.starts_with("core.cancel.")||s.operation=="core.ignored.unknown") typed.template operator()<qlib::cancel>();
        else if(s.operation.starts_with("core.l2.")) typed.template operator()<qlib::level>();
        else typed.template operator()<qlib::order>();
      }
      return result;
    });
    TrialResult result;
    const auto validation_begin=Clock::now();
    try {
      ValidateBooks(trial,payload->expected,mode);
      if(!snapshot) {Require(reasons==payload->expected_reasons,"core ignored reasons differ");Require(applied==payload->expected_applied,"core applied count differs");Require(measured.work_count==s.events_per_trial,"core measured work differs");}
      else for(std::size_t i=0;i<last_snapshots.size();++i) Require(SameBook(last_snapshots[i],ExpectedSnapshot(payload->expected[i],static_cast<std::uint32_t>(i+1),mode)),"measured snapshot differs");
      Require(HashWorkload(s,seed,payload->initial,payload->events)==hash,"core input changed");
      for(const auto& event:payload->events) if(const auto* order=std::get_if<qlib::order>(&event))
        Require(order->prev==nullptr&&order->next==nullptr,"input order links changed");
    } catch(const std::exception& error) {
      result.validation_status="invalid";result.validation_message=error.what();
    } catch(...) {
      result.validation_status="invalid";result.validation_message="unknown core validation failure";
    }
    const auto validated=Clock::now();trial.books.clear();const auto destroyed=Clock::now();
    result.measurements={{"prepare","none",0,CheckedDurationNs(begin,ready),0},measured,
      {"validate","none",0,CheckedDurationNs(validation_begin,validated),0},{"destroy","none",0,CheckedDurationNs(validated,destroyed),0}};
    if(snapshot&&s.instruments>1) result.measurements.push_back({"snapshot-round","snapshot_rounds",s.events_per_trial,measured.duration_ns,measured.checksum});
    const bool valid=result.validation_status=="valid";
    result.details_json="{\"applied\":"+std::to_string(applied)+",\"ignored\":"+std::to_string(snapshot?0:measured.work_count-applied)+",\"validated_books\":"+std::to_string(valid?s.instruments:0)+",\"validation_completed\":"+(valid?"true":"false")+",\"input_unchanged\":"+(valid?"true":"null")+",\"workload_hash\":"+JsonString(hash)+"}";
    return result;
  };
  return prepared;
}
}  // namespace qbook::bench
