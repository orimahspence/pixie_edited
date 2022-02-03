/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string>
#include <utility>
#include <vector>

#include "src/common/system/proc_parser.h"
#include "src/stirling/source_connectors/perf_profiler/java/agent/raw_symbol_update.h"
#include "src/stirling/source_connectors/perf_profiler/java/attach.h"
#include "src/stirling/source_connectors/perf_profiler/java/demangle.h"
#include "src/stirling/source_connectors/perf_profiler/symbolizers/java_symbolizer.h"
#include "src/stirling/utils/detect_application.h"

namespace px {
namespace stirling {

void JavaSymbolizationContext::UpdateSymbolMap() {
  auto reset_symbol_file = [&](const auto pos) {
    symbol_file_->seekg(pos);
    symbol_file_->clear();
    DCHECK(symbol_file_->good());
  };

  java::RawSymbolUpdate new_symbol;
  char* new_symbol_ptr = reinterpret_cast<char*>(&new_symbol);

  std::string buffer;
  std::string symbol;
  std::string fn_sig;
  std::string class_sig;

  buffer.reserve(300);
  symbol.reserve(100);
  fn_sig.reserve(100);
  class_sig.reserve(100);

  while (true) {
    const auto pos = symbol_file_->tellg();

    symbol_file_->read(new_symbol_ptr, sizeof(java::RawSymbolUpdate));
    if (!symbol_file_->good()) {
      // No data to be read. Break from the loop. Pedantically reset file pos so that when we
      // return here, we are in the correct state.
      reset_symbol_file(pos);
      break;
    }

    const uint64_t n = new_symbol.TotalNumSymbolBytes();

    if (buffer.capacity() < n) {
      buffer.resize(n);
    }

    symbol_file_->read(buffer.data(), n);
    if (!symbol_file_->good()) {
      // If the read fails, then the symbol file was left in a partially written state.
      // Reset the file position back to the beginning of a symbol, and break out of this loop.
      reset_symbol_file(pos);
      break;
    }

    // TODO(jps): Make the interface to the demangler consume string_view only, then
    // convert symbol, fn_sig, and class_sig to string_view (reduces copying).
    // TODO(jps): Remove null terminating character from java::RawSymbolUpdate.
    symbol.assign(buffer.data() + new_symbol.SymbolOffset(), new_symbol.symbol_size - 1);
    fn_sig.assign(buffer.data() + new_symbol.FnSigOffset(), new_symbol.fn_sig_size - 1);
    class_sig.assign(buffer.data() + new_symbol.ClassSigOffset(), new_symbol.class_sig_size - 1);

    // TODO(jps): Move the jsym_pfx to a common header. Consider adding it in the demangler.
    constexpr std::string_view jsym_pfx = "[j] ";
    const std::string demangled = absl::StrCat(jsym_pfx, java::Demangle(symbol, class_sig, fn_sig));

    // TODO(jps): Change to uint32_t in java::RawSymbolUpdate.
    const uint32_t code_size = static_cast<uint32_t>(new_symbol.code_size);
    symbol_map_.try_emplace(new_symbol.addr, demangled, code_size);
  }
  DCHECK(symbol_file_->good());
}

JavaSymbolizationContext::JavaSymbolizationContext(profiler::SymbolizerFn native_symbolizer_fn,
                                                   std::unique_ptr<std::ifstream> symbol_file)
    : native_symbolizer_fn_(native_symbolizer_fn), symbol_file_(std::move(symbol_file)) {
  DCHECK(symbol_file_->good());
  UpdateSymbolMap();
}

JavaSymbolizationContext::~JavaSymbolizationContext() { symbol_file_->close(); }

std::string_view JavaSymbolizationContext::Symbolize(const uintptr_t addr) {
  if (requires_refresh_) {
    // Member requires_refresh_ is set by IterationPreTick(), which is called "once per iteration,"
    // i.e. is set to true each time we drain the stack trace data from the underlying BPF
    // data tables. We will only attempt to update the symbol map (and possibly incur expensive
    // syscalls for file IO) if this member is set and we are symbolizing in this context.
    // Subsequent calls to Symbolize() in this iteration will not attempt to update the symbol map.
    UpdateSymbolMap();
    requires_refresh_ = false;
  }

  static std::string symbol;

  if (symbol_map_.size() > 0) {
    auto it = symbol_map_.upper_bound(addr);
    if (it != symbol_map_.begin()) {
      it--;
      const uint64_t addr_lower = it->first;
      const uint64_t addr_upper = addr_lower + it->second.size;
      if ((addr_lower <= addr) && (addr < addr_upper)) {
        symbol = it->second.symbol;
        return symbol;
      }
    }
  }
  return native_symbolizer_fn_(addr);
}

StatusOr<std::unique_ptr<Symbolizer>> JavaSymbolizer::Create(
    std::unique_ptr<Symbolizer> native_symbolizer) {
  auto java_symbolizer = std::unique_ptr<JavaSymbolizer>(new JavaSymbolizer);
  java_symbolizer->native_symbolizer_ = std::move(native_symbolizer);
  return std::unique_ptr<Symbolizer>(java_symbolizer.release());
}

void JavaSymbolizer::IterationPreTick() {
  native_symbolizer_->IterationPreTick();
  for (auto& [upid, ctx] : symbolization_contexts_) {
    ctx->set_requires_refresh();
  }
}

void JavaSymbolizer::DeleteUPID(const struct upid_t& upid) {
  // The inner map is owned by a unique_ptr; this will free the memory.
  symbolizer_functions_.erase(upid);
  symbolization_contexts_.erase(upid);
  native_symbolizer_->DeleteUPID(upid);
}

std::string_view JavaSymbolizer::Symbolize(JavaSymbolizationContext* ctx, const uintptr_t addr) {
  return ctx->Symbolize(addr);
}

profiler::SymbolizerFn JavaSymbolizer::GetSymbolizerFn(const struct upid_t& upid) {
  auto fn_it = symbolizer_functions_.find(upid);
  if (fn_it != symbolizer_functions_.end()) {
    return fn_it->second;
  }

  // The underlying symbolization function is the fallback if we fail out at some point below,
  // and is also the fallback if eventually we do not find a Java symbol.
  auto native_symbolizer_fn = native_symbolizer_->GetSymbolizerFn(upid);

  using fs_path = std::filesystem::path;
  const auto& proc_parser = system::ProcParser(system::Config::GetInstance());
  auto status_or_exe_path = proc_parser.GetExePath(upid.pid);

  if (!status_or_exe_path.ok()) {
    // Unable to get the read /prod/<pid> for target process.
    // Fall back to native symbolizer.
    symbolizer_functions_[upid] = native_symbolizer_fn;
    return native_symbolizer_fn;
  }
  const fs_path proc_exe = status_or_exe_path.ConsumeValueOrDie();

  if (DetectApplication(proc_exe) != Application::kJava) {
    // This process is not Java. Fall back to native symbolizer.
    symbolizer_functions_[upid] = native_symbolizer_fn;
    return native_symbolizer_fn;
  }

  const std::vector<std::string> libs = {
      "/pl/lib-px-java-agent-musl.so",
      "/pl/lib-px-java-agent-glibc.so",
  };
  const std::string kSymFilePathPfx = "/tmp/px-java-symbolization-agent";

  auto attacher = java::AgentAttacher(upid.pid, kSymFilePathPfx, libs);

  constexpr auto kTimeOutForAttach = std::chrono::milliseconds{250};
  constexpr auto kAttachRecheckPeriod = std::chrono::milliseconds{10};
  auto time_elapsed = std::chrono::milliseconds{0};

  while (!attacher.Finished()) {
    if (time_elapsed >= kTimeOutForAttach) {
      // Attacher did not complete. Fall back to native symbolizer.
      symbolizer_functions_[upid] = native_symbolizer_fn;
      return native_symbolizer_fn;
    }
    // Still waiting to finish the attach process.
    // TODO(jps): Create a temporary symbolization function,
    // and return that here to unblock Stirling.
    std::this_thread::sleep_for(kAttachRecheckPeriod);
    time_elapsed += kAttachRecheckPeriod;
  }

  if (!attacher.attached()) {
    // This process *is* Java, but we failed to attach the symbolization agent. Fall back to
    // symbolizer function from the underlying native symbolizer.
    // To prevent this from happening again, store that in the map.
    symbolizer_functions_[upid] = native_symbolizer_fn;
    return native_symbolizer_fn;
  }

  char const* const symbol_file_path_template = "/proc/$0/root/tmp/px-java-symbolization-agent.bin";
  const std::string symbol_file_path = absl::Substitute(symbol_file_path_template, upid.pid);
  auto symbol_file = std::unique_ptr<std::ifstream>(
      new std::ifstream(symbol_file_path, std::ios::in | std::ios::binary));

  if (!symbol_file) {
    // Could not open the symbol file from the symbolization agent: fall back to native symbolizer.
    symbolizer_functions_[upid] = native_symbolizer_fn;
    return native_symbolizer_fn;
  }

  DCHECK(symbolization_contexts_.find(upid) == symbolization_contexts_.end());

  const auto [iter, inserted] = symbolization_contexts_.try_emplace(upid, nullptr);
  DCHECK(inserted);
  if (inserted) {
    iter->second =
        std::make_unique<JavaSymbolizationContext>(native_symbolizer_fn, std::move(symbol_file));
  }
  auto& ctx = iter->second;

  using std::placeholders::_1;
  auto fn = std::bind(&JavaSymbolizer::Symbolize, this, ctx.get(), _1);

  symbolizer_functions_[upid] = fn;
  return fn;
}

}  // namespace stirling
}  // namespace px
