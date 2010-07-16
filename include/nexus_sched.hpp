#ifndef NEXUS_SCHED_HPP
#define NEXUS_SCHED_HPP

#include <nexus.hpp>

#include <string>
#include <vector>


namespace nexus {

class SchedulerDriver;

namespace internal { class SchedulerProcess; class MasterDetector; }


/**
 * Callback interface to be implemented by new frameworks' schedulers.
 */
class Scheduler
{
public:
  virtual ~Scheduler() {}

  // Callbacks for getting framework properties
  virtual std::string getFrameworkName(SchedulerDriver*);
  virtual ExecutorInfo getExecutorInfo(SchedulerDriver*);

  // Callbacks for various Nexus events
  virtual void registered(SchedulerDriver* d, FrameworkID fid) {}
  virtual void resourceOffer(SchedulerDriver* d,
                             OfferID oid,
                             const std::vector<SlaveOffer>& offers) {}
  virtual void offerRescinded(SchedulerDriver* d, OfferID oid) {}
  virtual void statusUpdate(SchedulerDriver* d, const TaskStatus& status) {}
  virtual void frameworkMessage(SchedulerDriver* d,
                                const FrameworkMessage& message) {}
  virtual void slaveLost(SchedulerDriver* d, SlaveID sid) {}
  virtual void error(SchedulerDriver* d, int code, const std::string& message);
};


/**
 * Abstract interface for driving a scheduler connected to Nexus.
 * This interface is used both to manage the scheduler's lifecycle (start it,
 * stop it, or wait for it to finish) and to send commands from the user
 * framework to Nexus (such as replies to offers). Concrete implementations
 * of SchedulerDriver will take a Scheduler as a parameter in order to make
 * callbacks into it on various events.
 */
class SchedulerDriver
{
public:
  virtual ~SchedulerDriver() {}

  // Lifecycle methods
  virtual int start() { return -1; }
  virtual int stop() { return -1; }
  virtual int join() { return -1; }
  virtual int run() { return -1; } // Start and then join driver

  // Communication methods
  virtual int sendFrameworkMessage(const FrameworkMessage& message) { return -1; }
  virtual int killTask(TaskID tid) { return -1; }
  virtual int replyToOffer(OfferID oid,
			   const std::vector<TaskDescription>& task,
			   const string_map& params) { return -1; }
  virtual int reviveOffers() { return -1; }
  virtual int sendHints(const string_map& hints) { return -1; }
};


/**
 * Concrete implementation of SchedulerDriver that communicates with
 * a Nexus master.
 */
class NexusSchedulerDriver : public SchedulerDriver
{
public:
  NexusSchedulerDriver(Scheduler* sched,
		       const std::string& url,
		       FrameworkID fid = "");

  virtual ~NexusSchedulerDriver();

  // Lifecycle methods
  virtual int start();
  virtual int stop();
  virtual int join();
  virtual int run(); // Start and then join driver

  // Communication methods
  virtual int sendFrameworkMessage(const FrameworkMessage& message);
  virtual int killTask(TaskID tid);
  virtual int replyToOffer(OfferID offerId,
			   const std::vector<TaskDescription>& task,
			   const string_map& params);
  virtual int reviveOffers();
  virtual int sendHints(const string_map& hints);

  // Scheduler getter; required by some of the SWIG proxies
  virtual Scheduler* getScheduler() { return sched; }

private:
  // Internal utility method to report an error to the scheduler
  void error(int code, const std::string& message);

  Scheduler* sched;
  std::string url;
  FrameworkID fid;

  // LibProcess process for communicating with master
  internal::SchedulerProcess* process;

  // Coordination between masters
  internal::MasterDetector* detector;

  // Are we currently registered with the master
  bool running;
  
  // Mutex to enforce all non-callbacks are execute serially
  pthread_mutex_t mutex;

  // Condition variable for waiting until driver terminates
  pthread_cond_t cond;

};


} /* namespace nexus { */

#endif /* NEXUS_SCHED_HPP */
