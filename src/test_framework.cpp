#include <iostream>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <nexus_sched.h>

using namespace std;

const int TOTAL_TASKS = 5;
int tasksStarted = 0;
int tasksFinished = 0;


void registered(nexus_sched *sched, framework_id fid)
{
  cout << "Registered with Nexus, framework ID = " << fid << endl;
}


void slot_offer(nexus_sched *sched, offer_id oid,
                nexus_slot *slots, int num_slots)
{
  // TODO: Make this loop over offers rather than looking only at first offer!
  cout << "Got slot offer " << oid << endl;
  if (tasksStarted >= TOTAL_TASKS) {
    cout << "Refusing it" << endl;
    nexus_sched_reply_to_offer(sched, oid, 0, 0, "timeout=-1");
  } else {
    task_id tid = tasksStarted++;
    cout << "Accepting it to start task " << tid << endl;
    nexus_task_desc desc = { tid, slots[0].sid, "task",
      "cpus=1\nmem=33554432", 0, 0 };
    nexus_sched_reply_to_offer(sched, oid, &desc, 1, "");
    if (tasksStarted > 4)
      nexus_sched_unreg(sched);
  }
}


void slot_offer_rescinded(nexus_sched *sched, offer_id oid)
{
  cout << "Slot offer rescinded: " << oid << endl;
}


void status_update(nexus_sched *sched, nexus_task_status *status)
{
  cout << "Task " << status->tid << " entered state " << status->state << endl;
  if (status->state == TASK_FINISHED) {
    tasksFinished++;
    if (tasksFinished == TOTAL_TASKS)
      exit(0);
  }
}


void framework_message(nexus_sched *sched, nexus_framework_message *msg)
{
  cout << "Got a framework message from slave " << msg->sid
       << ": " << (char *) msg->data << endl;
}


void slave_lost(nexus_sched *sched, slave_id sid)
{
  cout << "Lost slave " << sid << endl;
}  


void error(nexus_sched *sched, int code, const char *message)
{
  cout << "Error from Nexus: " << message << endl;
}


nexus_sched sched = {
  "test framework",
  "", // Executor (will be set in main to get absolute path)
  registered,
  slot_offer,
  slot_offer_rescinded,
  status_update,
  framework_message,
  slave_lost,
  error,
  (void *) "test",
  4,
  0
};


int main(int argc, char **argv)
{
  // Find this executable's directory to locate executor
  char buf[4096];
  realpath(dirname(argv[0]), buf);
  string executor = string(buf) + "/test-executor";
  sched.executor_name = executor.c_str();

  if (nexus_sched_init(&sched) < 0) {
    perror("nexus_sched_init");
    return -1;
  }

  if (nexus_sched_reg(&sched, argv[1]) < 0) {
    perror("nexus_sched_reg");
    return -1;
  }

  nexus_sched_join(&sched);

  if (nexus_sched_destroy(&sched) < 0) {
    perror("nexus_sched_destroy");
    return -1;
  }

  return 0;
}
