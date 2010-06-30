%import os
%from datetime import datetime
%from master import get_master
%from webui_lib import *
%
%master = get_master()
<html>
<head>
<title>Nexus Master on {{HOSTNAME}}</title>
<link rel="stylesheet" type="text/css" href="/static/stylesheet.css" />
  <script type='text/javascript' src='http://www.google.com/jsapi'></script>
  <script type='text/javascript'>
    google.load('visualization', '1', {'packages':['annotatedtimeline']});
    google.setOnLoadCallback(drawChart);
    function drawChart() {
      //var url = "http://google-visualization.appspot.com/python/dynamic_example";
      var url = "utilization_table";
      var query = new google.visualization.Query(url);
      query.send(handleQueryResponse);
    }

      function handleQueryResponse(response) { 
        var data = response.getDataTable();
        var chart = new google.visualization.AnnotatedTimeLine(document.getElementById('chart_div'));
        chart.draw(data, {displayAnnotations: true});
      }
  </script>
</head>
<body>

<h1>Nexus Master on {{HOSTNAME}}</h1>

<p>
Built: {{master.build_date}} by {{master.build_user}}<br />
Started: {{format_time(start_time)}}<br />
PID: {{master.pid}}<br />
Slaves: {{master.slaves.size()}}<br />
Frameworks: {{master.frameworks.size()}}<br />
Fault-tolerant: {{master.isFT}}<br />
</p>

<p>
Log:
<a href="/log/INFO/100">[last 100 lines]</a>
<a href="/log/INFO">[full]</a>
</p>

<h2>Resources</h2>

%total_cpus = 0
%total_mem = 0
%for s in master.slaves:
%  total_cpus += s.cpus
%  total_mem += s.mem
%end

%running_cpus = 0
%running_mem = 0
%for framework in master.frameworks:
%  for task in framework.tasks:
%    running_cpus += task.cpus
%    running_mem += task.mem
%  end
%end

%offered_cpus = 0
%offered_mem = 0
%for f in master.frameworks:
%  for o in f.offers:
%    for r in o.resources:
%      offered_cpus += r.cpus
%      offered_mem += r.mem
%    end
%  end
%end
%idle_cpus = total_cpus - (offered_cpus + running_cpus)
%idle_mem = total_mem - (offered_mem + running_mem)

<p>
Total in Cluster: {{total_cpus}} CPUs, {{format_mem(total_mem)}} MEM<br />
In Use: {{running_cpus}} CPUs, {{format_mem(running_mem)}} MEM<br />
Offered: {{offered_cpus}} CPUs, {{format_mem(offered_mem)}} MEM<br />
Idle: {{idle_cpus}} CPUs, {{format_mem(idle_mem)}} MEM<br />
</p>

<h2>Frameworks</h2>

%# TODO: Sort these by framework ID
%if master.frameworks.size() > 0:
  <table>
  <tr>
  <th>ID</th>
  <th>Name</th>
  <th>Executor</th>
  <th>Running Tasks</th>
  <th>CPUs</th>
  <th>MEM</th>
  <th>Max Share</th>
  <th>Connected</th>
  </tr>
  %for framework in master.frameworks:
    %cpu_share = 0
    %if total_cpus > 0:
    %  cpu_share = framework.cpus / float(total_cpus)
    %end
    %mem_share = 0
    %if total_mem > 0:
    %  mem_share = framework.mem / float(total_mem)
    %end
    %max_share = max(cpu_share, mem_share)
    <tr>
    <td>{{framework.id}}</td>
    <td><a href="/framework/{{framework.id}}">{{framework.name}}</a></td>
    <td>{{os.path.basename(framework.executor)}}</td>
    <td>{{framework.tasks.size()}}</td>
    <td>{{framework.cpus}}</td>
    <td>{{format_mem(framework.mem)}}</td>
    <td>{{'%.2f' % max_share}}</td>
    <td>{{format_time(framework.connect_time)}}</td>
    </tr>
  %end
  </table>
%else:
  <p>No frameworks are connected.</p>
%end

<h2>Slaves</h2>

%# TODO: Sort these by slave ID
%if master.slaves.size() > 0:
  <table>
  <tr>
  <th>ID</th>
  <th>Hostname</th>
  <th>CPUs</th>
  <th>MEM</th>
  <th>Connected</th>
  </tr>
  %for s in master.slaves:
    <tr>
    <td>{{s.id}}</td>
    <td><a href="http://{{s.public_dns}}:8081/">{{s.public_dns}}</a></td>
    <td>{{s.cpus}}</td>
    <td>{{format_mem(s.mem)}}</td>
    <td>{{format_time(s.connect_time)}}</td>
    </tr>
  %end
  </table>
%else:
  <p>No slaves are connected.</p>
%end

<h2>Resource Offers</h2>

%# TODO: Sort these by slot offer ID
%if offered_cpus > 0 or offered_mem > 0:
  <table>
  <tr>
  <th>Offer ID</th>
  <th>Framework ID</th>
  <th>CPUs</th>
  <th>MEM</th>
  <th>Slave IDs</th>
  </tr>
  %for f in master.frameworks:
    %for o in f.offers:
      %slave_ids = []
      %cpus = 0
      %mem = 0
      %for r in o.resources:
        %slave_ids.append(str(r.slave_id))
        %cpus += r.cpus
        %mem += r.mem
      %end
      <tr>
      <td>{{o.id}}</td>
      <td>{{o.framework_id}}</td>
      <td>{{cpus}}</td>
      <td>{{format_mem(mem)}}</td>
      <td>{{", ".join(slave_ids)}}</td>
      </tr>
    %end
  %end
  </table>
%else:
  <p>No offers are active.</p>
%end

<h2>Cluster Utilization</h2>
<div id='chart_div' style='width: 700px; height: 240px;'></div>

</body>
</html>
