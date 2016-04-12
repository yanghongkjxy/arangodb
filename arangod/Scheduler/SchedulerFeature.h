////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_SCHEDULER_SCHEDULER_FEATURE_H
#define ARANGOD_SCHEDULER_SCHEDULER_FEATURE_H 1

#include "Basics/Common.h"

#include "ApplicationFeatures/ApplicationFeature.h"

namespace arangodb {
namespace rest {
class Scheduler;
class Task;
}

class SchedulerFeature final : public application_features::ApplicationFeature {
 public:
  static rest::Scheduler* SCHEDULER;

 public:
  explicit SchedulerFeature(application_features::ApplicationServer* server);

 public:
  void collectOptions(std::shared_ptr<options::ProgramOptions>) override final;
  void validateOptions(std::shared_ptr<options::ProgramOptions>) override final;
  void start() override final;
  void stop() override final;

 private:
  uint64_t _nrSchedulerThreads;
  uint64_t _backend;
  bool _showBackends;

 public:
  uint64_t backend() const { return _backend; }
  void setProcessorAffinity(std::vector<size_t> const& cores);
  void buildControlCHandler();

 private:
  void buildScheduler();

 private:
  rest::Scheduler* _scheduler;
  std::vector<rest::Task*> _tasks;
};
}

#endif
