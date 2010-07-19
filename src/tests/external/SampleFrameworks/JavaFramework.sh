#!/bin/sh

# Set local Mesos runner to use 3 slaves
export MESOS_SLAVES=3

# Check that the Java test framework executes without crashing (returns 0).
exec $MESOS_HOME/swig/java/test_framework local
