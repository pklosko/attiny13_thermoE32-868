#!/usr/bin/python3

# uart2IoTd - Daemon 
#   - Read serial data from IoT devices, check/parse packet and push to IoT server
#
#
# (C) 2019 Petr KLOSKO
#  https://www.klosko.net/

import serial
import time
import datetime
import urllib.request
import logging
import logging.handlers
import sys
import os
import argparse
import syslog


# Settings and Defaults
SW_NAME      = "uart2IoTd.py"
SW_VERSION   = "20191103-1"
SW_MACHINE   = "omnia.home"              # Machine ID/name
SER_PORT     = "/dev/ttyUSB0"            # Serial port - COMx[win], /dev/ttyX[Linux]
SER_BAUD     = 9600                      # Baudrate
FW_VER       = 20180000                  # Firmware version prefix , tinny uses only 2 bytes
LOG_FILENAME = "/var/log/uart2IoTd.log"  # File name 
LOG_LEVEL    = logging.INFO              # Could be e.g. "INFO", DEBUG" or "WARNING"
DEBUG        = False                     # For debug mode ste to True
DAEMON       = False                     # For Daemno mode ste to True - auto setting in main(void)

REST_API     = {'host'   : 'http://www.klosko.net',  # Change to you URI
                'method' : '/push/?',                # Push method [API]
                'ua'     : 'IoTtinny13.'             # User agent prefix
               }
               
# Known devices description and values/infos
# 1: Key = DeviceID
#    0: MCU type
#    1: Connection type
#    2: HW desription, sensors, etc.
#    3: REST API Values to send | Key: position in the packet
#                                 0: Value NAME  / ?NAME=
#                                 1: Value dividier
# 
DEVICES = {32: ["tiny13", "Cable",   "ATtiny13-E32-828-pl2302",             {7: ["bc", 1] , 9:["pc",1]}],
           33: ["tiny13", "LoRaWAN", "ATtiny13-E32-828;Sensor:DS18B20+ADC", {7: ["t", 100], 9:["ub",100]}]
          }


def l(message):
  global DAEMON
  if DAEMON:
    syslog.syslog(message)
  else:
    sys.stderr.write(message + "\n")



def run():
  global SW_NAME, SW_VERSION, SW_MACHINE, SER_PORT, SER_BAUD, REST_API, FW_VER, LOG_FILENAME, LOG_LEVEL, DEBUG, DEVICES

# Define and parse command line arguments
  parser = argparse.ArgumentParser(description=SW_NAME+" v."+SW_VERSION+" Parse and send serial data to "+REST_API['host'])
  parser.add_argument("-l", "--log", help="File to write log to (default '" + LOG_FILENAME + "')")
  parser.add_argument("-p", "--port", help="Serial port interface (default '" + SER_PORT + "')")
  parser.add_argument("-d", "--debug", help="Prin output to stdout (default 'False')")

# If the log file, port, and debug switch is specified on the command line then override the defaults
  args = parser.parse_args()
  if args.log:
        LOG_FILENAME = args.log

  if args.port:
        SER_PORT = args.port

  if args.debug:
        DEBUG = args.debug

  logger    = logging.getLogger(__name__)
  logger.setLevel(LOG_LEVEL)
  handler   = logging.handlers.TimedRotatingFileHandler(LOG_FILENAME, when="midnight", backupCount=3)
  formatter = logging.Formatter('%(asctime)s %(levelname)-8s %(message)s')
  handler.setFormatter(formatter)
  logger.addHandler(handler)


  class MyLogger(object):
        def __init__(self, logger, level):
                """Needs a logger and a logger level."""
                self.logger = logger
                self.level = level

        def write(self, message):
                # Only log if there is a message (not just a new line)
                if message.rstrip() != "":
                        self.logger.log(self.level, message.rstrip())

        def flush(self):
                pass

  if (DEBUG == False):
# Replace stdout with logging to file
    sys.stdout = MyLogger(logger, logging.INFO)
    sys.stderr = MyLogger(logger, logging.ERROR)
    sys.stderr = MyLogger(logger, logging.DEBUG)

# Packet CRC calculation , based on DALLAS CRC 8
  def _crc8(crc, data):
    crc = (crc ^ data)
    for i in range(0,8):
      if (crc & 0x01):
        crc = ((crc >> 1) ^ 0x8C)
      else:
        crc >>= 1
    return crc

# Calculate pseudo MAC from Device ID
  def _id2mac(devID):
    MAC="";
    for x in range(6):  
      MAC = MAC + format(_crc8(x, devID), 'x')
    return MAC

# Check packet validity / struct
  def CheckPacket(packet):
    crc = 0
    for j in range(0,(packet[2])):
      crc = _crc8(crc, packet[j])  
    return (packet[0] == 0xCC and 
            packet[1] == 0x55 and 
            len(packet) == packet[2] 
            and crc == 0 )  
    
# Push to IoT server via REST API   
  def DoHTTPrequest(host, devID,  version, interval, status, Sdata):
    global DEBUG
    try:
      IDtxt  = _id2mac(devID)

      try:
        conn = DEVICES[devID][1]
      except:
        conn = "Unknown"

      try:
        mcu = DEVICES[devID][0]
      except:
        mcu = "unknown"

      try:
        device = DEVICES[devID][2]
      except:
        device = "Unknown"
      
      url = host + REST_API['method']
      for i in range(len(Sdata)):
        url = url + Sdata[i] + '&'

      headers = {}
      headers['User-Agent']  = REST_API['ua'] + "" + mcu + "-" + format(devID, 'x') + "-" + IDtxt
      headers['Device-Info'] = "Device:" + device + \
                               ";Status:" + str(status) + \
                               ";FW:" + str(version) + \
                               ";SW:" + SW_NAME+".v"+SW_VERSION+"@"+SW_MACHINE + \
                               ";Interval:" + str(interval) + \
                               ";Conn:" + conn
      req = urllib.request.Request(url, headers = headers)
      resp = urllib.request.urlopen(req)
      if (DEBUG):
        print(headers['User-Agent']) 
        print(headers['Device-Info'])                        
        respData = resp.read()
        print(respData)
    except Exception as e:
      print(str(e)) 

# Serial port init/setting          
  ser = serial.Serial(
      port=SER_PORT,\
      baudrate=SER_BAUD,\
      parity=serial.PARITY_NONE,\
      stopbits=serial.STOPBITS_ONE,\
      bytesize=serial.EIGHTBITS,\
        timeout=0)

  print(SW_NAME+"[v."+SW_VERSION+"]: Start")
  if (DEBUG):
    print("DEBUG MODE")
  print("Connected to: " + ser.portstr)

# Reading data loop
  data = []
  cnt  = 0
  while True:
    for c in ser.read():
        data.append(c)
        if (DEBUG):
          print(' '.join(map(hex, data)))    # print all bytes - just for debug
        if (cnt > 2):
          if (cnt == data[2]-1):             # get data/packet length
            print(' '.join(map(hex, data)))  # just for debug
            if CheckPacket(data):
              devID   = data[3]                              # Device ID
              int     = data[4]                              # Interval
              ver     = FW_VER + ((data[5] * 256) + data[6]) # Device sketch version
              Sdata   = []
              for i in range(7,10,2):                        # Fill REST API values
                Sdata.append(DEVICES[devID][3][i][0] + '=' + str('%.2f'%(((data[i] * 256) + data[i+1]) / DEVICES[devID][3][i][1])))
              status  = data[11]                             # Device status
              DoHTTPrequest(REST_API['host'], devID, ver, int, status, Sdata)
              if (DEBUG):
                print(datetime.datetime.now().strftime("%b %d %Y %H:%M:%S") + " : DevID=" + str(devID) + "; Status=" +str(status) + "; " + " | ".join(Sdata))
                print()
            data = []
            cnt  = 0
            break
        cnt += 1

  ser.close()
  print("Exit")

# Some functions for daemonize
def create_daemon():
  try:
    pid = os.fork()
    l('Serial2IoTd.py Start: %s' % str(pid))
    if pid > 0:
      sys.exit(0)

  except OSError as e:
    l('Unable to fork. Error: %s' % str(e))
    sys.exit(1)

  run()


def main():
  global DAEMON
  DAEMON = True
  create_daemon()

if __name__ == '__main__':
        main()

