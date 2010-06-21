#!/usr/bin/env python
import nexus
import sys
import time
import os

from subprocess import *

class MyExecutor(nexus.Executor):
  def __init__(self):
    nexus.Executor.__init__(self)

  def init(self, driver, arg):
    print "in daemon executor"

  def launchTask(self, driver, task):
    print "in launchTask"
    self.tid = task.taskId
    print "task id is " + str(task.taskId) + ", task.args is " + task.arg
    self.args = task.arg.split("\t")
    print "running: " + "java -cp " + self.args[0] + " " + self.args[1] + " " + self.args[2]
    print Popen("java -cp " + self.args[0] + " " + self.args[1] + " " + self.args[2], shell=True, stdout=PIPE).stdout.readline()
    update = nexus.TaskStatus(task.taskId, nexus.TASK_FINISHED, "")
    driver.sendStatusUpdate(update)
    

  def error(self, code, message):
    print "Error: %s" % message

if __name__ == "__main__":
  print "starting daemon framework executor"
  executor = MyExecutor()
  nexus.NexusExecutorDriver(executor).run()