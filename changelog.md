# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

---

## [Unreleased]

### Added
- Initial design plan (`design/modbusSensorEmulator.md`) covering:
  - FreeRTOS task architecture (modbus slave, per-sensor mode tasks, live-fetch, replay, NTP, web server, WiFi manager)
  - Modbus register maps for FG6485A (FC03) and S200 (FC04)
  - Three sensor modes per sensor: manual, live (Open-Meteo API), replay (timestamped CSV)
  - Web interface layout: status, per-sensor config cards, WiFi settings, Modbus activity log
  - NVS settings key table
  - WiFi AP/STA switching with mDNS (`emulator.local`)
  - NTP + manual time fallback
  - IP-geolocation for automatic location detection (live mode)
  - CSV replay file format
  - RGB LED behaviour (blue idle / green blink / red blink / red solid)
  - Development sequence
