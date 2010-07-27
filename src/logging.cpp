#include "logging.hpp"

#include <sys/stat.h>

#include <glog/logging.h>

#include "fatal.hpp"

using std::string;
using namespace mesos::internal;


void Logging::registerOptions(Configurator* conf)
{
  conf->addOption<bool>("quiet", 'q', "Disable logging to stderr", false);
  conf->addOption<string>("log_dir",
                          "Where to put logs (default: MESOS_HOME/logs)");
}


void Logging::init(const char* programName, const Params& conf)
{
  // Set glog's parameters through Google Flags variables
  string logDir = getLogDir(conf);
  if (logDir != "") {
    if (mkdir(logDir.c_str(), 0755) < 0 && errno != EEXIST) {
      fatalerror("Failed to create log directory %s", logDir.c_str());
    }
    FLAGS_log_dir = logDir;
  }
  FLAGS_logbufsecs = 1;
  google::InitGoogleLogging(programName);

  if (!isQuiet(conf))
    google::SetStderrLogging(google::INFO);
}


string Logging::getLogDir(const Params& conf)
{
  if (conf.contains("log_dir"))
    return conf.get("log_dir", "");
  else if (conf.contains("home"))
    return conf.get("home", "") + "/logs";
  else
    return "";
}


bool Logging::isQuiet(const Params& conf)
{
  return conf.get<bool>("quiet", false);
}
