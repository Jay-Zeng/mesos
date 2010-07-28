import java.io.File;
import mesos.*;

public class TestFramework {
  static {
    System.loadLibrary("mesos");
  }

  static class MyScheduler extends Scheduler {
    int launchedTasks = 0;
    int finishedTasks = 0;
    int totalTasks = 5;

    public MyScheduler() {}

    public MyScheduler(int numTasks) {
      totalTasks = numTasks;
    }

    @Override
    public String getFrameworkName(SchedulerDriver d) {
      return "Java test framework";
    }

    @Override
    public ExecutorInfo getExecutorInfo(SchedulerDriver d) {
      try {
        return new ExecutorInfo(
            new File("./test_executor").getCanonicalPath(),
            new byte[0]);
      } catch (Exception e) {
        e.printStackTrace();
        System.exit(1);
        return null;
      }
    }

    @Override
    public void registered(SchedulerDriver d, String fid) {
      System.out.println("Registered! FID = " + fid);
    }

    @Override
    public void resourceOffer(SchedulerDriver d,
                              String oid,
                              SlaveOfferVector offers) {
      System.out.println("Got offer offer " + oid);
      TaskDescriptionVector tasks = new TaskDescriptionVector();
      for (int i = 0; i < offers.size(); i++) {
        if (launchedTasks < totalTasks) {
          SlaveOffer offer = offers.get(i);
          int taskId = launchedTasks++;
          StringMap taskParams = new StringMap();
          taskParams.set("cpus", "1");
          taskParams.set("mem", "134217728");
          System.out.println("Launching task " + taskId);
          tasks.add(new TaskDescription(taskId,
                                        offer.getSlaveId(),
                                        "task " + taskId,
                                        taskParams,
                                        new byte[0]));
        }
      }
      StringMap params = new StringMap();
      params.set("timeout", "1");
      d.replyToOffer(oid, tasks, params);
    }

    @Override
    public void statusUpdate(SchedulerDriver d, TaskStatus status) {
      System.out.println("Status update: task " + status.getTaskId() +
                         " is in state " + status.getState());
      if (status.getState() == TaskState.TASK_FINISHED) {
        finishedTasks++;
        System.out.println("Finished tasks: " + finishedTasks);
        if (finishedTasks == totalTasks)
          d.stop();
      }
    }

    @Override
    public void error(SchedulerDriver d, int code, String message) {
      System.out.println("Error: " + message);
    }
  }

  public static void main(String[] args) throws Exception {
    if (args.length < 1 || args.length > 2) {
      System.out.println("Invalid use: please specify a master");
    } else if (args.length == 1) {
      new MesosSchedulerDriver(new MyScheduler(),args[0]).run();
    } else {
      new MesosSchedulerDriver(new MyScheduler(Integer.parseInt(args[1])), args[0]).run();
    }
  }
}
