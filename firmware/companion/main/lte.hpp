// TODO(LTE): EG912U LTE/GNSS modem provider.
//
// The production companion connects an EG912U over UART (AT + PPP via esp_modem). This
// prototype has no modem, so the provider reports "not present" and the connectivity manager
// never selects it. Contract for the real implementation:
//   * start() powers the modem, registers on the network and brings up PPP (esp_netif),
//     returning once the attempt is scheduled (non-blocking).
//   * stop() tears PPP down and puts the modem in low-power mode; it is called as soon as
//     home Wi-Fi is connected, so LTE is never used for data while on Wi-Fi.
//   * GNSS fixes from the same modem replace position::SimulatedProvider.
#pragma once

namespace lte {

inline bool present() { return false; }
inline bool start() { return false; }
inline void stop() {}
inline bool connected() { return false; }

}  // namespace lte
