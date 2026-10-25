#include "FailingOutputStream.h"

#include "Runtime.h"

#include <arrow/io/interfaces.h>
#include <arrow/result.h>

#include <algorithm>
#include <stdexcept>

namespace qbook::testing {
namespace {
void Trace(StreamProbe& probe, const std::string& file, StreamOperation operation) {
  if (probe.calls.size() == probe.calls.capacity()) { probe.overflow = true; return; }
  probe.calls.push_back({file, operation, std::this_thread::get_id()});
}

class FailingOutputStream final : public arrow::io::OutputStream {
public:
  FailingOutputStream(std::shared_ptr<arrow::io::OutputStream> output,
                      std::shared_ptr<StreamProbe> probe, StreamFaultPlan plan, std::string file)
      : output_(std::move(output)), probe_(std::move(probe)), plan_(std::move(plan)),
        file_(std::move(file)) {}
  ~FailingOutputStream() override {
    try { Trace(*probe_, file_, StreamOperation::Destroy); } catch (...) {}
  }
  arrow::Status Close() override {
    Trace(*probe_, file_, StreamOperation::Close);
    auto status = output_->Close();
    if (!status.ok()) return status;
    if (Selected() && plan_.fail_close) {
      ++probe_->failures;
      return arrow::Status::IOError("injected close: ", file_);
    }
    return arrow::Status::OK();
  }
  arrow::Status Abort() override {
    Trace(*probe_, file_, StreamOperation::Abort);
    aborted_ = true;
    auto status = output_->Abort();
    if (!status.ok()) return status;
    if (Selected() && plan_.fail_abort) {
      ++probe_->failures;
      return arrow::Status::IOError("injected abort: ", file_);
    }
    return arrow::Status::OK();
  }
  bool closed() const override { return output_->closed(); }
  arrow::Result<std::int64_t> Tell() const override { return output_->Tell(); }
  arrow::Status Write(const void* data, std::int64_t nbytes) override {
    Trace(*probe_, file_, StreamOperation::Write);
    if (aborted_) {
      ++probe_->writes_after_abort;
      return arrow::Status::IOError("stream already aborted: ", file_);
    }
    if (Selected() && probe_->armed.exchange(false, std::memory_order_acq_rel)) {
      probe_->entered.store(true, std::memory_order_release);
      while (plan_.pause_write && !probe_->release.load(std::memory_order_acquire)) {
        if (probe_->cancel.load(std::memory_order_acquire) ||
            (plan_.control && plan_.control->Failed()) ||
            std::chrono::steady_clock::now() >= plan_.deadline)
          return arrow::Status::IOError("injected gate cancelled: ", file_);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
      if (plan_.fail_write) {
        ++probe_->failures;
        if (plan_.partial_write_bytes) {
          auto status = output_->Write(data, std::min(nbytes, static_cast<std::int64_t>(plan_.partial_write_bytes)));
          if (!status.ok()) return status;
        }
        return arrow::Status::IOError("injected write: ", file_);
      }
    }
    return output_->Write(data, nbytes);
  }
private:
  bool Selected() const { return file_ == plan_.file; }
  std::shared_ptr<arrow::io::OutputStream> output_;
  std::shared_ptr<StreamProbe> probe_;
  StreamFaultPlan plan_;
  std::string file_;
  bool aborted_ = false;
};
}

detail::OutputStreamFactory MakeOutputFactory(std::shared_ptr<StreamProbe> probe, StreamFaultPlan plan) {
  return [probe = std::move(probe), plan = std::move(plan)](const std::filesystem::path& path)
      -> std::shared_ptr<arrow::io::OutputStream> {
    const auto file = path.filename().string();
    Trace(*probe, file, StreamOperation::Open);
    if (file == plan.file && plan.fail_open) {
      ++probe->failures;
      throw std::runtime_error("injected open: " + file);
    }
    return std::make_shared<FailingOutputStream>(detail::OpenExclusiveOutput(path), probe, plan, file);
  };
}
}  // namespace qbook::testing
