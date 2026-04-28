# Capacitor Replacement

The SN110 board has two electrolytic capacitors. On devices manufactured in
2004, these caps can degrade to the point where the 3.3V rail is unstable,
even if they pass a basic capacitance check and show no visible signs of
failure (no bulging, no leaking).

## Symptoms

- Device appears completely dead: no status LEDs, blank LCD, no network presence
- Measuring voltage between Vout and GND on the board shows ~1V fluctuating
  instead of the expected stable +3.3 VDC
- Capacitors pass a multimeter capacitance check and look fine externally

## Capacitors on the Board

There are exactly two electrolytic capacitors on the SN110 board. Both should
be replaced as a pair.

| Location | Original Spec | Replacement Part |
|----------|--------------|------------------|
| C1 | 100 uF / 100V | Rubycon ZLH 100ZLH100MEFC10X20 |
| C2 | 2200 uF / 25V | Rubycon ZLH 25ZLH2200MEFC12.5X30 |

## Replacement Parts

### 100 uF / 100V (C1)

| Parameter | Value |
|-----------|-------|
| **Part number** | Rubycon 100ZLH100MEFC10X20 |
| **Series** | ZLH (low impedance, long life) |
| **Capacitance** | 100 uF (+/-20%) |
| **Voltage** | 100V DC |
| **Temperature** | 105C, 10,000 hours |
| **Dimensions** | 10mm dia x 20mm, 5mm lead pitch |
| **DigiKey** | [100ZLH100MEFC10X20](https://www.digikey.com/en/products/detail/rubycon/100ZLH100MEFC10X20/3563824) |

### 2200 uF / 25V (C2)

| Parameter | Value |
|-----------|-------|
| **Part number** | Rubycon 25ZLH2200MEFC12.5X30 |
| **Series** | ZLH (low impedance, long life) |
| **Capacitance** | 2200 uF (+/-20%) |
| **Voltage** | 25V DC |
| **Temperature** | 105C, 10,000 hours |
| **Dimensions** | 12.5mm dia x 30mm, 5mm lead pitch |
| **DigiKey** | [25ZLH2200MEFC12.5X30](https://www.digikey.com/en/products/detail/rubycon/25ZLH2200MEFC12-5X30/3564206) |

## Notes

The Rubycon ZLH series is a good match for this application: 105C rated,
10,000-hour lifetime, and low impedance. Any equivalent 105C capacitor with
matching capacitance, voltage rating, and physical dimensions will work.

Observe polarity when soldering -- the negative stripe on the capacitor body
must match the board markings.
