# Engineering Contract


## ESP32 firmware handoff · 2026-11-04

Problem: preserve and review the board bring-up, PTT recovery, desktop standby and BLE/mobile control work as a reproducible firmware-only handoff.
Scope: voice 0.5.17-handoff, offline hwtest-2-handoff, native tests, pinned dependencies and device docs. No server models, browser tools, phone App, real credentials or deployable images.
Architecture/contract: existing half-duplex WS PCM remains; capability-gated lab receipts and physically authorized BLE management are documented in docs/firmware-handoff-2026-11-04.md.
Verification: five native suites, isolated ESP-IDF 6.1.0 TLS/plain/self-test builds, source comparison and credential review passed; machine evidence is docs/firmware-review-2026-11-04.json.
Release boundary: review candidate only, not flashed this round. Battery ADC, recording uplink disconnects, V2 hardware acceptance and BLE/WiFi/TLS long-run coexistence remain open. See handoff for safe active-slot update and rollback.
