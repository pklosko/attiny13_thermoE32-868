# attiny13_thermoE32-868

**ATTiny13 - DS18b20 thermometer**

- Send data via E32-828 device [LoRaWan], processed by Turris Omnia router (Python daemon) & REST API


---

**Arduino IDE settings:**

   ATtiny13, 
   1.2MHz internal; 
   LTO disabled; 
   BOD disabled
   
---

**Router side:**

   HW : Turris Omnia, Python installed
   SW :  router/uart2IoTd.py Daemon
   
   LoRaWan HW: E32-868 + PL2302 (directly wired) + hot glue + housing
   
   ![Alt text](E32-868-PL2302_thumb.jpg?raw=true "ATTiny13 - DS18b20 thermometer, Router Side")
   
---

**(c) 2019 Petr KLOSKO**
 
 UART code by 
      Łukasz Podkalicki <lpodkalicki@gmail.com> @lpodkalicki
      [https://blog.podkalicki.com/100-projects-on-attiny13/]
