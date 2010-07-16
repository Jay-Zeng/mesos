#include <getopt.h>

#include "isolation_module_factory.hpp"
#include "slave.hpp"
#include "slave_webui.hpp"

using boost::lexical_cast;
using boost::bad_lexical_cast;

using namespace std;

using namespace nexus::internal::slave;


void usage(const char *programName)
{
  cerr << "Usage: " << programName
       << " --url MASTER_URL"
       << " [--cpus NUM]"
       << " [--mem NUM]"
       << " [--isolation TYPE]"
       << " [--webui-port PORT]"
       << " [--quiet]" << endl
       << endl
       << "MASTER_URL may be one of:" << endl
       << "  nexus://id@host:port" << endl
       << "  zoo://host1:port1,host2:port2,..." << endl
       << "  zoofile://file where file contains a host:port pair per line"
       << endl;
}


int main(int argc, char **argv)
{
  if (argc == 2 && string("--help") == argv[1]) {
    usage(argv[0]);
    exit(1);
  }

  option options[] = {
    {"url", required_argument, 0, 'u'},
    {"cpus", required_argument, 0, 'c'},
    {"mem", required_argument, 0, 'm'},
    {"isolation", required_argument, 0, 'i'},
    {"webui-port", required_argument, 0, 'w'}, 
    {"quiet", no_argument, 0, 'q'},
  };

  string url = "";
  Resources resources(1, 1 * Gigabyte);
  string isolation = "process";
  char* webuiPortStr = "8081"; // C string because it is sent to python C API
  bool quiet = false;

  int opt;
  int index;
  while ((opt = getopt_long(argc, argv, "u:c:m:i:w:q", options, &index)) != -1) {
    switch (opt) {
      case 'u':
        url = optarg;
        break;
      case 'c': 
        resources.cpus = atoi(optarg);
        break;
      case 'm':
        resources.mem = atoll(optarg);
        break;
      case 'i':
        isolation = optarg;
        break;
      case 'w':
        webuiPortStr = optarg;
        break;
      case 'q':
        quiet = true;
        break;
      case '?':
        // Error parsing options; getopt prints an error message, so just exit
        exit(1);
        break;
      default:
        break;
    }
  }

  if (url == "") {
    cerr << "Master URL argument (--url) required." << endl;
    exit(1);
  }

  if (!quiet)
    google::SetStderrLogging(google::INFO);

  FLAGS_log_dir = "/tmp";
  FLAGS_logbufsecs = 1;
  google::InitGoogleLogging(argv[0]);

  LOG(INFO) << "Creating \"" << isolation << "\" isolation module";
  IsolationModule *isolationModule = IsolationModule::create(isolation);

  if (isolationModule == NULL)
    fatal("unrecognized isolation type: %s", isolation.c_str());

  LOG(INFO) << "Build: " << BUILD_DATE << " by " << BUILD_USER;
  LOG(INFO) << "Starting Nexus slave";

  Slave* slave = new Slave(resources, false, isolationModule);
  PID pid = Process::spawn(slave);

  MasterDetector *detector = MasterDetector::create(url, pid, false, quiet);

#ifdef NEXUS_WEBUI
  if (chdir(dirname(argv[0])) != 0)
    fatalerror("could not change into %s for running webui", dirname(argv[0]));

  // TODO(*): Since we normally don't use exceptions in Mesos, replace
  // use of an exception here with use of a utility to handle checking
  // that the input string is actually a number that fits
  // in the type being used (in this case, short).
  try {
    lexical_cast<short>(webuiPortStr);
  } catch(bad_lexical_cast &) {
    fatal("Passed invalid string for webui port number.\n");
  }

  startSlaveWebUI(pid, webuiPortStr);
#endif

  Process::wait(pid);

  MasterDetector::destroy(detector);

  IsolationModule::destroy(isolationModule);

  delete slave;

  return 0;
}
