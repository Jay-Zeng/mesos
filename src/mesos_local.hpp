#ifndef __MESOS_LOCAL_HPP__
#define __MESOS_LOCAL_HPP__

// Include the master and slave headers here so that the mesos_sched
// library will re-compile when they are changed.
#include "master.hpp"
#include "slave.hpp"

#include "configurator.hpp"

namespace mesos { namespace internal { namespace local {

void registerOptions(Configurator* conf);

PID launch(int numSlaves, int32_t cpus, int64_t mem,
	   bool initLogging, bool quiet);

PID launch(const Params& conf, bool initLogging);

void shutdown();

}}}

#endif /* __MESOS_LOCAL_HPP__ */
