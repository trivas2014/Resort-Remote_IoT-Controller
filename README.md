ESP32 Smart Hot Tub Automation

**Overview**

This repository contains the C++ firmware and configuration details for a custom-engineered, ESP32-based hot tub controller. The system modernizes an existing hot tub by integrating secure, remote over-the-air (OTA) control of the 120V gas furnace and 24V pump systems via a mobile web application.

**System Architecture**

Microcontroller: ESP32

Hardware Integration: 4 opto-isolated relays and 2 mechanical actuators spliced into existing high-voltage circuits.

Backend: Google Firebase Realtime Database (RTDB) providing sub-100ms latency for instant control and monitoring.

Frontend: Progressive Web App (PWA) with secure user authorization.

**Key Features & Safety Mechanisms**

Real-Time Synchronization: Utilizes Firebase RTDB streams to ensure the physical hardware state and the mobile UI are perfectly synchronized in real-time.

Automated Recovery: A custom boot-watchdog loop is programmed to safely recover and reset the system state following a power outage.

Hardware Fail-Safes: The master 120V timer was physically rewired and integrated with the embedded logic to guarantee mechanical shutdown, preventing runaway pump operations or overheating.
