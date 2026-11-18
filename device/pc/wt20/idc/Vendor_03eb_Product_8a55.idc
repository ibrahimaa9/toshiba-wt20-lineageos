# Toshiba Portege WT20-B touchscreen (Atmel maXTouch over USB HID)
touch.deviceType = touchScreen
touch.orientationAware = 1
# Its last frame before a release repeats the previous position ~37 ms late, which reads as
# "finger stopped" and kills flings; see TouchInputMapper::dispatchTouches
touch.ignoreStationaryFrames = 1
# Built into the tablet (USB HID makes Android treat it as external, which misroutes touch
# to an attached HDMI display)
device.internal = 1
