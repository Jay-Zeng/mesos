import bottle
import commands
import datetime
import sqlite3
import json
import os

from bottle import route, send_file, template, response

start_time = datetime.datetime.now()
database_location = os.getcwd() + '/logs/event_history_db.sqlite3'


@route('/')
def index():
  bottle.TEMPLATES.clear() # For rapid development
  return template("index", start_time = start_time)


@route('/framework/:id#[0-9]*\-[0-9]*#')
def framework(id):
  bottle.TEMPLATES.clear() # For rapid development
  return template("framework", framework_id = id)


@route('/static/:filename#.*#')
def static(filename):
  send_file(filename, root = './webui/static')


@route('/log/:level#[A-Z]*#')
def log_full(level):
  send_file('nexus-master.' + level, root = '/tmp',
            guessmime = False, mimetype = 'text/plain')


@route('/log/:level#[A-Z]*#/:lines#[0-9]*#')
def log_tail(level, lines):
  bottle.response.content_type = 'text/plain'
  return commands.getoutput('tail -%s /tmp/nexus-master.%s' % (lines, level))

#generate list of task history using JSON
@route('/tasks_json')
def tasks_json():
  try:
    conn = sqlite3.connect(database_location)
  except:
    return "Error opening database at " + database_location
  conn.row_factory = sqlite3.Row
  c = conn.cursor()
  c.execute("SELECT * FROM task;")
  result = c.fetchall()
  json_string = "{\"ResultSet\": {\"TotalItems\":" + str(len(result)) + ", \"Items\":[\n"
  k = 0
  for row in result:
    k += 1
    json_string += "\t\t{\n"
    i = 0
    for col in row.keys():
      i += 1
      if col == "resource_list":
        json_string += "\t\t\"" + col + "\":" + str(row[col])
      else:
        json_string += "\t\t\"" + col + "\":\"" + str(row[col]) + "\""
      if i != len(row.keys()):
        json_string += ","
      json_string += "\n"
    json_string += "\t\t}"
    if k != len(result):
      json_string += ","
    json_string += "\n"
  json_string += "\t]}\n}"
  response.header['Content-Type'] = 'text/plain' 
  return str(json_string)

#generate list of framework history using JSON
@route('/frameworks_json')
def frameworks_json():
  try:
    conn = sqlite3.connect(database_location) 
  except:
    return "Error opening database at " + database_location
  conn.row_factory = sqlite3.Row
  c = conn.cursor()
  c.execute("SELECT * FROM framework;")
  result = c.fetchall()
  json_string = "{\"ResultSet\": {\"TotalItems\":" + str(len(result)) + ", \"Items\":[\n"
  k = 0
  for row in result:
    k += 1
    json_string += "\t\t{\n"
    i = 0
    for col in row.keys():
      i += 1
      json_string += "\t\t\"" + col + "\":\"" + str(row[col]) + "\""
      if i != len(row.keys()):
        json_string += ","
      json_string += "\n"
    json_string += "\t\t}"
    if k != len(result):
      json_string += ","
    json_string += "\n"
  json_string += "\t]}\n}"
  response.header['Content-Type'] = 'text/plain' 
  return str(json_string)

#The following is for debug only
#TODO(andyk):delete the following eventually
@route('/test')
def test():
  json_string = "{\"ResultSet\": {\"TotalItems\":1, \"Items\":[{\"taskid\":\"0\"}]}}"
  response.header['Content-Type'] = 'text/plain' 
  return str(json_string)

#The following is no longer used, was used for google api graphs, will break
#TODO(andyk):delete the following eventually
@route('/utilization_table')
def utilization_table():
  description = [("datetime", "datetime", "datetime"),
                 ("fw_id", "number", "fw_id"),
                 ("fw_name", "string", "fw_name"),
                 ("cpus", "number", "cpus")]
  dt = gviz_api.DataTable(description)

  #read in the history of master utilization,   FILE = open("/mnt/shares",'r')
  datarow = ""
  fws = {}
  for line in FILE:
    row = line.split("#")
    #here is what we get from /mnt/shares: tick, timestamp in microseconds, f->id, f->name,  f->cpus,  f->mem, cpu_share, mem_share, max_share
    if len(row)>1:
      timestamp = datetime.datetime.fromtimestamp(float(row[1])/1000000.0)
      #fws[int(row[2])] = {'datetime': timestamp, 'fw_id':int(row[2]), 'fw_name':row[3], 'cpus':int(row[4]), 'mem':int(row[5]), 'cpu_share':float(row[6]), 'mem_share'}
    
  #Now make a data table to hold the last 100 frameworks, and add data (date, util) points to the
  #data table which will be consumed by the visualization on the nexus webui
      #datarow = datarow + row[0] + ", " + row[1] + ", date_time: " + str(date_time) + "\n" 
      #data_row = [[date_time, int(row[2]), row[3], int(row[4])]]
      #dt.AppendData(data_row)
  
    #since the format that annotatedtimeline requires is 
    #(datetime or date, y-val, [annotation_title, annotation text]),
    #lets only send a filtered view of this data table
  #response.header['Content-Type'] = 'text/plain' 
  #return dt.ToJSonResponse()
  #return datarow
  return ""


bottle.TEMPLATE_PATH.append('./webui/master/%s.tpl')
bottle.run(host = '0.0.0.0', port = 8080)
