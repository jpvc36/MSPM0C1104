# 📘 MSPM0C1104 I2C Register Read Example (PCM9211 Compatible)

This example demonstrates how to perform a register read using the I2C controller on the **LP-MSPM0C1104** LaunchPad. It is a **fully interrupt-driven**, low-level implementation using **TI DriverLib (not TI-Drivers)**.

---

## ✅ Key Features

- Uses **I2C0** with:
  - **SDA = PA0**
  - **SCL = PA11**
- Sends a 1-byte register address to slave at **address 0x40**
- Performs a **repeated START** followed by a **1-byte read**
- Validated with a **TI PCM9211** audio codec
- Fully interrupt-based using **TI DriverLib APIs**
- LED toggles after a successful transfer (or breakpoint can be used to inspect data)

---

## 🔧 Requirements

- **LaunchPad**: LP-MSPM0C1104
- **IDE**: Code Composer Studio (CCS Theia or Eclipse)
- **Toolchain**: TI Arm Clang
- **TI SDK**: MSPM0 SDK v1.40.0.03 or later

---

## 📁 Files

| File | Description |
|------|-------------|
| `ReadByte.c` | Main application performing I2C register read |
| `ReadByte.syscfg` | SysConfig file enabling I2C0 on correct pins |

---

## 🧪 Behavior

1. The controller sends a **1-byte register address** (`0x34`) to slave `0x40`
2. It then performs a **read of 1 byte**
3. Received data is stored in `gRxPacket[0]`
4. If successful, onboard LEDs toggle forever (or use a breakpoint to inspect value)

---

## 🚀 How to Use

1. Import this project into **CCS**
2. Ensure `ReadByte.syscfg` is opened and saved so the pins/I2C config regenerate
3. Connect the PCM9211 (or compatible device) to:
   - **SDA → PA0**
   - **SCL → PA11**
4. Build and flash the project
5. Set a breakpoint after the read to inspect `gRxPacket[0]`
6. Watch the onboard LED blink if everything went well

---

## 🔌 Hardware Notes

- External I2C pull-ups (4.7kΩ–10kΩ) may be required on SCL/SDA
- PCM9211 should be powered and responsive at address `0x40`

---

## 🧠 Tips

- This example is minimal and easy to extend for multi-byte register access
- For more complex transfers, adjust `gTxPacket`, `gTxLen`, and `gRxLen`
- You can expand the ISR to handle FIFO-based burst transfers

---
