/* TODO(benh): Compile with a way to figure out which set of messages you used, and that way when someone with a different set of messages sends you a message you can declare that that message is not in your language of understanding. */
/* TODO(benh): Fix link functionality (processes need to send process_exit message since a dead process on one node might not know that a process on another node linked with it). */
/* TODO(benh): What happens when a remote link exits? Do we close the socket correclty?. */
/* TODO(benh): Revisit receive, pause, and await semantics. */
/* TODO(benh): Handle/Enable forking. */
/* TODO(benh): Use multiple processing threads (do process affinity). */
/* TODO(benh): Reclaim/Recycle stack (use Lithe!). */
/* TODO(benh): Better error handling (i.e., warn if re-spawn process). */
/* TODO(benh): Better protocol format checking in read_msg. */
/* TODO(benh): Use different backends for files and sockets. */
/* TODO(benh): Allow messages to be received out-of-order (i.e., allow
   someone to do a receive with a message id and let other messages
   queue until a message with that message id is received).  */
/* TODO(benh): LinkManager::link and LinkManager::send are pretty big
   functions, we could probably create some queue that the I/O thread
   checks for sending messages and creating links instead ... that
   would probably be faster, and have less contention for the mutex
   (that might mean we can eliminate contention for the mutex!). */

#include <assert.h>
#include <errno.h>
#include <ev.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#include <arpa/inet.h>

#ifdef USE_LITHE
#include <ht/atomic.h>
#endif /* USE_LITHE */

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include <boost/tuple/tuple.hpp>

#include <algorithm>
#include <deque>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <list>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stack>
#include <stdexcept>

#include "fatal.hpp"
#include "foreach.hpp"
#include "gate.hpp"
#include "process.hpp"
#include "singleton.hpp"
#include "utility.hpp"

using boost::make_tuple;
using boost::tuple;

using std::cout;
using std::cerr;
using std::deque;
using std::endl;
using std::find;
using std::list;
using std::make_pair;
using std::map;
using std::max;
using std::pair;
using std::queue;
using std::set;
using std::stack;


#ifdef __sun__
#define gethostbyname2(name, _) gethostbyname(name)
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif
#ifndef MAP_32BIT
#define MAP_32BIT 0
#endif
#endif /* __sun__ */

#ifdef __APPLE__
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif
#ifndef MAP_32BIT
#define MAP_32BIT 0
#endif
#endif /* __APPLE__ */



#define Byte (1)
#define Kilobyte (1024*Byte)
#define Megabyte (1024*Kilobyte)
#define Gigabyte (1024*Megabyte)
#define PROCESS_STACK_SIZE (64*Kilobyte)


#define malloc(bytes)                                               \
  ({ void *tmp;                                                     \
     if ((tmp = malloc(bytes)) == NULL)                             \
       fatalerror("malloc"); tmp;                                   \
   })

#define realloc(address, bytes)                                     \
  ({ void *tmp;                                                     \
     if ((tmp = realloc(address, bytes)) == NULL)                   \
       fatalerror("realloc"); tmp;                                  \
   })


#ifdef USE_LITHE
#define acquire(l) spinlock_lock(&l ## _lock)
#define release(l) spinlock_unlock(&l ## _lock)
class Synchronized
{
public:
  int *lock;
  Synchronized(int *_lock)
    : lock(_lock) { spinlock_lock(lock); }
  ~Synchronized() { spinlock_unlock(lock); }
  operator bool () { return true; }
};
#define synchronized(l) if (Synchronized s = Synchronized(&l ## _lock))
#else
#define acquire(l) pthread_mutex_lock(&l ## _mutex)
#define release(l) pthread_mutex_unlock(&l ## _mutex)
class Synchronized
{
public:
  pthread_mutex_t *mutex;
  Synchronized(pthread_mutex_t *_mutex)
    : mutex(_mutex) { pthread_mutex_lock(mutex); }
  ~Synchronized() { pthread_mutex_unlock(mutex); }
  operator bool () { return true; }
};
#define synchronized(l) if (Synchronized s = Synchronized(&l ## _mutex))
#endif /* USE_LITHE */


/* Local server socket. */
static int s = -1;

/* Local IP address. */
static uint32_t ip = 0;

/* Local port. */
static uint16_t port = 0;

/* Event loop. */
static struct ev_loop *loop = NULL;

/* Queue of new I/O watchers. */
static queue<ev_io *> *io_watchersq = new queue<ev_io *>();

/* Watcher queues lock/mutex. */
#ifdef USE_LITHE
static int io_watchersq_lock = UNLOCKED;
#else
static pthread_mutex_t io_watchersq_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif /* USE_LITHE */

/* Asynchronous watcher for interrupting loop. */
static ev_async async_watcher;

/* Timer watcher for process timeouts. */
static ev_timer timer_watcher;

/* Process timers lock/mutex. */
#ifdef USE_LITHE
static int timers_lock = UNLOCKED;
#else
static pthread_mutex_t timers_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif /* USE_LITHE */

/* Map of process timers (we exploit that the map is SORTED!). */
typedef tuple<ev_tstamp, Process *, int> timeout_t;
typedef list<timeout_t> timeouts_t;
static map<ev_tstamp, timeouts_t> *timers =
  new map<ev_tstamp, timeouts_t>();

/* Flag to indicate whether or to update the timer on async interrupt. */
static bool update_timer = false;

/* Server watcher for accepting connections. */
static ev_io server_watcher;

/* I/O thread. */
static pthread_t io_thread;

/* Processing thread. */
static pthread_t proc_thread;

/* Scheduling context for processing thread. */
static ucontext_t proc_uctx_schedule;

/* Running context for processing thread. */
static ucontext_t proc_uctx_running;

/* Current process of processing thread. */
//static __thread Process *proc_process = NULL;
static Process *proc_process = NULL;

/* Flag indicating if performing safe call into legacy. */
// static __thread bool legacy = false;
static bool legacy = false;

/* Thunk to safely call into legacy. */
// static __thread std::tr1::function<void (void)> *legacy_thunk;
static const std::tr1::function<void (void)> *legacy_thunk;

/* Global 'pipe' id uniquely assigned to each process. */
static uint32_t global_pipe = 0;

/* Status of processing thread. */
static int idle = 0;

/* Scheduler gate. */
static Gate *gate = new Gate();

/* Stack of stacks. */
static stack<void *> *stacks = new stack<void *>();

/* Record? */
static bool recording = false;

/* Record(s) for replay. */
static std::fstream record_msgs;
static std::fstream record_pipes;

/* Replay? */
static bool replaying = false;

/* Replay messages (id -> queue of messages). */
static map<uint32_t, queue<struct msg *> > *replay_msgs =
  new map<uint32_t, queue<struct msg *> >();

/* Replay pipes (parent id -> stack of remaining child ids). */
static map<uint32_t, deque<uint32_t> > *replay_pipes =
  new map<uint32_t, deque<uint32_t> >();

/* Filter? */
static bool filtering = false;

/* Filter. */
static MessageFilter *filterer = NULL;

/*
 * Filtering mutex (needs to be recursive incase a filterer wants to
 * do anything fancy, which is possible given that filters will get
 * used for testing).
*/
static pthread_mutex_t filter_mutex;

/* Tick, tock ... manually controlled clock! */
class InternalProcessClock
{
public:
  InternalProcessClock()
  {
    initial = current = elapsed = ev_time();
  }

  ~InternalProcessClock() {}

  ev_tstamp getCurrent(Process *process)
  {
    ev_tstamp tstamp;

    if (currents.count(process) != 0) {
      tstamp = currents[process];
    } else {
      tstamp = currents[process] = initial;
    }

    return tstamp;
  }

  void setCurrent(Process *process, ev_tstamp tstamp)
  {
    currents[process] = tstamp;
  }

  ev_tstamp getCurrent()
  {
    return current;
  }

  void setCurrent(ev_tstamp tstamp)
  {
    current = tstamp;
  }

  ev_tstamp getElapsed()
  {
    return elapsed;
  }

  void setElapsed(ev_tstamp tstamp)
  {
    elapsed = tstamp;
  }

  void discard(Process *process)
  {
    assert(process != NULL);
    currents.erase(process);
  }

private:
  map<Process *, ev_tstamp> currents;
  ev_tstamp initial;
  ev_tstamp current;
  ev_tstamp elapsed;
};

static InternalProcessClock *clk = NULL;


struct write_ctx {
  int len;
  struct msg *msg;
  bool close;
};

struct read_ctx {
  int len;
  struct msg *msg;
};


static void initialize();

void handle_await(struct ev_loop *loop, ev_io *w, int revents);

void read_msg(struct ev_loop *loop, ev_io *w, int revents);
void write_msg(struct ev_loop *loop, ev_io *w, int revents);
void write_connect(struct ev_loop *loop, ev_io *w, int revents);

void link_connect(struct ev_loop *loop, ev_io *w, int revents);

#ifdef USE_LITHE
void trampoline(void *arg);
#else
void trampoline(int process0, int process1);
#endif /* USE_LITHE */


PID make_pid(const char *str)
{
  PID pid = { 0 };
  std::istringstream iss(str);
  iss >> pid;
  return pid;
}


PID::operator std::string() const
{
  std::ostringstream oss;
  oss << *this;
  return oss.str();
}


bool PID::operator ! () const
{
  return !pipe && !ip && !port;
}


std::ostream& operator << (std::ostream& stream, const PID& pid)
{
  /* Call inet_ntop since inet_ntoa is not thread-safe! */
  char ip[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, (in_addr *) &pid.ip, ip, INET_ADDRSTRLEN) == NULL)
    memset(ip, 0, INET_ADDRSTRLEN);

  stream << pid.pipe << "@" << ip << ":" << pid.port;
  return stream;
}


std::istream& operator >> (std::istream& stream, PID& pid)
{
  pid.pipe = 0;
  pid.ip = 0;
  pid.port = 0;

  std::string str;
  if (!(stream >> str)) {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  if (str.size() > 500) {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  char host[512];
  int id;
  unsigned short port;
  if (sscanf(str.c_str(), "%d@%[^:]:%hu", &id, host, &port) != 3) {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  hostent *he = gethostbyname2(host, AF_INET);
  if (!he) {
    stream.setstate(std::ios_base::badbit);
    return stream;
  }

  pid.pipe = id;
  pid.ip = *((uint32_t *) he->h_addr);
  pid.port = port;
  return stream;
}


bool operator < (const PID& left, const PID& right)
{
  if (left.ip == right.ip && left.port == right.port)
    return left.pipe < right.pipe;
  else if (left.ip == right.ip && left.port != right.port)
    return left.port < right.port;
  else
    return left.ip < right.ip;
}


bool operator == (const PID& left, const PID& right)
{
  return (left.pipe == right.pipe &&
	  left.ip == right.ip &&
	  left.port == right.port);
}


void ProcessClock::pause()
{
  initialize();

  acquire(timers);
  {
    // For now, only one global clock (rather than clock per
    // process). This Means that we have to take special care to
    // ensure happens-before timing (currently done for local message
    // sends and spawning new processes, not currently done for
    // PROCESS_EXIT messages).
    if (clk == NULL) {
      clk = new InternalProcessClock();

      // The existing libev timer might actually timeout, but now that
      // clk != NULL, no "time" will actually have passed, so no
      // timeouts will actually occur.
    }
  }
  release(timers);
}


void ProcessClock::resume()
{
  initialize();

  acquire(timers);
  {
    if (clk != NULL) {
      delete clk;
      clk = NULL;
    }

    update_timer = true;
    ev_async_send(loop, &async_watcher);
  }
  release(timers);
}


void ProcessClock::advance(double secs)
{
  acquire(timers);
  {
    if (clk != NULL) {
      clk->setElapsed(clk->getElapsed() + secs);

      // Might need to wakeup the processing thread.
      gate->open();
    }
  }
  release(timers);
}


static inline int set_nbio(int fd)
{
  int flags;

  /* If they have O_NONBLOCK, use the Posix way to do it. */
#ifdef O_NONBLOCK
  /* TODO(*): O_NONBLOCK is defined but broken on SunOS 4.1.x and AIX 3.2.5. */
  if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
    flags = 0;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
  /* Otherwise, use the old way of doing it. */
  flags = 1;
  return ioctl(fd, FIOBIO, &flags);
#endif
}


struct node { uint32_t ip; uint16_t port; };

bool operator < (const node& left, const node& right)
{
  if (left.ip == right.ip)
    return left.port < right.port;
  else
    return left.ip < right.ip;
}

std::ostream& operator << (std::ostream& stream, const node& n)
{
  stream << n.ip << ":" << n.port;
  return stream;
}


class LinkManager : public Singleton<LinkManager>
{
private:
  /* Map from PID (local/remote) to process. */
  map<PID, set<Process *> > links;

  /* Map from socket to node (ip, port). */
  map<int, node> sockets;

  /* Maps from node (ip, port) to socket. */
  map<node, int> temps;
  map<node, int> persists;

  /* Map from socket to outgoing messages. */
  map<int, queue<struct msg *> > outgoing;

  pthread_mutex_t mutex;

  friend class Singleton<LinkManager>;

  LinkManager()
  {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mutex, &attr);
    pthread_mutexattr_destroy(&attr);
  }

public:
  /*
   * TODO(benh): The semantics we want to support for link are such
   * that if there is nobody to link to (local or remote) then a
   * PROCESS_EXIT message gets generated. This functionality has only
   * been implemented when the link is local, not remote. Of course,
   * if there is nobody listening on the remote side, then this should
   * work remotely ... but if there is someone listening remotely just
   * not at that pipe value, then it will silently continue executing.
  */
  void link(Process *process, const PID &to)
  {
    //cout << "calling link" << endl;

    assert(process != NULL);

    node n = { to.ip, to.port };

    pthread_mutex_lock(&mutex);
    {
      // Check if node is remote and there isn't a persistant link.
      if ((n.ip != ip || n.port != port) &&
	  persists.find(n) == persists.end()) {
	int s;

	/* Create socket for communicating with remote process. */
	if ((s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP)) < 0)
	  fatalerror("failed to link (socket)");
    
	/* Use non-blocking sockets. */
	if (set_nbio(s) < 0)
	  fatalerror("failed to link (set_nbio)");

	//cout << "created linked socket " << s << endl;

	/* Record socket. */
	sockets[s] = n;

	/* Record node. */
	persists[n] = s;

	/* Allocate the watcher. */
	ev_io *io_watcher = (ev_io *) malloc(sizeof(ev_io));

	struct sockaddr_in addr;
      
	memset(&addr, 0, sizeof(addr));
      
	addr.sin_family = PF_INET;
	addr.sin_port = htons(to.port);
	addr.sin_addr.s_addr = to.ip;

	if (connect(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
	  if (errno != EINPROGRESS)
	    fatalerror("failed to link (connect)");

	  /* Initialize watcher for connecting. */
	  ev_io_init(io_watcher, link_connect, s, EV_WRITE);
	} else {
	  /* Initialize watcher for reading. */
	  io_watcher->data = malloc(sizeof(struct read_ctx));

	  /* Initialize read context. */
	  struct read_ctx *ctx = (struct read_ctx *) io_watcher->data;

	  ctx->len = 0;
	  ctx->msg = (struct msg *) malloc(sizeof(struct msg));

	  ev_io_init(io_watcher, read_msg, s, EV_READ);
	}

	/* Enqueue the watcher. */
	acquire(io_watchersq);
	{
	  io_watchersq->push(io_watcher);
	}
	release(io_watchersq);

	/* Interrupt the loop. */
	ev_async_send(loop, &async_watcher);
      }

      links[to].insert(process);
    }
    pthread_mutex_unlock(&mutex);
  }

  void send(struct msg *msg)
  {
    assert(msg != NULL);

    //cout << "(1) sending msg to " << msg->to << endl;

    node n = { msg->to.ip, msg->to.port };

    pthread_mutex_lock(&mutex);
    {
      // Check if there is already a link.
      map<node, int>::iterator it;
      if ((it = persists.find(n)) != persists.end() ||
	  (it = temps.find(n)) != temps.end()) {
	int s = it->second;
	//cout << "(2) found a socket " << s << endl;
	if (outgoing.find(s) == outgoing.end()) {
	  assert(persists.find(n) != persists.end());
	  assert(temps.find(n) == temps.end());
	  //cout << "(3) reusing (sleeping persistant) socket " << s << endl;

	  /* Initialize the outgoing queue. */
	  outgoing[s];

	  /* Allocate/Initialize the watcher. */
	  ev_io *io_watcher = (ev_io *) malloc (sizeof (ev_io));

	  io_watcher->data = malloc(sizeof(struct write_ctx));

	  /* Initialize the write context. */
	  struct write_ctx *ctx = (struct write_ctx *) io_watcher->data;

	  ctx->len = 0;
	  ctx->msg = msg;
	  ctx->close = false;

	  ev_io_init(io_watcher, write_msg, s, EV_WRITE);

	  /* Enqueue the watcher. */
	  acquire(io_watchersq);
	  {
	    io_watchersq->push(io_watcher);
	  }
	  release(io_watchersq);
    
	  /* Interrupt the loop. */
	  ev_async_send(loop, &async_watcher);
	} else {
	  //cout << "(3) reusing socket " << s << endl;
	  outgoing[s].push(msg);
	}
      } else {
	int s;

	/* Create socket for communicating with remote process. */
	if ((s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP)) < 0)
	  fatalerror("failed to send (socket)");
    
	/* Use non-blocking sockets. */
	if (set_nbio(s) < 0)
	  fatalerror("failed to send (set_nbio)");

	//cout << "(2) created temporary socket " << s << endl;

	/* Record socket. */
	sockets[s] = n;

	/* Record node. */
	temps[n] = s;

	/* Initialize the outgoing queue. */
	outgoing[s];

	/* Allocate/Initialize the watcher. */
	ev_io *io_watcher = (ev_io *) malloc (sizeof (ev_io));

	io_watcher->data = malloc(sizeof(struct write_ctx));

	/* Initialize the write context. */
	struct write_ctx *ctx = (struct write_ctx *) io_watcher->data;

	ctx->len = 0;
	ctx->msg = msg;
	ctx->close = true;

	struct sockaddr_in addr;
      
	memset(&addr, 0, sizeof(addr));
      
	addr.sin_family = PF_INET;
	addr.sin_port = htons(msg->to.port);
	addr.sin_addr.s_addr = msg->to.ip;
    
	if (connect(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
	  if (errno != EINPROGRESS)
	    fatalerror("failed to send (connect)");

	  /* Initialize watcher for connecting. */
	  ev_io_init(io_watcher, write_connect, s, EV_WRITE);
	} else {
	  /* Initialize watcher for writing. */
	  ev_io_init(io_watcher, write_msg, s, EV_WRITE);
	}

	/* Enqueue the watcher. */
	acquire(io_watchersq);
	{
	  io_watchersq->push(io_watcher);
	}
	release(io_watchersq);

	/* Interrupt the loop. */
	ev_async_send(loop, &async_watcher);
      }
    }
    pthread_mutex_unlock(&mutex);
  }

  struct msg * next(int s)
  {
    struct msg *msg = NULL;
    pthread_mutex_lock(&mutex);
    {
      assert(outgoing.find(s) != outgoing.end());
      if (!outgoing[s].empty()) {
	msg = outgoing[s].front();
	outgoing[s].pop();
      }
    }
    pthread_mutex_unlock(&mutex);
    return msg;
  }

  struct msg * next_or_close(int s)
  {
    //cout << "next_or_close socket " << s << endl;
    struct msg *msg;
    pthread_mutex_lock(&mutex);
    {
      if ((msg = next(s)) == NULL) {
	assert(outgoing[s].empty());
	outgoing.erase(s);
	assert(temps.find(sockets[s]) != temps.end());
	temps.erase(sockets[s]);
	sockets.erase(s);
	::close(s);
      }
    }
    pthread_mutex_unlock(&mutex);
    return msg;
  }

  struct msg * next_or_sleep(int s)
  {
    //cout << "next_or_sleep socket " << s << endl;
    struct msg *msg;
    pthread_mutex_lock(&mutex);
    {
      if ((msg = next(s)) == NULL) {
	assert(outgoing[s].empty());
	outgoing.erase(s);
	assert(persists.find(sockets[s]) != persists.end());
      }
    }
    pthread_mutex_unlock(&mutex);
    return msg;
  }

  void closed(int s)
  {
    //cout << "closed socket " << s << endl;
    pthread_mutex_lock(&mutex);
    {
      map<int, node>::iterator it = sockets.find(s);
      if (it != sockets.end()) {
	exited(it->second);
	persists.erase(sockets[s]);
	temps.erase(sockets[s]);
	sockets.erase(s);
	outgoing.erase(s);
	::close(s);
      }
    }
    pthread_mutex_unlock(&mutex);
  }

  /*
   * TODO(benh): It would be cleaner if these exited routines could
   * call back into ProcessManager ... then we wouldn't have to
   * convince ourselves that the accesses to each Process object will
   * always be valid.
   */

  void exited(const node &n)
  {
    pthread_mutex_lock(&mutex);
    {
      list<PID> removed;
      /* Look up all linked processes. */
      foreachpair (const PID &pid, set<Process *> &processes, links) {
	if (pid.ip == n.ip && pid.port == n.port) {
	  /* N.B. If we call exited(pid) we might invalidate iteration. */
	  /* Deliver PROCESS_EXIT messages (if we aren't replaying). */
	  if (!replaying) {
	    foreach (Process *process, processes) {
	      struct msg *msg = (struct msg *) malloc(sizeof(struct msg));
	      msg->from.pipe = pid.pipe;
	      msg->from.ip = pid.ip;
	      msg->from.port = pid.port;
	      msg->to.pipe = process->pid.pipe;
	      msg->to.ip = process->pid.ip;
	      msg->to.port = process->pid.port;
	      msg->id = PROCESS_EXIT;
	      msg->len = 0;
	      process->enqueue(msg);
	    }
	  }
	  removed.push_back(pid);
	}
      }
      foreach (const PID &pid, removed)
	links.erase(pid);
    }
    pthread_mutex_unlock(&mutex);
  }

  void exited(Process *process)
  {
    pthread_mutex_lock(&mutex);
    {
      /* Remove any links this process might have had. */
      foreachpair (_, set<Process *> &processes, links)
	processes.erase(process);

      const PID &pid = process->getPID();

      /* Look up all linked processes. */
      map<PID, set<Process *> >::iterator it = links.find(pid);

      if (it != links.end()) {
	set<Process *> &processes = it->second;
	/* Deliver PROCESS_EXIT messages (if we aren't replaying). */
	if (!replaying) {
	  foreach (Process *p, processes) {
	    assert(process != p);
	    struct msg *msg = (struct msg *) malloc(sizeof(struct msg));
	    msg->from.pipe = pid.pipe;
	    msg->from.ip = pid.ip;
	    msg->from.port = pid.port;
	    msg->to.pipe = p->pid.pipe;
	    msg->to.ip = p->pid.ip;
	    msg->to.port = p->pid.port;
	    msg->id = PROCESS_EXIT;
	    msg->len = 0;
            // TODO(benh): Preserve happens-before when using clock.
	    p->enqueue(msg);
	  }
	}
	links.erase(pid);
      }
    }
    pthread_mutex_unlock(&mutex);
  }
};

/* Singleton LinkManager instance. */
template<> LinkManager * Singleton<LinkManager>::singleton = NULL;
template<> bool Singleton<LinkManager>::instantiated = false;


class ProcessManager : public Singleton<ProcessManager>
{
private:
  /* Map of all local spawned and running processes. */
  map<uint32_t, Process *> processes;

  /* Map of all waiting processes. */
  map<Process *, set<Process *> > waiters;

  /* Map of gates for waiting threads. */
  map<Process *, Gate *> gates;

  /* Processes lock/mutex. */
#ifdef USE_LITHE
  int processes_lock;
#else
  pthread_mutex_t processes_mutex;
#endif /* USE_LITHE */

  /* Queue of runnable processes (implemented as deque). */
  deque<Process *> runq;

  /* Run queue lock/mutex. */
#ifdef USE_LITHE
  int runq_lock;
#else
  pthread_mutex_t runq_mutex;
#endif /* USE_LITHE */

  friend class Singleton<ProcessManager>;

  ProcessManager()
  {
#ifdef USE_LITHE
    processes_lock = UNLOCKED;
    runq_lock = UNLOCKED;
#else
    pthread_mutex_init(&processes_mutex, NULL);
    pthread_mutex_init(&runq_mutex, NULL);
#endif /* USE_LITHE */
  }

public:
  Process * lookup(const PID &pid)
  {
    if (!(pid.ip == ip && pid.port == port))
      return NULL;

    Process *process = NULL;

    acquire(processes);
    {
      map<uint32_t, Process *>::iterator it = processes.find(pid.pipe);
      if (it != processes.end()) {
	process = it->second;
      }
    }
    release(processes);

    return process;
  }

  void record(struct msg *msg)
  {
    assert(recording && !replaying);
    acquire(processes);
    {
      record_msgs.write((char *) msg, sizeof(struct msg) + msg->len);
      if (record_msgs.fail())
	fatalerror("failed to write to messages record");
    }
    release(processes);
  }

  void replay()
  {
    assert(!recording && replaying);
    acquire(processes);
    {
      if (!record_msgs.eof()) {
	struct msg *msg = (struct msg *) malloc(sizeof(struct msg));

	/* Read a message worth of data. */
	record_msgs.read((char *) msg, sizeof(struct msg));

	if (record_msgs.eof()) {
	  free(msg);
	  release(processes);
	  return;
	}

	if (record_msgs.fail())
	  fatalerror("failed to read from messages record");

	/* Read the body of the message if necessary. */
	if (msg->len != 0) {
	  struct msg *temp = msg;
	  msg = (struct msg *) malloc(sizeof(struct msg) + msg->len);
	  memcpy(msg, temp, sizeof(struct msg));
	  free(temp);
	  record_msgs.read((char *) msg + sizeof(struct msg), msg->len);
	  if (record_msgs.fail())
	    fatalerror("failed to read from messages record");
	}

	/* Add message to be delivered later. */
	(*replay_msgs)[msg->to.pipe].push(msg);
      }

      /* Deliver any messages to available processes. */
      foreachpair (uint32_t pipe, Process *process, processes) {
	queue<struct msg *> &msgs = (*replay_msgs)[pipe];
	while (!msgs.empty()) {
	  struct msg *msg = msgs.front();
	  msgs.pop();
	  process->enqueue(msg);
	}
      }
    }
    release(processes);
  }

#ifdef USE_LITHE
  static void do_run(lithe_task_t *task, void *arg)
  {
    Process *process = (Process *) task->tls;

    assert(process->state == Process::RUNNING);

    ProcessManager::instance()->cleanup(process);

    /* Go be productive doing something else ... */
    lithe_sched_reenter();
  }


  void run(Process *process)
  {
    assert(process != NULL);

    process->state = Process::RUNNING;

    try {
      (*process)();
    } catch (const std::exception &e) {
      cerr << "libprocess: " << process->pid
	   << " exited due to " << e.what() << endl;
    } catch (...) {
      cerr << "libprocess: " << process->pid
	   << " exited due to unknown exception" << endl;
    }

    lithe_task_block(do_run, NULL);
  }
#else
  void run(Process *process)
  {
    /*
     * N.B. The process gets locked before 'schedule' runs it (it gets
     * enqueued in 'trampoline'). So, after we can update the state
     * and then unlock.
    */
    {
      process->state = Process::RUNNING;
    }
    process->unlock();

    try {
      (*process)();
    } catch (const std::exception &e) {
      cerr << "libprocess: " << process->pid
	   << " exited due to "
	   << e.what() << endl;
    } catch (...) {
      cerr << "libprocess: " << process->pid
	   << " exited due to unknown exception" << endl;
    }

    cleanup(process);

    proc_process = NULL;
    setcontext(&proc_uctx_schedule);
  }
#endif /* USE_LITHE */


#ifdef USE_LITHE
  static void do_kill(lithe_task_t *task, void *arg)
  {
    Process *process = (Process *) task->tls;

    assert(process->state == Process::RUNNING);

    ProcessManager::instance()->cleanup(process);

    /* Go be productive doing something else ... */
    lithe_sched_reenter();
  }


  void kill(Process *process)
  {
    lithe_task_block(do_kill, NULL);
  }
#else
  void kill(Process *process)
  {
    cleanup(process);

    proc_process = NULL;
    setcontext(&proc_uctx_schedule);
  }
#endif /* USE_LITHE */

  
  void spawn(Process *process)
  {
    assert(process != NULL);

    process->state = Process::INIT;

    void *stack = NULL;

    acquire(processes);
    {
      /* Record process. */
      processes[process->pid.pipe] = process;

      /* Reuse a stack if possible. */
      if (!stacks->empty()) {
	stack = stacks->top();
	stacks->pop();
      }
    }
    release(processes);

    if (stack == NULL) {
      const int protection = (PROT_READ | PROT_WRITE);
      const int flags = (MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT);

      stack = mmap(NULL, PROCESS_STACK_SIZE, protection, flags, -1, 0);

      if (stack == MAP_FAILED)
	fatalerror("mmap failed (spawn)");

      /* Disallow all memory access to the last page. */
      if (mprotect(stack, getpagesize(), PROT_NONE) != 0)
	fatalerror("mprotect failed (spawn)");
    }

#ifdef USE_LITHE
    stack_t s;
    s.ss_sp = stack;
    s.ss_size = PROCESS_STACK_SIZE;

    lithe_task_init(&process->task, &s);

    process->task.tls = process;

    /* TODO(benh): Is there a better way to store the stack info? */
    process->uctx.uc_stack.ss_sp = stack;
    process->uctx.uc_stack.ss_size = PROCESS_STACK_SIZE;
#else
    /* Set up the ucontext. */
    if (getcontext(&process->uctx) < 0)
      fatalerror("getcontext failed (spawn)");
    
    process->uctx.uc_stack.ss_sp = stack;
    process->uctx.uc_stack.ss_size = PROCESS_STACK_SIZE;
    process->uctx.uc_link = 0;

    /* Package the arguments. */
#ifdef __x86_64__
    assert(sizeof(unsigned long) == sizeof(Process *));
    int process0 = (unsigned int) (unsigned long) process;
    int process1 = (unsigned long) process >> 32;
#else
    assert(sizeof(unsigned int) == sizeof(Process *));
    int process0 = (unsigned int) process;
    int process1 = 0;
#endif /* __x86_64__ */

    makecontext(&process->uctx, (void (*)()) trampoline, 2, process0, process1);
#endif /* USE_LITHE */

    /* Add process to the run queue. */
    enqueue(process);
  }


  void cleanup(Process *process)
  {
    //cout << "cleanup for " << process->pid << endl;

#ifdef USE_LITHE
    /* TODO(benh): Assert that we are on the transition stack. */
#endif /* USE_LITHE */

    /* Inform link manager. */
    LinkManager::instance()->exited(process);

    /* Processes that were waiting on exiting process. */
    list<Process *> resumable;

    /* Possible gate non-libprocess threads are waiting at. */
    Gate *gate = NULL;

    /* Remove process. */
    acquire(processes);
    {
      /* Remove from internal clock (if necessary). */
      acquire(timers);
      {
        if (clk != NULL)
          clk->discard(process);
      }
      release(timers);

      process->lock();
      {
	/* Free any pending messages. */
	while (!process->msgs.empty()) {
	  struct msg *msg = process->msgs.front();
	  process->msgs.pop_front();
	  free(msg);
	}

	/* Free current message. */
	if (process->current) free(process->current);

	/*
	 * TODO(benh): Can't recycle stacks unless we get off the
	 * stack by the time someone actually wants to use the stack.
	 *
	 * stacks->push(process->uctx.uc_stack.ss_sp);
	 */

	processes.erase(process->pid.pipe);

	/* TODO(benh): Confirm process not in timers. */

	/* Confirm process not in runq. */
	assert(find(runq.begin(), runq.end(), process) == runq.end());

	/* Confirm that the process is not in any waiting queue. */
	foreachpair (_, set<Process *> &waiting, waiters)
	  assert(waiting.find(process) == waiting.end());

	/* Grab all the waiting processes that are now resumable. */
	foreach (Process *waiter, waiters[process])
	  resumable.push_back(waiter);

	waiters.erase(process);

	/* Lookup gate to wake up waiting non-libprocess threads. */
	map<Process *, Gate *>::iterator it = gates.find(process);
	if (it != gates.end()) {
	  gate = it->second;
	  /* N.B. The last thread that leaves the gate also free's it. */
	  gates.erase(it);
	}

	process->state = Process::EXITED;
      }
      process->unlock();
    }
    release(processes);

    /*
     * N.B. After opening the gate we can no longer dereference
     * 'process' since it might already be cleaned up by user code (a
     * waiter might have cleaned up the stack where the process was
     * allocated).
     */
    if (gate != NULL)
      gate->open();

    foreach (Process *p, resumable) {
      p->lock();
      {
	// Process 'p' might be RUNNING because it is racing to become
	// WAITING while we are actually trying to get it to become
	// running again..
	assert(p->state == Process::RUNNING || p->state == Process::WAITING);
	if (p->state == Process::RUNNING) {
	  p->state = Process::INTERRUPTED;
	} else {
	  p->state = Process::READY;
	  enqueue(p);
	}
      }
      p->unlock();
    }
  }


  void link(Process *process, const PID &to)
  {
    /* Check if link is local. */
    if (to.ip == ip && to.port == port) {
      /* Make sure local process is still valid! */
      bool valid = false;

      acquire(processes);
      {
	if (processes.find(to.pipe) != processes.end())
	  valid = true;
      }
      release(processes);

      if (!valid) {
	struct msg *msg = (struct msg *) malloc(sizeof(struct msg));
	msg->from.pipe = to.pipe;
	msg->from.ip = to.ip;
	msg->from.port = to.port;
	msg->to.pipe = process->pid.pipe;
	msg->to.ip = process->pid.ip;
	msg->to.port = process->pid.port;
	msg->id = PROCESS_EXIT;
	msg->len = 0;
	process->enqueue(msg);
	return;
      }

      /* TODO(benh): Process object for 'to' could become invalid here! */

      LinkManager::instance()->link(process, to);

      /* Make sure local process is still valid! */
      valid = false;

      acquire(processes);
      {
	if (processes.find(to.pipe) != processes.end())
	  valid = true;
      }
      release(processes);

      /* TODO(benh): Better solution or send possible duplicate PROCESS_EXIT? */
      assert(valid);
    } else {
      LinkManager::instance()->link(process, to);
    }
  }


#ifdef USE_LITHE
  static void do_receive(lithe_task_t *task, void *arg)
  {
    timeout_t *timeout = (timeout_t *) arg;

    Process *process = (Process *) task->tls;

    process->lock();
    {
      /* Start timeout if necessary. */
      if (timeout != NULL)
	ProcessManager::instance()->start_timeout(*timeout);

      /* Context switch. */
      process->state = Process::RECEIVING;

      /* Ensure nothing enqueued since check in Process::receive. */
      if (!process->msgs.empty()) {
	process->state = Process::READY;
	ProcessManager::instance()->enqueue(process);
      }
    }
    process->unlock();

    /* N.B. We could resume the task if a message has arrived. *

    /* Go be productive doing something else ... */
    lithe_sched_reenter();
  }


  void receive(Process *process, double secs)
  {
    assert(process != NULL);
    if (secs > 0) {
      timeout_t timeout = create_timeout(process, secs);
      assert(sizeof(timeout_t *) == sizeof(void *));
      lithe_task_block(do_receive, &timeout);
      process->lock();
      {
	assert(process->state == Process::READY ||
	       process->state == Process::TIMEDOUT);

	/*
	 * Attempt to cancel the timeout if necessary.
	 * N.B. Failed cancel means unnecessary timeouts (hence generation).
	 */
	if (process->state != Process::TIMEDOUT)
	  cancel_timeout(timeout);

	/* Update the generation (handles racing timeouts). */
	process->generation++;

	process->state = Process::RUNNING;
      }
      process->unlock();
    } else {
      lithe_task_block(do_receive, NULL);
      process->lock();
      {
	assert(process->state == Process::READY);
	process->state = Process::RUNNING;
      }
      process->unlock();
    }
  }
#else
  void receive(Process *process, double secs)
  {
    //cout << "ProcessManager::receive" << endl;
    assert(process != NULL);
    process->lock();
    {
      /* Ensure nothing enqueued since check in Process::receive. */
      if (process->msgs.empty()) {
	if (secs > 0) {
	  /* Create timeout. */
	  const timeout_t &timeout = create_timeout(process, secs);

	  /* Start the timeout. */
	  start_timeout(timeout);

	  /* Context switch. */
	  process->state = Process::RECEIVING;
	  swapcontext(&process->uctx, &proc_uctx_running);

	  assert(process->state == Process::READY ||
		 process->state == Process::TIMEDOUT);

	  /* Attempt to cancel the timer if necessary. */
	  if (process->state != Process::TIMEDOUT)
	    cancel_timeout(timeout);

	  /* N.B. No cancel means possible unnecessary timeouts. */

	  process->state = Process::RUNNING;
      
	  /* Update the generation (handles racing timeouts). */
	  process->generation++;
	} else {
	  /* Context switch. */
	  process->state = Process::RECEIVING;
	  swapcontext(&process->uctx, &proc_uctx_running);
	  assert(process->state == Process::READY);
	  process->state = Process::RUNNING;
	}
      }
    }
    process->unlock();
  }
#endif /* USE_LITHE */


#ifdef USE_LITHE
  static void do_pause(lithe_task_t *task, void *arg)
  {
    timeout_t *timeout = (timeout_t *) arg;

    Process *process = (Process *) task->tls;

    process->lock();
    {
      /* Start timeout. */
      ProcessManager::instance()->start_timeout(*timeout);

      /* Context switch. */
      process->state = Process::PAUSED;
    }
    process->unlock();

    /* Go be productive doing something else ... */
    lithe_sched_reenter();
  }


  void pause(Process *process, double secs)
  {
    assert(process != NULL);

    if (secs > 0) {
      timeout_t timeout = create_timeout(process, secs);
      assert(sizeof(timeout_t *) == sizeof(void *));
      lithe_task_block(do_pause, &timeout);
      assert(process->state == Process::TIMEDOUT);
      process->state = Process::RUNNING;
    }
  }
#else
  void pause(Process *process, double secs)
  {
    assert(process != NULL);

    process->lock();
    {
      if (secs > 0) {
	/* Create/Start the timeout. */
	start_timeout(create_timeout(process, secs));

	/* Context switch. */
	process->state = Process::PAUSED;
	swapcontext(&process->uctx, &proc_uctx_running);
	assert(process->state == Process::TIMEDOUT);
	process->state = Process::RUNNING;
      } else {
	/* Modified context switch (basically a yield). */
	process->state = Process::READY;
	enqueue(process);
	swapcontext(&process->uctx, &proc_uctx_running);
	assert(process->state == Process::READY);
	process->state = Process::RUNNING;
      }
    }
    process->unlock();
  }
#endif /* USE_LITHE */


#ifdef USE_LITHE
  static void do_wait(lithe_task_t *task, void *arg)
  {
    Process *process = (Process *) task->tls;

    bool resume = false;

    process->lock();
    {
      if (process->state == Process::RUNNING) {
	/* Context switch. */
	process->state = Process::WAITING;
      } else {
	assert(process->state == Process::INTERRUPTED);
	process->state = Process::READY;
	resume = true;
      }
    }
    process->unlock();

    if (resume)
      lithe_task_resume(task);

    /* Go be productive doing something else ... */
    lithe_sched_reenter();
  }


  bool wait(PID pid)
  {
    /* TODO(benh): Account for a non-libprocess task/ctx. */
    assert(false);

    Process *process;

    if (lithe_task_gettls((void **) &process) < 0)
      abort();

    bool waited = false;

    /* Now we can add the process to the waiters. */
    acquire(processes);
    {
      map<uint32_t, Process *>::iterator it = processes.find(pid.pipe);
      if (it != processes.end()) {
	assert(it->second->state != Process::EXITED);
	waiters[it->second].insert(process);
	waited = true;
      }
    }
    release(processes);

    /* If we waited then we should context switch. */
    if (waited) {
      lithe_task_block(do_wait, NULL);
      assert(process->state == Process::READY);
      process->state = Process::RUNNING;
    }

    return waited;
  }
#else
  bool wait(PID pid)
  {
    if (pthread_self() != proc_thread)
      return external_wait(pid);

    Process *process = proc_process;

    bool waited = false;

    /* Now we can add the process to the waiters. */
    acquire(processes);
    {
      map<uint32_t, Process *>::iterator it = processes.find(pid.pipe);
      if (it != processes.end()) {
	assert(it->second->state != Process::EXITED);
	waiters[it->second].insert(process);
	waited = true;
      }
    }
    release(processes);

    /* If we waited then we should context switch. */
    if (waited) {
      process->lock();
      {
	if (process->state == Process::RUNNING) {
	  /* Context switch. */
	  process->state = Process::WAITING;
	  swapcontext(&process->uctx, &proc_uctx_running);
	  assert(process->state == Process::READY);
	  process->state = Process::RUNNING;
	} else {
	  /* Process is cleaned up and we have been removed from waiters. */
	  assert(process->state == Process::INTERRUPTED);
	  process->state = Process::RUNNING;
	}
      }
      process->unlock();
    }

    return waited;
  }
#endif /* USE_LITHE */


  bool external_wait(PID pid)
  {
    // We use a gate for external waiters. A gate is single use. That
    // is, a new gate is created when the first external thread shows
    // up and wants to wait for a process that currently has no
    // gate. Once that process exits, the last external thread to
    // leave the gate will also clean it up. Note that a gate will
    // never get more external threads waiting on it after it has been
    // opened, since the process should no longer be valid and
    // therefore will not have an entry in 'processes'.

    Gate *gate = NULL;
    Gate::state_t old;

    /* Try and approach the gate if necessary. */
    acquire(processes);
    {
      map<uint32_t, Process *>::iterator it = processes.find(pid.pipe);
      if (it != processes.end()) {
	assert(it->second->state != Process::EXITED);
	Process *process = it->second;
	/* Check and see if a gate already exists. */
	if (gates.find(process) == gates.end())
	  gates[process] = new Gate();
	gate = gates[process];
	old = gate->approach();
      }
    }
    release(processes);

    /* Now arrive at the gate and wait until it opens. */
    if (gate != NULL) {
      gate->arrive(old);
      if (gate->empty())
	delete gate;
      return true;
    }

    return false;
  }


#ifdef USE_LITHE
  static void do_await(lithe_task_t *task, void *arg)
  {
    ev_io *io_watcher = (ev_io *) arg;

    Process *process = (Process *) task->tls;

    process->lock();
    {
      /* Enqueue the watcher. */
      acquire(io_watchersq);
      {
	io_watchersq->push(io_watcher);
      }
      release(io_watchersq);

      /* Interrupt the loop. */
      ev_async_send(loop, &async_watcher);

      /* Context switch. */
      process->state = Process::AWAITING;

      /*
       * N.B. It is difficult to check if a new message has arrived
       * since the await call was issued because the queue might not
       * have been empty. One could imagine passing along the
       * information in a subsequent version.
       */
    }
    process->unlock();

    /* Go be productive doing something else ... */
    lithe_sched_reenter();
  }


  bool await(Process *process, int fd, int op)
  {
    assert(process != NULL);

    bool interrupted = false;

    if (fd < 0)
      return false;

    /* Allocate/Initialize the watcher. */
    ev_io *io_watcher = (ev_io *) malloc(sizeof(ev_io));

    if ((op & Process::RDWR) == Process::RDWR)
      ev_io_init(io_watcher, handle_await, fd, EV_READ | EV_WRITE);
    else if ((op & Process::RDONLY) == Process::RDONLY)
      ev_io_init(io_watcher, handle_await, fd, EV_READ);
    else if ((op & Process::WRONLY) == Process::WRONLY)
      ev_io_init(io_watcher, handle_await, fd, EV_WRITE);

    /* Create tuple describing state (on heap in case we get interrupted). */
    io_watcher->data = new tuple<Process *, int>(process, process->generation);

    assert(sizeof(ev_io *) == sizeof(void *));
    lithe_task_block(do_await, io_watcher);

    process->lock();
    {
      assert(process->state == Process::READY ||
	     process->state == Process::INTERRUPTED);

      /* Update the generation (handles racing awaited). */
      process->generation++;

      if (process->state == Process::INTERRUPTED)
	interrupted = true;

      process->state = Process::RUNNING;
    }
    process->unlock();

    return !interrupted;
  }
#else
  bool await(Process *process, int fd, int op, double secs, bool ignore)
  {
    assert(process != NULL);

    bool interrupted = false;

    process->lock();
    {
      /* Consider a non-empty message queue as an immediate interrupt. */
      if (!ignore && !process->msgs.empty()) {
	process->unlock();
	return false;
      }

      assert(secs > 0);

      /* Create timeout. */
      const timeout_t &timeout = create_timeout(process, secs);

      /* Start the timeout. */
      start_timeout(timeout);

      // Treat an await with a bad fd as an interruptible pause!
      if (fd >= 0) {
	/* Allocate/Initialize the watcher. */
	ev_io *io_watcher = (ev_io *) malloc(sizeof(ev_io));

	if ((op & Process::RDWR) == Process::RDWR)
	  ev_io_init(io_watcher, handle_await, fd, EV_READ | EV_WRITE);
	else if ((op & Process::RDONLY) == Process::RDONLY)
	  ev_io_init(io_watcher, handle_await, fd, EV_READ);
	else if ((op & Process::WRONLY) == Process::WRONLY)
	  ev_io_init(io_watcher, handle_await, fd, EV_WRITE);

	/* Tuple describing state (on heap in case we get interrupted). */
	io_watcher->data =
	  new tuple<Process *, int>(process, process->generation);

	/* Enqueue the watcher. */
	acquire(io_watchersq);
	{
	  io_watchersq->push(io_watcher);
	}
	release(io_watchersq);
    
	/* Interrupt the loop. */
	ev_async_send(loop, &async_watcher);
      }

      /* Context switch. */
      process->state = Process::AWAITING;
      swapcontext(&process->uctx, &proc_uctx_running);
      assert(process->state == Process::READY ||
	     process->state == Process::TIMEDOUT ||
	     process->state == Process::INTERRUPTED);

      /* Attempt to cancel the timer if necessary. */
      if (process->state != Process::TIMEDOUT)
	cancel_timeout(timeout);

      if (process->state == Process::INTERRUPTED)
	interrupted = true;

      process->state = Process::RUNNING;
      
      /* Update the generation (handles racing awaited). */
      process->generation++;
    }
    process->unlock();

    return !interrupted;
  }
#endif /* USE_LITHE */


  void awaited(Process *process, int generation)
  {
    process->lock();
    {
      if (process->state == Process::AWAITING &&
	  process->generation == generation) {
	process->state = Process::READY;
	enqueue(process);
      }
    }
    process->unlock();
  }


  void timedout(Process *process, int generation)
  {
    assert(process != NULL);

    /* Make sure the process is still valid! */
    bool valid = false;

    acquire(processes);
    {
      map<uint32_t, Process *>::iterator it = processes.find(process->pid.pipe);
      if (it != processes.end() && it->second == process)
	valid = true;
    }
    release(processes);

    if (!valid)
      return;

    /* TODO(benh): Process could become invalid here! Reference counting? */

    process->lock();
    {
      /* N.B. State != READY after timeout, but generation still same. */
      if (process->state != Process::READY &&
	  process->generation == generation) {
	/* N.B. Process may be RUNNING due to "outside" thread 'receive'. */
	assert(process->state == Process::RUNNING ||
	       process->state == Process::RECEIVING ||
	       process->state == Process::AWAITING ||
	       process->state == Process::INTERRUPTED ||
	       process->state == Process::PAUSED);
	if (process->state != Process::RUNNING ||
	    process->state != Process::INTERRUPTED)
	  ProcessManager::instance()->enqueue(process);
	process->state = Process::TIMEDOUT;
      }
    }
    process->unlock();

    /* TODO(benh): Check if process became invalid! */
    valid = false;
    acquire(processes);
    {
      map<uint32_t, Process *>::iterator it = processes.find(process->pid.pipe);
      if (it != processes.end() && it->second == process)
	valid = true;
    }
    release(processes);

    /* If process is invalid, we probably just wrote over some memory ... */
    assert(valid);
  }


  timeout_t create_timeout(Process *process, double secs)
  {
    assert(process != NULL);

    ev_tstamp tstamp;

    acquire(timers);
    {
      if (clk != NULL) {
        tstamp = clk->getCurrent(process) + secs;
      } else {
	// TODO(benh): Unclear if want ev_now(...) or ev_time().
	tstamp = ev_time() + secs;
      }
    }
    release(timers);

    return make_tuple(tstamp, process, process->generation);
  }


  void start_timeout(const timeout_t &timeout)
  {
    ev_tstamp tstamp = timeout.get<0>();

    /* Add the timer. */
    acquire(timers);
    {
      if (timers->size() == 0 || tstamp < timers->begin()->first) {
	// Need to interrupt the loop to update/set timer repeat.
	(*timers)[tstamp].push_back(timeout);
	update_timer = true;
	ev_async_send(loop, &async_watcher);
      } else {
	// Timer repeat is adequate, just add the timeout.
	assert(timers->size() >= 1);
	(*timers)[tstamp].push_back(timeout);
      }
    }
    release(timers);
  }


  bool cancel_timeout(const timeout_t &timeout)
  {
    bool cancelled = false;

    acquire(timers);
    {
      /* Check if the timer has fired (this is highly unoptimized). */
      foreachpair (const ev_tstamp &tstamp, timeouts_t &timeouts, *timers) {
	list<timeout_t>::iterator it = timeouts.begin();
	while (it != timeouts.end()) {
	  if (it->get<0>() == timeout.get<0>() &&
	      it->get<1>() == timeout.get<1>() &&
	      it->get<2>() == timeout.get<2>()) {
	    timeouts.erase(it++);
	    cancelled = true;
	    break;
	  } else {
	    ++it;
	  }
	}
	if (cancelled) {
          if (timeouts.size() == 0)
            timers->erase(tstamp);
	  break;
        }
      }
    }
    release(timers);

    return cancelled;
  }


  void enqueue(Process *process)
  {
    assert(process != NULL);
    acquire(runq);
    {
      assert(find(runq.begin(), runq.end(), process) == runq.end());
      runq.push_back(process);
    }
    release(runq);
    
    /* Wake up the processing thread if necessary. */
    gate->open();
  }

  Process * dequeue()
  {
    Process *process = NULL;

    acquire(runq);
    {
      if (!runq.empty()) {
	process = runq.front();
	runq.pop_front();
      }
    }
    release(runq);

    return process;
  }

  void deliver(struct msg *msg, Process *sender = NULL)
  {
    assert(msg != NULL);
    assert(!replaying);
//     cout << endl;
//     cout << "msg->from.pipe: " << msg->from.pipe << endl;
//     cout << "msg->from.ip: " << msg->from.ip << endl;
//     cout << "msg->from.port: " << msg->from.port << endl;
//     cout << "msg->to.pipe: " << msg->to.pipe << endl;
//     cout << "msg->to.ip: " << msg->to.ip << endl;
//     cout << "msg->to.port: " << msg->to.port << endl;
//     cout << "msg->id: " << msg->id << endl;
//     cout << "msg->len: " << msg->len << endl;

    synchronized(processes) {
      Process *receiver = NULL;

      if (processes.count(msg->to.pipe) != 0)
        receiver = processes[msg->to.pipe];

      if (receiver != NULL) {
        // If we have a local sender AND we are using a manual clock
        // then update the current time of the receiver to preserve
        // the happens-before relationship between the sender and
        // receiver. Note that the assumption is that the sender
        // remains valid for at least the duration of this routine (so
        // that we can look up it's current time).
        if (sender != NULL) {
          synchronized(timers) {
            if (clk != NULL) {
              clk->setCurrent(receiver, max(clk->getCurrent(receiver),
                                            clk->getCurrent(sender)));
            }
          }
        }

        receiver->enqueue(msg);
      } else {
	free(msg);
      }
    }
  }
};

/* Singleton ProcessManager instance. */
template<> ProcessManager * Singleton<ProcessManager>::singleton = NULL;
template<> bool Singleton<ProcessManager>::instantiated = false;


static void handle_async(struct ev_loop *loop, ev_async *w, int revents)
{
  acquire(io_watchersq);
  {
    /* Start all the new I/O watchers. */
    while (!io_watchersq->empty()) {
      ev_io *io_watcher = io_watchersq->front();
      io_watchersq->pop();
      ev_io_start(loop, io_watcher);
    }
  }
  release(io_watchersq);

  acquire(timers);
  {
    if (update_timer) {
      if (!timers->empty()) {
	// Determine the current time.
	ev_tstamp current_tstamp;
	if (clk != NULL) {
	  current_tstamp = clk->getCurrent();
	} else {
	  // TODO(benh): Unclear if want ev_now(...) or ev_time().
	  current_tstamp = ev_time();
	}

	timer_watcher.repeat = timers->begin()->first - current_tstamp;

	// Check when the timer event should fire.
        if (timer_watcher.repeat <= 0) {
	  // Feed the event now!
	  timer_watcher.repeat = 0;
	  ev_timer_again(loop, &timer_watcher);
          ev_feed_event(loop, &timer_watcher, EV_TIMEOUT);
        } else {
	  // Only repeat the timer if not using a manual clock (a call
	  // to ProcessClock::advance() will force a timer event later).
	  if (clk != NULL && timer_watcher.repeat > 0)
	    timer_watcher.repeat = 0;
	  ev_timer_again(loop, &timer_watcher);
	}
      }

      update_timer = false;
    }
  }
  release(timers);
}


void handle_await(struct ev_loop *loop, ev_io *w, int revents)
{
  tuple<Process *, int> *t = (tuple<Process *, int> *) w->data;

  ProcessManager::instance()->awaited(t->get<0>(), t->get<1>());

  ev_io_stop(loop, w);

  delete t;

  free(w);
}


void read_data(struct ev_loop *loop, ev_io *w, int revents)
{
  int c = w->fd;
  //cout << "read_data on " << c << " started" << endl;

  struct read_ctx *ctx = (struct read_ctx *) w->data;

  /* Read the data starting from the last read. */
  int len = recv(c,
		 (char *) ctx->msg + sizeof(struct msg) + ctx->len,
		 ctx->msg->len - ctx->len,
		 0);

  if (len > 0) {
    ctx->len += len;
  } else if (len < 0 && errno == EWOULDBLOCK) {
    return;
  } else if (len == 0 || (len < 0 &&
			  (errno == ECONNRESET ||
			   errno == EBADF ||
			   errno == EHOSTUNREACH))) {
    /* Socket has closed. */
    //perror("libprocess recv error: ");
    //cout << "read_data: closing socket " << c << endl;
    LinkManager::instance()->closed(c);

    /* Stop receiving ... */
    ev_io_stop (loop, w);
    close(c);
    free(ctx->msg);
    free(ctx);
    free(w);
    return;
  } else {
    fatalerror("unhandled socket error: please report (read_data)");
  }

  if (ctx->len == ctx->msg->len) {
    /* Deliver message. */
    ProcessManager::instance()->deliver(ctx->msg);

    /* Reinitialize read context. */
    ctx->len = 0;
    ctx->msg = (struct msg *) malloc(sizeof(struct msg));

    //cout << "read_data on " << c << " finished" << endl;

    /* Continue receiving ... */
    ev_io_stop (loop, w);
    ev_io_init (w, read_msg, c, EV_READ);
    ev_io_start (loop, w);
  }
}


void read_msg(struct ev_loop *loop, ev_io *w, int revents)
{
  int c = w->fd;
  //cout << "read_msg on " << c << " started" << endl;

  struct read_ctx *ctx = (struct read_ctx *) w->data;

  /* Read the message starting from the last read. */
  int len = recv(c,
		 (char *) ctx->msg + ctx->len,
		 sizeof (struct msg) - ctx->len,
		 0);

  if (len > 0) {
    ctx->len += len;
  } else if (len < 0 && errno == EWOULDBLOCK) {
    return;
  } else if (len == 0 || (len < 0 &&
			  (errno == ECONNRESET ||
			   errno == EBADF ||
			   errno == EHOSTUNREACH))) {
    /* Socket has closed. */
    //perror("libprocess recv error: ");
    //cout << "read_msg: closing socket " << c << endl;
    LinkManager::instance()->closed(c);

    /* Stop receiving ... */
    ev_io_stop (loop, w);
    close(c);
    free(ctx->msg);
    free(ctx);
    free(w);
    return;
  } else {
    fatalerror("unhandled socket error: please report (read_msg)");
  }

  if (ctx->len == sizeof(struct msg)) {
    /* Check and see if we need to receive data. */
    if (ctx->msg->len > 0) {
      /* Allocate enough space for data. */
      ctx->msg = (struct msg *)
	realloc (ctx->msg, sizeof(struct msg) + ctx->msg->len);

      /* TODO(benh): Optimize ... try doing a read first! */
      ctx->len = 0;

      /* Start receiving data ... */
      ev_io_stop (loop, w);
      ev_io_init (w, read_data, c, EV_READ);
      ev_io_start (loop, w);
    } else {
      /* Deliver message. */
      //cout << "delivering message" << endl;
      ProcessManager::instance()->deliver(ctx->msg);

      /* Reinitialize read context. */
      ctx->len = 0;
      ctx->msg = (struct msg *) malloc(sizeof(struct msg));

      /* Continue receiving ... */
      ev_io_stop (loop, w);
      ev_io_init (w, read_msg, c, EV_READ);
      ev_io_start (loop, w);
    }
  }
}


static void write_data(struct ev_loop *loop, ev_io *w, int revents)
{
  int c = w->fd;

  //cout << "write_data on " << c << " started" << endl;

  struct write_ctx *ctx = (struct write_ctx *) w->data;

  int len = send(c,
		 (char *) ctx->msg + sizeof(struct msg) + ctx->len,
		 ctx->msg->len - ctx->len,
		 MSG_NOSIGNAL);

  if (len > 0) {
    ctx->len += len;
  } else if (len < 0 && errno == EWOULDBLOCK) {
    return;
  } else if (len == 0 || (len < 0 &&
			  (errno == ECONNRESET ||
			   errno == EBADF ||
			   errno == EHOSTUNREACH ||
			   errno == EPIPE))) {
    /* Socket has closed. */
    //perror("libprocess send error: ");
    //cout << "write_data: closing socket " << c << endl;
    LinkManager::instance()->closed(c);

    /* Stop receiving ... */
    ev_io_stop (loop, w);
    close(c);
    free(ctx->msg);
    free(ctx);
    free(w);
    return;
  } else {
    fatalerror("unhandled socket error: please report (write_data)");
  }

  if (ctx->len == ctx->msg->len) {
    ev_io_stop (loop, w);
    free(ctx->msg);

    if (ctx->close)
      ctx->msg = LinkManager::instance()->next_or_close(c);
    else
      ctx->msg = LinkManager::instance()->next_or_sleep(c);

    if (ctx->msg != NULL) {
      ctx->len = 0;
      ev_io_init(w, write_msg, c, EV_WRITE);
      ev_io_start(loop, w);
    } else {
      //cout << "write_data on " << c << " finished" << endl;
      free(ctx);
      free(w);
    }
  }
}


void write_msg(struct ev_loop *loop, ev_io *w, int revents)
{
  int c = w->fd;
  //cout << "write_msg on " << c << " started" << endl;

  struct write_ctx *ctx = (struct write_ctx *) w->data;

  int len = send(c,
		 (char *) ctx->msg + ctx->len,
		 sizeof (struct msg) - ctx->len,
		 MSG_NOSIGNAL);

  if (len > 0) {
    ctx->len += len;
  } else if (len < 0 && errno == EWOULDBLOCK) {
    return;
  } else if (len == 0 || (len < 0 &&
			  (errno == ECONNRESET ||
			   errno == EBADF ||
			   errno == EHOSTUNREACH ||
			   errno == EPIPE))) {
    /* Socket has closed. */
    //perror("libprocess send error: ");
    //cout << "write_msg: closing socket " << c << endl;
    LinkManager::instance()->closed(c);

    /* Stop receiving ... */
    ev_io_stop (loop, w);
    close(c);
    free(ctx->msg);
    free(ctx);
    free(w);
    return;
  } else {
    fatalerror("unhandled socket error: please report (write_msg)");
  }

  if (ctx->len == sizeof(struct msg)) {
    /* Check and see if we need to write data. */
    if (ctx->msg->len > 0) {
      
      /* TODO(benh): Optimize ... try doing a write first! */
      ctx->len = 0;

      /* Start writing data ... */
      ev_io_stop(loop, w);
      ev_io_init(w, write_data, c, EV_WRITE);
      ev_io_start(loop, w);
    } else {
      //cout << "write_msg: closing socket" << endl;
      ev_io_stop(loop, w);
      free(ctx->msg);

      if (ctx->close)
	ctx->msg = LinkManager::instance()->next_or_close(c);
      else
	ctx->msg = LinkManager::instance()->next_or_sleep(c);

      if (ctx->msg != NULL) {
	ctx->len = 0;
	ev_io_init(w, write_msg, c, EV_WRITE);
	ev_io_start(loop, w);
      } else {
	//cout << "write_msg on " << c << " finished" << endl;
	free(ctx);
	free(w);
      }
    }
  }
}


void write_connect(struct ev_loop *loop, ev_io *w, int revents)
{
  //cout << "write_connect" << endl;
  int s = w->fd;

  struct write_ctx *ctx = (struct write_ctx *) w->data;

  ev_io_stop(loop, w);

  /* Check that the connection was successful. */
  int opt;
  socklen_t optlen = sizeof(opt);

  if (getsockopt(s, SOL_SOCKET, SO_ERROR, &opt, &optlen) < 0) {
    //cerr << "failed to connect (getsockopt)" << endl;
    LinkManager::instance()->closed(s);
    free(ctx->msg);
    free(ctx);
    free(w);
    return;
  }

  if (opt != 0) {
    //cerr << "failed to connect" << endl;
    LinkManager::instance()->closed(s);
    free(ctx->msg);
    free(ctx);
    free(w);
    return;
  }

  /* TODO(benh): Optimize ... try doing a write first. */

  ev_io_init(w, write_msg, s, EV_WRITE);
  ev_io_start(loop, w);
}



void link_connect(struct ev_loop *loop, ev_io *w, int revents)
{
  //cout << "link_connect" << endl;
  int s = w->fd;

  ev_io_stop(loop, w);

  /* Check that the connection was successful. */
  int opt;
  socklen_t optlen = sizeof(opt);

  if (getsockopt(s, SOL_SOCKET, SO_ERROR, &opt, &optlen) < 0) {
    //cerr << "failed to connect (getsockopt)" << endl;
    LinkManager::instance()->closed(s);
    free(w);
    return;
  }

  if (opt != 0) {
    //cerr << "failed to connect" << endl;
    LinkManager::instance()->closed(s);
    free(w);
    return;
  }

  /* Reuse/Initialize the watcher. */
  w->data = malloc(sizeof(struct read_ctx));

  /* Initialize read context. */
  struct read_ctx *ctx = (struct read_ctx *) w->data;

  ctx->len = 0;
  ctx->msg = (struct msg *) malloc(sizeof(struct msg));

  /* Initialize watcher for reading. */
  ev_io_init(w, read_msg, s, EV_READ);

  ev_io_start(loop, w);
}


void handle_timeout(struct ev_loop *loop, ev_timer *w, int revents)
{
  list<timeout_t> timedout;

  acquire(timers);
  {
    ev_tstamp current_tstamp;

    if (clk != NULL) {
      current_tstamp = clk->getCurrent();
    } else {
      // TODO(benh): Unclear if want ev_now(...) or ev_time().
      current_tstamp = ev_time();
    }

    map<ev_tstamp, list<timeout_t> >::iterator it = timers->begin();
    map<ev_tstamp, list<timeout_t> >::iterator last = timers->begin();

    for (; it != timers->end(); ++it) {
      // Check if timer has expired.
      ev_tstamp tstamp = it->first;
      if (tstamp > current_tstamp) {
	last = it;
	break;
      }

      // Save expired timeouts and determine the amount of time that
      // has been simulated if using the manual clock.
      const list<timeout_t> &timeouts = it->second;

      foreach (const timeout_t &timeout, timeouts) {
	if (clk != NULL) {
	  ev_tstamp current = timeout.get<0>();
	  Process *process = timeout.get<1>();
          // Current time may be greater than timeout if a local
          // message is received (and happens-before kicks in).
          clk->setCurrent(process, max(clk->getCurrent(process), current));
	}
	// TODO(benh): Ensure deterministic order for testing?
	timedout.push_back(timeout);
      }
    }

    if (it == timers->end())
      timers->clear();
    else if (last != timers->begin())
      timers->erase(timers->begin(), last);

    assert(timers->empty() || (timers->begin()->first > current_tstamp));

    // TODO(benh): Make this code look like the code in handle_async.

    if (!timers->empty() && clk == NULL) {
      timer_watcher.repeat = timers->begin()->first - current_tstamp;
      assert(timer_watcher.repeat > 0);
      ev_timer_again(loop, &timer_watcher);
    } else {
      timer_watcher.repeat = 0;
      ev_timer_again(loop, &timer_watcher);
    }

    update_timer = false;
  }
  release(timers);

  foreach (const timeout_t &timeout, timedout)
    ProcessManager::instance()->timedout(timeout.get<1>(), timeout.get<2>());
}


void handle_accept(struct ev_loop *loop, ev_io *w, int revents)
{
  //cout << "handle_accept" << endl;
  int s = w->fd;

  struct sockaddr_in addr;

  socklen_t addrlen = sizeof(addr);

  /* Do accept. */
  int c = accept(s, (struct sockaddr *) &addr, &addrlen);

  if (c < 0) {
    return;
  }

  /* Make socket non-blocking. */
  if (set_nbio(c) < 0) {
    close(c);
    return;
  }

  /* Turn off Nagle (on TCP_NODELAY) so pipelined requests don't wait. */
  int on = 1;
  if (setsockopt(c, SOL_TCP, TCP_NODELAY, &on, sizeof(on)) < 0) {
    close(c);
    return;
  }

  /* Allocate the watcher. */
  ev_io *io_watcher = (ev_io *) malloc (sizeof (ev_io));

  io_watcher->data = malloc(sizeof(struct read_ctx));

  /* Initialize the read context */
  struct read_ctx *ctx = (struct read_ctx *) io_watcher->data;

  ctx->len = 0;
  ctx->msg = (struct msg *) malloc(sizeof(struct msg));

  /* Initialize watcher for reading. */
  ev_io_init(io_watcher, read_msg, c, EV_READ);

  ev_io_start(loop, io_watcher);
}


static void * node(void *arg)
{
  ev_loop(((struct ev_loop *) arg), 0);

  return NULL;
}


#ifdef USE_LITHE

void ProcessScheduler::enter()
{
  if (waiter == -1)
    __sync_bool_compare_and_swap(&waiter, -1, ht_id());

  if (waiter == ht_id())
    schedule();

  lithe_sched_t *sched = NULL;

  spinlock_lock(&lock);
  {
    int lowest = INT_MAX;
    typedef pair<int, int> counts_t;
    foreachpair (lithe_sched_t *child, counts_t &counts, children) {
      /* Don't bother giving a hart unless it has been requested. */
      if (counts.first < counts.second) {
	/* Give to the most neglected child. */
	if (counts.first < lowest) {
	  sched = child;
	  lowest = counts.first;
	}
      }
    }
    if (sched != NULL)
      children[sched].first++;
  }
  spinlock_unlock(&lock);

  if (sched != NULL)
    lithe_sched_enter(sched);

  lithe_sched_yield();
}


void ProcessScheduler::yield(lithe_sched_t *child)
{
  enter();
}


void ProcessScheduler::reg(lithe_sched_t *child)
{
  spinlock_lock(&lock);
  {
    children[child] = make_pair(0,0);
  }
  spinlock_unlock(&lock);
}


void ProcessScheduler::unreg(lithe_sched_t *child)
{
  int count;
  spinlock_lock(&lock);
  {
    count = children[child].first;
    children.erase(child);
  }
  spinlock_unlock(&lock);

//   cout << "child had a count of: " << count << " + 1 = " << count + 1 << endl;
}


void ProcessScheduler::request(lithe_sched_t *child, int k)
{
  spinlock_lock(&lock);
  {
    children[child] = make_pair(0, k);
  }
  spinlock_unlock(&lock);
  lithe_sched_request(k);
}


void ProcessScheduler::unblock(lithe_task_t *task)
{
  assert(false);
}


void ProcessScheduler::schedule()
{
  do {
    Process *process = ProcessManager::instance()->dequeue();

    if (process == NULL) {
      Gate::state_t old = gate->approach();
      process = ProcessManager::instance()->dequeue();
      if (process == NULL) {
	/* Wait at gate if idle. */
	gate->arrive(old);
	continue;
      } else {
	/* Leave gate since we dequeued a process. */
	gate->leave();
      }
    }

    assert(process->state == Process::INIT ||
	   process->state == Process::READY ||
	   process->state == Process::INTERRUPTED ||
	   process->state == Process::TIMEDOUT);

    assert(waiter == ht_id());
    waiter = -1;

    lithe_sched_request(1);

//     cout << ht_id() << " running " << process->getPID() <<  endl;

    /* Start/Continue process. */
    if (process->state == Process::INIT)
      lithe_task_do(&process->task, trampoline, process);
    else
      lithe_task_resume(&process->task);
  } while (true);
}



/*
* N.B. For now, we can only support one ProcessScheduler at a
* time. This is a deficiency of the singleton design for
* ProcessManager and LinkManager, and should be addressed in the
* future.
*/
static bool running = false;


ProcessScheduler::ProcessScheduler()
{
  if (!__sync_bool_compare_and_swap(&running, false, true))
    fatalerror("only one process scheduler can be running");

  /* Setup scheduler. */
  spinlock_init(&lock);

  waiter = -1;

  if (lithe_sched_register_task(&funcs, this, &task) != 0)
    abort();

  /* Request a processing thread. */
  if (lithe_sched_request(1) < 0)
    abort();
}


ProcessScheduler::~ProcessScheduler()
{
  lithe_task_t *task;
  lithe_sched_unregister_task(&task);
  if (task != &this->task)
    abort();

  if (!__sync_bool_compare_and_swap(&running, true, false))
    abort();
}

#else

void * schedule(void *arg)
{
  if (getcontext(&proc_uctx_schedule) < 0)
    fatalerror("getcontext failed (schedule)");

  do {
    if (replaying)
      ProcessManager::instance()->replay();

    Process *process = ProcessManager::instance()->dequeue();

    if (process == NULL) {
      Gate::state_t old = gate->approach();
      process = ProcessManager::instance()->dequeue();
      if (process == NULL) {

        // When using the manual clock, we want to let all the
        // processes "run" up to the current time so that processes
        // receive messages in order. If we let one process have a
        // drastically advanced current time then it may try send
        // messages to another process that, due to the happens-before
        // relationship, will inherit it's drastically advanced
        // current time. If the processing thread gets to this point
        // (i.e., the point where no other processes are runnable)
        // with the manual clock means that all of the processes have
        // been run which could be run up to the current time. The
        // only way another process could become runnable is if (1) it
        // receives a message from another node, (2) a file descriptor
        // it is awaiting has become ready, or (3) if it has a
        // timeout. We can ignore processes that become runnable due
        // to receiving a message from another node or getting an
        // event on a file descriptor because that should not change
        // the timing happens-before relationship of the local
        // processes (unless of course the file descriptor was created
        // from something like timerfd, in which case, since the
        // programmer is not using the timing source provided in
        // libprocess and all bets are off). Thus, we can check that
        // there are no pending timeouts before the current time and
        // move the current time to the next timeout value, and tell
        // the timer to update itself.

        synchronized(timers) {
          if (clk != NULL) {
            if (!timers->empty()) {
              // Adjust the current time to the next timeout, provided
              // it is not past the elapsed time.
              ev_tstamp tstamp = timers->begin()->first;
              if (tstamp <= clk->getElapsed())
                clk->setCurrent(tstamp);
              
              update_timer = true;
              ev_async_send(loop, &async_watcher);
            } else {
              // Woah! This comment is the only thing in this else
              // branch because this is a pretty serious state ... the
              // only way to make progress is for another node to send
              // a message or for an event to occur on a file
              // descriptor that a process is awaiting. We may want to
              // consider doing (or printing) something here.
            }
          }
        }

	/* Wait at gate if idle. */
	gate->arrive(old);
	continue;
      } else {
	gate->leave();
      }
    }

    process->lock();
    {
      //cout << "process->pid: " << process->pid << endl;
      //cout << "process->state: " << process->state << endl;
      assert(process->state == Process::INIT ||
	     process->state == Process::READY ||
	     process->state == Process::INTERRUPTED ||
	     process->state == Process::TIMEDOUT);

      /* Continue process. */
      proc_process = process;
      swapcontext(&proc_uctx_running, &process->uctx);
      while (legacy) {
	(*legacy_thunk)();
	swapcontext(&proc_uctx_running, &process->uctx);
      }
      proc_process = NULL;
    }
    process->unlock();
  } while (true);
}

#endif /* USE_LITHE */


#ifdef USE_LITHE
void trampoline(void *arg)
{
  assert(arg != NULL);
  Process *process = (Process *) arg;
  /* Run the process. */
  ProcessManager::instance()->run(process);
}
#else
void trampoline(int process0, int process1)
{
  /* Unpackage the arguments. */
#ifdef __x86_64__
  assert (sizeof(unsigned long) == sizeof(Process *));
  Process *process = (Process *)
    (((unsigned long) process1 << 32) + (unsigned int) process0);
#else
  assert (sizeof(unsigned int) == sizeof(Process *));
  Process *process = (Process *) (unsigned int) process0;
#endif /* __x86_64__ */
  /* Run the process. */
  ProcessManager::instance()->run(process);
}
#endif /* USE_LITHE */


/*
 * We might need/want to catch terminating signals to close our log
 * ... or the underlying filesystem and operating system might be
 * robust enough to flush our last writes and close the file cleanly,
 * or we might need to force flushes at appropriate times. However,
 * for now, adding signal handlers freely is not allowed because they
 * will clash with Java and Python virtual machines and causes hard to
 * debug crashes/segfaults. This can be revisited when recording gets
 * turned on by default.
 */

// void sigbad(int signal, struct sigcontext *ctx)
// {
//   if (recording) {
//     assert(!replaying);
//     record_msgs.close();
//     record_pipes.close();
//   }

//   /* Pass on the signal (so that a core file is produced).  */
//   struct sigaction sa;
//   sa.sa_handler = SIG_DFL;
//   sigemptyset(&sa.sa_mask);
//   sa.sa_flags = 0;
//   sigaction(signal, &sa, NULL);
//   raise(signal);
// }


static void initialize()
{
  static volatile bool initialized = false;
  static volatile bool initializing = true;

  // Try and do the initialization or wait for it to complete.
  if (initialized && !initializing) {
    return;
  } else if (initialized && initializing) {
    while (initializing);
    return;
  } else {
    if (!__sync_bool_compare_and_swap(&initialized, false, true)) {
      while (initializing);
      return;
    }
  }

//   /* Install signal handler. */
//   struct sigaction sa;

//   sa.sa_handler = (void (*) (int)) sigbad;
//   sigemptyset (&sa.sa_mask);
//   sa.sa_flags = SA_RESTART;

//   sigaction (SIGTERM, &sa, NULL);
//   sigaction (SIGINT, &sa, NULL);
//   sigaction (SIGQUIT, &sa, NULL);
//   sigaction (SIGSEGV, &sa, NULL);
//   sigaction (SIGILL, &sa, NULL);
// #ifdef SIGBUS
//   sigaction (SIGBUS, &sa, NULL);
// #endif
// #ifdef SIGSTKFLT
//   sigaction (SIGSTKFLT, &sa, NULL);
// #endif
//   sigaction (SIGABRT, &sa, NULL);
//   sigaction (SIGFPE, &sa, NULL);

#ifdef __sun__
  /* Need to ignore this since we can't do MSG_NOSIGNAL on Solaris. */
  signal(SIGPIPE, SIG_IGN);
#endif /* __sun__ */

#ifndef USE_LITHE
  /* Setup processing thread. */
  if (pthread_create (&proc_thread, NULL, schedule, NULL) != 0)
    fatalerror("failed to initialize (pthread_create)");
#endif /* USE_LITHE */

  char *value;

  /* Check environment for ip. */
  value = getenv("LIBPROCESS_IP");
  ip = value != NULL ? atoi(value) : 0;

  /* Check environment for port. */
  value = getenv("LIBPROCESS_PORT");
  port = value != NULL ? atoi(value) : 0;

  /* Check environment for replay. */
  value = getenv("LIBPROCESS_REPLAY");
  replaying = value != NULL;

  /* Setup for recording or replaying. */
  if (recording && !replaying) {
    /* Setup record. */
    time_t t;
    time(&t);
    std::string record(".record-");
    std::string date(ctime(&t));

    replace(date.begin(), date.end(), ' ', '_');
    replace(date.begin(), date.end(), '\n', '\0');

    /* TODO(benh): Create file if it doesn't exist. */
    record_msgs.open((record + "msgs-" + date).c_str(),
		     std::ios::out | std::ios::binary | std::ios::app);
    record_pipes.open((record + "pipes-" + date).c_str(),
		      std::ios::out | std::ios::app);
    if (!record_msgs.is_open() || !record_pipes.is_open())
      fatal("could not open record(s) for recording");
  } else if (replaying) {
    assert(!recording);
    /* Open record(s) for replay. */
    record_msgs.open((std::string(".record-msgs-") += value).c_str(),
		     std::ios::in | std::ios::binary);
    record_pipes.open((std::string(".record-pipes-") += value).c_str(),
		      std::ios::in);
    if (!record_msgs.is_open() || !record_pipes.is_open())
      fatal("could not open record(s) with prefix %s for replay", value);

    /* Read in all pipes from record. */
    while (!record_pipes.eof()) {
      uint32_t parent, child;
      record_pipes >> parent >> child;
      if (record_pipes.fail())
	fatal("could not read from record");
      (*replay_pipes)[parent].push_back(child);
    }

    record_pipes.close();
  }

  /* TODO(benh): Check during replay that the same ip and port is used. */

  // Lookup hostname if missing ip (avoids getting 127.0.0.1). Note
  // that we need only one ip address, so that other processes can
  // send and receive and don't get confused as to whom they are
  // sending to.
  if (ip == 0) {
    char hostname[512];

    if (gethostname(hostname, sizeof(hostname)) < 0)
      fatalerror("failed to initialize (gethostname)");

    /* Lookup IP address of local hostname. */
    struct hostent *he;

    if ((he = gethostbyname2(hostname, AF_INET)) == NULL)
      fatalerror("failed to initialize (gethostbyname2)");

    ip = *((uint32_t *) he->h_addr_list[0]);
  }

  /* Create a "server" socket for communicating with other nodes. */
  if ((s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP)) < 0)
    fatalerror("failed to initialize (socket)");

  /* Make socket non-blocking. */
  if (set_nbio(s) < 0)
    fatalerror("failed to initialize (set_nbio)");

  /* Set up socket. */
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = PF_INET;
  addr.sin_addr.s_addr = ip;
  addr.sin_port = htons(port);

  if (bind(s, (struct sockaddr *) &addr, sizeof(addr)) < 0)
    fatalerror("failed to initialize (bind)");

  /* Lookup and store assigned ip and assigned port. */
  socklen_t addrlen = sizeof(addr);
  if (getsockname(s, (struct sockaddr *) &addr, &addrlen) < 0)
    fatalerror("failed to initialize (getsockname)");

  ip = addr.sin_addr.s_addr;
  port = ntohs(addr.sin_port);

  if (listen(s, 500000) < 0)
    fatalerror("failed to initialize (listen)");

  /* Setup event loop. */
#ifdef __sun__
  loop = ev_default_loop(EVBACKEND_POLL | EVBACKEND_SELECT);
#else
  loop = ev_default_loop(EVFLAG_AUTO);
#endif /* __sun__ */

  ev_async_init(&async_watcher, handle_async);
  ev_async_start(loop, &async_watcher);

  ev_timer_init(&timer_watcher, handle_timeout, 0., 2100000.0);
  ev_timer_again(loop, &timer_watcher);

  ev_io_init(&server_watcher, handle_accept, s, EV_READ);
  ev_io_start(loop, &server_watcher);

//   ev_child_init(&child_watcher, child_exited, pid, 0);
//   ev_child_start(loop, &cw);

//   /* Install signal handler. */
//   struct sigaction sa;

//   sa.sa_handler = ev_sighandler;
//   sigfillset (&sa.sa_mask);
//   sa.sa_flags = SA_RESTART; /* if restarting works we save one iteration */
//   sigaction (w->signum, &sa, 0);

//   sigemptyset (&sa.sa_mask);
//   sigaddset (&sa.sa_mask, w->signum);
//   sigprocmask (SIG_UNBLOCK, &sa.sa_mask, 0);

  if (pthread_create(&io_thread, NULL, node, loop) != 0)
    fatalerror("failed to initialize node (pthread_create)");

  // Intialize the filter mutex.
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&filter_mutex, &attr);
  pthread_mutexattr_destroy(&attr);

  initializing = false;
}


Process::Process()
{
  initialize();

#ifdef USE_LITHE
  l = UNLOCKED;
#else
  pthread_mutex_init(&m, NULL);
#endif /* USE_LITHE */

  current = NULL;

  generation = 0;

  /* Initialize the PID associated with the process. */
#ifdef USE_LITHE
#error "TODO(benh): Make Lithe version include an htls proc_process."
#else
  if (!replaying) {
    /* Get a new unique pipe identifier. */
    pid.pipe = __sync_add_and_fetch(&global_pipe, 1);
  } else {
    /* Lookup pipe from record. */
    map<uint32_t, deque<uint32_t> >::iterator it = proc_process == NULL
      ? replay_pipes->find(0)
      : replay_pipes->find(proc_process->pid.pipe);

    /* Check that this is an expected process creation. */
    if (it == replay_pipes->end() && !it->second.empty())
      fatal("not expecting to create (this) process during replay");

    pid.pipe = it->second.front();
    it->second.pop_front();
  }

  if (recording) {
    assert(!replaying);
    record_pipes << " " << (proc_process == NULL ? 0 : proc_process->pid.pipe);
    record_pipes << " " << pid.pipe;
  }
#endif /* USE_LITHE */

  pid.ip = ip;
  pid.port = port;

  // If using a manual clock, try and set current time of process
  // using happens before relationship between creator and createe!
  synchronized(timers) {
    if (clk != NULL) {
      if (pthread_self() == proc_thread) {
        assert(proc_process != NULL);
        clk->setCurrent(this, clk->getCurrent(proc_process));
      } else {
        clk->setCurrent(this, clk->getCurrent());
      }
    }
  }
}


Process::~Process()
{
  //cout << "Process::~Process" << endl;
}


void Process::enqueue(struct msg *msg)
{
  assert(msg != NULL);

  synchronized(filter) {
    if (filtering) {
      assert(filterer != NULL);
      if (filterer->filter(msg)) {
        free(msg);
        return;
      }
    }
  }

  lock();
  {
    assert (state != EXITED);

    msgs.push_back(msg);

    if (state == RECEIVING) {
      state = READY;
      ProcessManager::instance()->enqueue(this);
    } else if (state == AWAITING) {
      state = INTERRUPTED;
      ProcessManager::instance()->enqueue(this);
    }

    assert(state == INIT ||
	   state == READY ||
	   state == RUNNING ||
	   state == PAUSED ||
	   state == WAITING ||
	   state == INTERRUPTED ||
	   state == TIMEDOUT);
  }
  unlock();
}


struct msg * Process::dequeue()
{
  struct msg *msg = NULL;

  lock();
  {
    assert (state == RUNNING);
    if (!msgs.empty()) {
      msg = msgs.front();
      msgs.pop_front();
    }
  }
  unlock();

  return msg;
}


PID Process::self() const
{
  return pid;
}


PID Process::from() const
{
  PID pid = { 0, 0, 0 };
  return current != NULL ? current->from : pid;
}


void Process::inject(const PID &from, MSGID id, const char *data, size_t length)
{
  if (replaying)
    return;

  /* Disallow sending messages using an internal id. */
  if (id < PROCESS_MSGID)
    return;

  /* Allocate/Initialize outgoing message. */
  struct msg *msg = (struct msg *) malloc(sizeof(struct msg) + length);

  msg->from.pipe = from.pipe;
  msg->from.ip = from.ip;
  msg->from.port = from.port;
  msg->to.pipe = pid.pipe;
  msg->to.ip = pid.ip;
  msg->to.port = pid.port;
  msg->id = id;
  msg->len = length;

  if (length > 0)
    memcpy((char *) msg + sizeof(struct msg), data, length);

  synchronized(filter) {
    if (filtering) {
      assert(filterer != NULL);
      if (filterer->filter(msg)) {
        free(msg);
        return;
      }
    }
  }

  lock();
  {
    msgs.push_front(msg);
  }
  unlock();
}


void Process::send(const PID &to, MSGID id, const char *data, size_t length)
{
  //cout << "Process::send" << endl;

  if (replaying)
    return;
  
  /* Disallow sending messages using an internal id. */
  if (id < PROCESS_MSGID)
    return;

  /* Allocate/Initialize outgoing message. */
  struct msg *msg = (struct msg *) malloc(sizeof(struct msg) + length);

  msg->from.pipe = pid.pipe;
  msg->from.ip = pid.ip;
  msg->from.port = pid.port;
  msg->to.pipe = to.pipe;
  msg->to.ip = to.ip;
  msg->to.port = to.port;
  msg->id = id;
  msg->len = length;

  if (length > 0)
    memcpy((char *) msg + sizeof(struct msg), data, length);

//   cout << endl;
//   cout << "msg->from.pipe: " << msg->from.pipe << endl;
//   cout << "msg->from.ip: " << msg->from.ip << endl;
//   cout << "msg->from.port: " << msg->from.port << endl;
//   cout << "msg->to.pipe: " << msg->to.pipe << endl;
//   cout << "msg->to.ip: " << msg->to.ip << endl;
//   cout << "msg->to.port: " << msg->to.port << endl;
//   cout << "msg->id: " << msg->id << endl;
//   cout << "msg->len: " << msg->len << endl;

  if (to.ip == ip && to.port == port)
    /* Local message. */
    ProcessManager::instance()->deliver(msg, this);
  else
    /* Remote message. */
    LinkManager::instance()->send(msg);
}


MSGID Process::receive(double secs)
{
  //cout << "Process::receive(" << secs << ")" << endl;
  /* Free current message. */
  if (current != NULL) {
    free(current);
    current = NULL;
  }

  /* Check if there is a message queued. */
  if ((current = dequeue()) != NULL)
    goto found;

#ifdef USE_LITHE
  /* TODO(benh): Account for a non-libprocess task/ctx. */
  /* Avoid blocking if negative seconds. */
  if (secs >= 0)
    ProcessManager::instance()->receive(this, secs);
    
  /* Check for a message (otherwise we timed out). */
  if ((current = dequeue()) == NULL)
    goto timeout;
#else
  if (pthread_self() == proc_thread) {
    /* Avoid blocking if negative seconds. */
    if (secs >= 0)
      ProcessManager::instance()->receive(this, secs);

    /* Check for a message (otherwise we timed out). */
    if ((current = dequeue()) == NULL)
      goto timeout;

  } else {
    /* Do a blocking (spinning) receive if on "outside" thread. */
    /* TODO(benh): Handle timeout. */
    do {
      lock();
      {
	if (state == TIMEDOUT) {
	  state = RUNNING;
	  unlock();
	  goto timeout;
	}
	assert(state == RUNNING);
      }
      unlock();
      usleep(50000); // 50000 == ~RTT 
    } while ((current = dequeue()) == NULL);
  }
#endif /* USE_LITHE */

 found:
  assert (current != NULL);

  if (recording)
    ProcessManager::instance()->record(current);

  return current->id;

 timeout:
  current = (struct msg *) malloc(sizeof(struct msg));
  current->from.pipe = 0;
  current->from.ip = 0;
  current->from.port = 0;
  current->to.pipe = pid.pipe;
  current->to.ip = pid.ip;
  current->to.port = pid.port;
  current->id = PROCESS_TIMEOUT;
  current->len = 0;

  if (recording)
    ProcessManager::instance()->record(current);

  return current->id;
}


MSGID Process::call(const PID &to, MSGID id,
		    const char *data, size_t length, double secs)
{
  send(to, id, data, length);
  return receive(secs);
}


const char * Process::body(size_t *length) const
{
  if (current != NULL && current->len > 0) {
    if (length != NULL)
      *length = current->len;
    return (char *) current + sizeof(struct msg);
  } else {
    if (length != NULL)
      *length = 0;
    return NULL;
  }
}


void Process::pause(double secs)
{
#ifdef USE_LITHE
  /* TODO(benh): Handle non-libprocess task/ctx (i.e., proc_thread below). */
  ProcessManager::instance()->pause(this, secs);
#else
  if (pthread_self() == proc_thread) {
    if (replaying)
      ProcessManager::instance()->pause(this, 0);
    else
      ProcessManager::instance()->pause(this, secs);
  } else {
    sleep(secs);
  }
#endif /* USE_LITHE */
}


PID Process::link(const PID &to)
{
  ProcessManager::instance()->link(this, to);
  return to;
}


bool Process::await(int fd, int op, const timeval& tv)
{
  return await(fd, op, tv, true);
}


bool Process::await(int fd, int op, const timeval& tv, bool ignore)
{
  double secs = tv.tv_sec + (tv.tv_usec * 1e-6);

  if (secs <= 0)
    return true;

  return ProcessManager::instance()->await(this, fd, op, secs, ignore);
}


bool Process::ready(int fd, int op)
{
  if (fd < 0)
    return false;

  fd_set rdset;
  fd_set wrset;

  FD_ZERO(&rdset);
  FD_ZERO(&wrset);

  if (op & RDWR) {
    FD_SET(fd, &rdset);
    FD_SET(fd, &wrset);
  } else if (op & RDONLY) {
    FD_SET(fd, &rdset);
  } else if (op & WRONLY) {
    FD_SET(fd, &wrset);
  }

  struct timeval timeout;
  memset(&timeout, 0, sizeof(timeout));

  select(fd+1, &rdset, &wrset, NULL, &timeout);

  return FD_ISSET(fd, &rdset) || FD_ISSET(fd, &wrset);
}


double Process::elapsed()
{
  double now = 0;

  acquire(timers);
  {
    if (clk != NULL) {
      now = clk->getCurrent(this);
    } else {
      // TODO(benh): Unclear if want ev_now(...) or ev_time().
      now = ev_time();
    }
  }
  release(timers);

  return now;
}


void Process::post(const PID &to, MSGID id, const char *data, size_t length)
{
  initialize();

  if (replaying)
    return;

  /* Disallow sending messages using an internal id. */
  if (id < PROCESS_MSGID)
    return;

  /* Allocate/Initialize outgoing message. */
  struct msg *msg = (struct msg *) malloc(sizeof(struct msg) + length);

  msg->from.pipe = 0;
  msg->from.ip = 0;
  msg->from.port = 0;
  msg->to.pipe = to.pipe;
  msg->to.ip = to.ip;
  msg->to.port = to.port;
  msg->id = id;
  msg->len = length;

  if (length > 0)
    memcpy((char *) msg + sizeof(struct msg), data, length);

//   cout << endl;
//   cout << "msg->from.pipe: " << msg->from.pipe << endl;
//   cout << "msg->from.ip: " << msg->from.ip << endl;
//   cout << "msg->from.port: " << msg->from.port << endl;
//   cout << "msg->to.pipe: " << msg->to.pipe << endl;
//   cout << "msg->to.ip: " << msg->to.ip << endl;
//   cout << "msg->to.port: " << msg->to.port << endl;
//   cout << "msg->id: " << msg->id << endl;
//   cout << "msg->len: " << msg->len << endl;

  if (to.ip == ip && to.port == port)
    /* Local message. */
    ProcessManager::instance()->deliver(msg);
  else
    /* Remote message. */
    LinkManager::instance()->send(msg);
}


PID Process::spawn(Process *process)
{
  initialize();

  if (process != NULL) {
    // If using a manual clock, try and set current time of process
    // using happens before relationship between spawner and spawnee!
    synchronized(timers) {
      if (clk != NULL) {
        if (pthread_self() == proc_thread) {
          assert(proc_process != NULL);
          clk->setCurrent(process, clk->getCurrent(proc_process));
        } else {
          clk->setCurrent(process, clk->getCurrent());
        }
      }
    }

    ProcessManager::instance()->spawn(process);
#ifdef USE_LITHE
    lithe_sched_request(1);
#endif /* USE_LITHE */
    return process->pid;
  } else {
    PID pid = { 0, 0, 0 };
    return pid;
  }
}


bool Process::wait(PID pid)
{
  initialize();

  /*
   * N.B. This could result in a deadlock! We could check if such was
   * the case by doing:
   *
   *   if (proc_process && proc_process->pid == pid) {
   *     handle deadlock here;
   *  }
   *
   * But for now, deadlocks seem like better bugs to try and fix than
   * segmentation faults that might occur because a client thinks it
   * has waited on a process and it is now finished (and can be
   * cleaned up).
   */

  return ProcessManager::instance()->wait(pid);
}


bool Process::wait(Process *process)
{
  if (process == NULL)
    return false;

  return wait(process->getPID());
}


void Process::invoke(const std::tr1::function<void (void)> &thunk)
{
  initialize();
  legacy_thunk = &thunk;
  legacy = true;
  assert(proc_process != NULL);
  swapcontext(&proc_process->uctx, &proc_uctx_running);
  legacy = false;
}


void Process::filter(MessageFilter *filter)
{
  initialize();

  acquire(filter);
  {
    filterer = filter;
    filtering = filter != NULL;
  }
  release(filter);
}
