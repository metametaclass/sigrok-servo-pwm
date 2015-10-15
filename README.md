# sigrok-servo-pwm
sigrok-compatible pwm (servo/esc) signal analyzer


Decoder for pulse-width modulation signal used in ESC/servo controllers. 
Usage steps:
* Connect sigrok-compatible logic analyzer (e.g. saleae logic, usbee ax or clones) to your rc-receiver or flight controller outputs.
* Power on transmitter/receiver/flightcontroller
* run 
```
sigrok-cli -d fx2lafw --config samplerate=20k --continuous -p 0,1,2,3,4,5 -o /dev/stdout -O hex | ./pwm -s 20 
```

# sigrok

http://sigrok.org/ 
