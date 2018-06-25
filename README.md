# sigrok-servo-pwm
sigrok-compatible pwm (servo/esc) and SBus signal analyzer


Decoder for pulse-width modulation and FrSky SBus signal used in ESC/servo controllers. 
Usage steps:
* Connect sigrok-compatible logic analyzer (e.g. saleae logic, usbee ax or clones) to your rc-receiver or flight controller outputs.
* Power on transmitter/receiver/flightcontroller
* run 
```
sigrok-cli -d fx2lafw --config samplerate=20k --continuous -p 0,1,2,3,4,5 -o /dev/stdout -O binary | ./pwm -s 20 
```


* run with SBus receivers on probes 0 and 1:
```
sigrok-cli -d fx2lafw --config samplerate=2m --continuous -p 0,1 -o /dev/stdout -O binary | ./pwm -s 2000 -b 
```

# sigrok

http://sigrok.org/ 
