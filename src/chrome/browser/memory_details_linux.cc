// Copyright 2026 The Chromium Authors and Alex313031
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/memory_details.h"

#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <set>
#include <vector>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/process/process_iterator.h"
#include "base/process/process_metrics.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/scoped_blocking_call.h"
#include "build/build_config.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/grit/branded_strings.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/common/process_type.h"
#include "ui/base/l10n/l10n_util.h"

using base::ProcessEntry;
namespace {

struct Process {
  pid_t pid;
  pid_t parent;
};

typedef std::map<pid_t, Process> ProcessMap;

// Get information on all the processes running on the system.
ProcessMap GetProcesses() {
  ProcessMap map;

  base::ProcessIterator process_iter(nullptr);
  while (const ProcessEntry* process_entry = process_iter.NextProcessEntry()) {
    Process process;
    process.pid = process_entry->pid();
    process.parent = process_entry->parent_pid();
    map.try_emplace(process.pid, process);
  }
  return map;
}

// For each of a list of pids, collect memory information about that process.
ProcessData GetProcessDataMemoryInformation(const std::vector<pid_t>& pids) {
  ProcessData process_data;
  process_data.processes.reserve(pids.size());

  for (pid_t pid : pids) {
    ProcessMemoryInformation pmi;

    pmi.pid = pid;
    pmi.num_processes = 1;

    pmi.process_type = pid == base::GetCurrentProcId()
                           ? content::PROCESS_TYPE_BROWSER
                           : content::PROCESS_TYPE_UNKNOWN;

    std::unique_ptr<base::ProcessMetrics> metrics(
        base::ProcessMetrics::CreateProcessMetrics(pid));
    pmi.num_open_fds = metrics->GetOpenFdCount();
    pmi.open_fds_soft_limit = metrics->GetOpenFdSoftLimit();

    process_data.processes.push_back(pmi);
  }
  return process_data;
}

// Find all children of the given process with pid |root|.
std::vector<pid_t> GetAllChildren(const ProcessMap& processes, pid_t root) {
  std::vector<pid_t> children;
  children.push_back(root);

  std::set<pid_t> wavefront;
  wavefront.insert(root);

  while (!wavefront.empty()) {
    std::set<pid_t> next_wavefront;
    for (const auto& entry : processes) {
      const Process& process = entry.second;
      if (wavefront.count(process.parent)) {
        children.push_back(process.pid);
        next_wavefront.insert(process.pid);
      }
    }
    wavefront.swap(next_wavefront);
  }
  return children;
}

}  // namespace

MemoryDetails::MemoryDetails() = default;

ProcessData* MemoryDetails::ChromeBrowser() {
  return &process_data_[0];
}

void MemoryDetails::CollectProcessData(
    const std::vector<ProcessMemoryInformation>& child_info) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);

  ProcessMap process_map = GetProcesses();

  ProcessData current_browser =
      GetProcessDataMemoryInformation(GetAllChildren(process_map, getpid()));
  current_browser.name = l10n_util::GetStringUTF16(IDS_SHORT_PRODUCT_NAME);
  current_browser.process_name = u"thorium";

  // Build a lookup of the child process info collected on the IO thread so
  // that matching by pid is O(1) instead of a linear scan per process.
  std::map<pid_t, const ProcessMemoryInformation&> child_info_by_pid;
  for (const ProcessMemoryInformation& child : child_info) {
    child_info_by_pid.emplace(child.pid, child);
  }

  for (ProcessMemoryInformation& process : current_browser.processes) {
    // Check if this is one of the child processes whose data we collected
    // on the IO thread, and if so copy over that data.
    auto it = child_info_by_pid.find(process.pid);
    if (it != child_info_by_pid.end()) {
      process.titles = it->second.titles;
      process.process_type = it->second.process_type;
    }
  }

  process_data_.push_back(current_browser);

#if BUILDFLAG(IS_CHROMEOS)
  base::GetSwapInfo(&swap_info_);
#endif

  // Finally return to the browser thread.
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&MemoryDetails::CollectChildInfoOnUIThread, this));
}
