#include "ProtocolTrial.h"
#include "Measure.h"
#include "Workloads.h"
#include "Protocol.h"
#include "support/FeedEncoder.h"

#include <array>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>

#ifndef QBOOK_SOURCE_DIR
#define QBOOK_SOURCE_DIR "."
#endif

namespace qbook::bench {
namespace {
constexpr std::int64_t kRecv = 1234567890123;
struct Frame {
  // 故意从 data()+1 解码，所有正式帧同样覆盖非对齐读取。
  std::vector<std::byte> storage;
  std::optional<qlib::feed_event> expected;
  DecodeError error = DecodeError::None;
  std::span<const std::byte> bytes() const { return {storage.data()+1,storage.size()-1}; }
};
struct ProtocolPayload {
  CaseSpec spec; std::vector<Frame> frames; std::vector<std::size_t> indices;
  std::array<std::uint64_t,6> expected_errors{};
  std::uint64_t expected_checksum=0,bytes=0;
};
void Require(bool condition,std::string_view why) {if(!condition)throw std::runtime_error(std::string(why));}
template<class T> bool Same(const T& a,const T& b) {
  if constexpr(std::is_same_v<T,qlib::order>) return std::tie(a.seq,a.order_id,a.price,a.qty,a.new_qty,a.instrument_id,a.side,a.type,a.action,a.event_ns,a.recv_ns,a.prev,a.next)==std::tie(b.seq,b.order_id,b.price,b.qty,b.new_qty,b.instrument_id,b.side,b.type,b.action,b.event_ns,b.recv_ns,b.prev,b.next);
  else if constexpr(std::is_same_v<T,qlib::trade>) return std::tie(a.seq,a.buy_order_id,a.sell_order_id,a.price,a.qty,a.event_ns,a.instrument_id,a.side,a.recv_ns)==std::tie(b.seq,b.buy_order_id,b.sell_order_id,b.price,b.qty,b.event_ns,b.instrument_id,b.side,b.recv_ns);
  else if constexpr(std::is_same_v<T,qlib::cancel>) return std::tie(a.seq,a.order_id,a.qty,a.instrument_id,a.side,a.event_ns,a.recv_ns)==std::tie(b.seq,b.order_id,b.qty,b.instrument_id,b.side,b.event_ns,b.recv_ns);
  else return std::tie(a.seq,a.price,a.qty,a.event_ns,a.instrument_id,a.side,a.recv_ns)==std::tie(b.seq,b.price,b.qty,b.event_ns,b.instrument_id,b.side,b.recv_ns);
}
bool SameEvent(const qlib::feed_event& a,const qlib::feed_event& b) {
  if(a.index()!=b.index())return false;
  return std::visit([&](const auto& value){return Same(value,std::get<std::remove_cvref_t<decltype(value)>>(b));},a);
}
std::uint64_t ScalarChecksum(const std::optional<qlib::feed_event>& event,DecodeError error) {
  if(!event)return static_cast<std::uint8_t>(error);
  return std::visit([](const auto& value){return static_cast<std::uint64_t>(value.seq)^static_cast<std::uint64_t>(value.qty)^value.instrument_id;},*event);
}
void ValidateFrame(const Frame& frame,BookMode mode) {
  const auto result=DecodeFrame(frame.bytes(),kRecv,mode);
  Require(result.error==frame.error,"protocol rejection reason differs");
  Require(result.event.has_value()==frame.expected.has_value(),"protocol optional event differs");
  if(result.event)Require(SameEvent(*result.event,*frame.expected),"protocol scalar fields differ");
}
Frame Encode(qlib::feed_event value) {
  const auto encoded=qbook::testing::EncodeFeedEvent(value);
  Frame result;result.storage.push_back(std::byte{0xdd});
  result.storage.insert(result.storage.end(),encoded.bytes().begin(),encoded.bytes().end());
  std::visit([](auto& v){v.recv_ns=kRecv;},value);result.expected=std::move(value);
  if(std::holds_alternative<qlib::order>(*result.expected)) {
    // 指针及 padding 污染不能进入解码后的本地链接。
    for(std::size_t offset=40;offset<56;++offset)result.storage[2+offset]=std::byte{0xa5};
    result.storage[2+63]=std::byte{0xe7};
  }
  return result;
}
qlib::feed_event GeneratedEvent(std::string_view kind,std::uint64_t index) {
  const auto seq=static_cast<std::int64_t>(index+1);const auto instrument=static_cast<std::uint32_t>(index%31+1);
  const auto side=index%2?qlib::side::sell:qlib::side::buy;
  if(kind=="valid_trade") return qlib::trade{seq,seq*2,seq*2+1,100+seq,1+seq%19,1000+seq,instrument,side,-777};
  if(kind=="valid_cancel") return qlib::cancel{seq,seq,1+seq%19,instrument,side,1000+seq,-777};
  if(kind=="valid_level") return qlib::level{seq,100+seq,seq%2?100:-2,1000+seq,instrument,side,-777};
  qlib::order value{};value.seq=seq;value.order_id=seq;value.price=100+seq;value.qty=100;value.new_qty=-7;
  value.instrument_id=instrument;value.side=side;value.type=qlib::order_type::limit;
  value.action=index%2?qlib::order_action::modify:qlib::order_action::add;value.event_ns=1000+seq;value.recv_ns=-777;
  return value;
}
Frame MakeFrame(std::string_view kind,std::uint64_t index,BookMode mode) {
  auto base=GeneratedEvent(kind,index);
  if(kind=="mode_mismatch") base=GeneratedEvent(mode==BookMode::Mbo?"valid_level":"valid_order",index);
  // 字段/长度拒绝使用本模式允许的记录，避免先触发模式筛选。
  if((kind=="invalid_field"||kind=="wrong_size"||kind=="unknown_tag"||kind=="empty")&&mode==BookMode::L2) base=GeneratedEvent("valid_level",index);
  if(kind=="invalid_field")std::visit([](auto& value){value.side=static_cast<qlib::side>(7);},base);
  Frame result=Encode(base);
  if(kind=="empty"){result.storage.resize(1);result.error=DecodeError::EmptyFrame;}
  else if(kind=="unknown_tag"){result.storage[1]=std::byte{0xff};result.error=DecodeError::UnknownTag;}
  else if(kind=="wrong_size"){result.storage.pop_back();result.error=DecodeError::WrongSize;}
  else if(kind=="invalid_field")result.error=DecodeError::InvalidField;
  else if(kind=="mode_mismatch")result.error=DecodeError::ModeMismatch;
  if(result.error!=DecodeError::None)result.expected.reset();
  return result;
}
void ValidateGolden() {
  // 独立十进制答案来自固定 fixture 说明，既不调用编码器也不从解码结果生成。
  qlib::order order{};order.seq=72623859790382856;order.order_id=1230066625199609624;
  order.price=10000000000;order.qty=100000000;order.new_qty=-9;order.instrument_id=287454020;
  order.side=qlib::side::buy;order.type=qlib::order_type::limit;order.action=qlib::order_action::add;order.event_ns=1234567890;order.recv_ns=kRecv;
  auto deletion=order;deletion.seq=9;deletion.price=-77;deletion.qty=-44;deletion.new_qty=-7;deletion.side=qlib::side::sell;deletion.type=qlib::order_type::market;deletion.action=qlib::order_action::modify;deletion.event_ns=0;
  const std::vector<std::pair<std::string,qlib::feed_event>> golden{
    {"order_add.bin",order},{"order_modify_delete.bin",deletion},
    {"trade.bin",qlib::trade{10,1230066625199609624,2387509390608836392,20000000000,200000000,1234567891,1432778632,qlib::side::sell,kRecv}},
    {"cancel.bin",qlib::cancel{11,1230066625199609624,300000000,287454020,qlib::side::buy,1234567892,kRecv}},
    {"level.bin",qlib::level{12,30000000000,-2,1234567893,2864434397U,qlib::side::sell,kRecv}}};
  for(const auto& [name,value]:golden) {
    std::ifstream input(std::filesystem::path(QBOOK_SOURCE_DIR)/"tests/fixtures/protocol"/name,std::ios::binary);
    Require(input.is_open(),"missing independent protocol fixture");
    const std::vector<char> raw((std::istreambuf_iterator<char>(input)),{});
    Require(!input.bad(),"cannot read protocol fixture");
    Frame frame;frame.storage.push_back(std::byte{0});for(char c:raw)frame.storage.push_back(static_cast<std::byte>(c));frame.expected=value;
    ValidateFrame(frame,std::holds_alternative<qlib::level>(value)?BookMode::L2:BookMode::Mbo);
  }
}
std::string HashPayload(const ProtocolPayload& p,std::uint64_t seed) {
  WorkloadHasher hash;hash.String(kGeneratorVersion);hash.String(CanonicalCaseJson(p.spec));hash.U64(seed);
  hash.U64(p.frames.size());for(const auto& frame:p.frames){hash.U64(frame.bytes().size());hash.Bytes(frame.bytes());hash.U64(static_cast<std::uint8_t>(frame.error));if(frame.expected)hash.Event(*frame.expected);}
  hash.U64(p.indices.size());for(auto index:p.indices)hash.U64(index);return hash.Finish();
}
}  // namespace

const std::vector<std::string>& ProtocolOperations() {
  static const std::vector<std::string> values{"protocol.valid.order","protocol.valid.trade","protocol.valid.cancel","protocol.valid.level",
    "protocol.reject.empty","protocol.reject.unknown_tag","protocol.reject.wrong_size","protocol.reject.mode_mismatch","protocol.reject.invalid_field","protocol.mixed"};return values;
}
PreparedBenchmark PrepareProtocolBenchmark(const CaseSpec& s,const ExperimentSpec& experiment) {
  ValidateGolden();const auto mode=s.mode=="Mbo"?BookMode::Mbo:BookMode::L2;
  auto p=std::make_shared<ProtocolPayload>();p->spec=s;
  std::vector<std::string> kinds;
  if(s.operation=="protocol.mixed") {
    for(const auto& [kind,count]:s.frame_mix)for(std::uint64_t n=0;n<count;++n)kinds.push_back(kind);
  } else {
    auto kind=s.operation.substr(s.operation.rfind('.')+1);
    if(s.operation.starts_with("protocol.valid."))kind="valid_"+kind;
    kinds.assign(s.working_set_frames,kind);
  }
  for(std::size_t i=0;i<kinds.size();++i) {
    auto frame=MakeFrame(kinds[i],i,mode);ValidateFrame(frame,mode);p->bytes+=frame.bytes().size();p->frames.push_back(std::move(frame));
  }
  Require(p->frames.size()==s.working_set_frames,"protocol frame workset differs");
  p->indices.reserve(s.events_per_trial);
  for(std::uint64_t i=0;i<s.events_per_trial;++i) {
    const auto index=static_cast<std::size_t>((i+experiment.seed%s.working_set_frames)%s.working_set_frames);
    p->indices.push_back(index);const auto& frame=p->frames[index];
    ++p->expected_errors[static_cast<std::size_t>(frame.error)];
    p->expected_checksum=(p->expected_checksum*131)^ScalarChecksum(frame.expected,frame.error);
  }
  const auto hash=HashPayload(*p,experiment.seed);
  PreparedBenchmark result{s,hash,{{"decode","frames","batch DecodeFrame calls; classification and scalar checksum included"},
      {"validate","none","all owned frames independently checked field by field and input content rechecked"}},"batch-boundaries-local-classification-nonaligned-input",{}};
  result.run=[p,hash,seed=experiment.seed,mode](const TrialContext&){
    std::array<std::uint64_t,6> errors{};
    auto measured=MeasureBatch("decode","frames",[&]{
      BatchObservation observation{};
      for(auto index:p->indices) {
        const auto decoded=DecodeFrame(p->frames[index].bytes(),kRecv,mode);
        ++errors.at(static_cast<std::size_t>(decoded.error));
        observation.checksum=(observation.checksum*131)^ScalarChecksum(decoded.event,decoded.error);++observation.work_count;
      }return observation;
    });
    TrialResult trial;
    const auto before=Clock::now();
    try {
      Require(errors==p->expected_errors,"protocol error histogram differs");
      Require(measured.work_count==p->spec.events_per_trial&&measured.checksum==p->expected_checksum,"protocol measured work/checksum differs");
      for(const auto& frame:p->frames)ValidateFrame(frame,mode);
      Require(HashPayload(*p,seed)==hash,"protocol input changed");
    } catch(const std::exception& error) {
      trial.validation_status="invalid";trial.validation_message=error.what();
    } catch(...) {
      trial.validation_status="invalid";trial.validation_message="unknown protocol validation failure";
    }
    const auto after=Clock::now();
    trial.measurements={measured,{"validate","none",0,CheckedDurationNs(before,after),0}};
    const bool valid=trial.validation_status=="valid";
    trial.details_json="{\"working_set_bytes\":"+std::to_string(p->bytes)+",\"validated_owned_frames\":"+std::to_string(valid?p->frames.size():0)+",\"decode_attempts\":"+std::to_string(measured.work_count)+",\"independent_golden_frames\":5,\"validation_completed\":"+(valid?"true":"false")+",\"input_unchanged\":"+(valid?"true":"null")+",\"errors\":[";
    for(std::size_t i=0;i<errors.size();++i){if(i)trial.details_json+=',';trial.details_json+=std::to_string(errors[i]);}
    trial.details_json+="]}";return trial;
  };
  return result;
}
}  // namespace qbook::bench
