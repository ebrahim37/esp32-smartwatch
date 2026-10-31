// Settings: connections (companion, phone, AirPods, home Wi-Fi), notifications, vibration,
// display, time zone, web configuration window and device information.
#include <cstring>

#include "app/services.hpp"
#include "app/settings.hpp"
#include "app/state.hpp"
#include "ble/central.hpp"
#include "ble/link.hpp"
#include "board.hpp"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "ui/ui.hpp"

namespace ui {
namespace {

std::string mac_str(const uint8_t* m) {
    char b[20];
    snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

// NimBLE stores addresses little-endian; display them in the usual order.
std::string ble_addr_str(const uint8_t* a) {
    uint8_t r[6];
    for (int i = 0; i < 6; ++i) r[i] = a[5 - i];
    return mac_str(r);
}

class ListScreen : public Screen {
public:
    void build(lv_obj_t* c) override {
        c_ = c;
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(c, 12, 0);
        lv_obj_set_style_pad_row(c, 8, 0);
        populate();
    }
    void rebuild() {
        int32_t y = lv_obj_get_scroll_y(c_);
        clear(c_);
        populate();
        lv_obj_scroll_to_y(c_, y, LV_ANIM_OFF);
    }
    virtual void populate() = 0;

protected:
    lv_obj_t* c_ = nullptr;
};

// ---- Companion -------------------------------------------------------------------------

class CompanionScan : public ListScreen {
public:
    const char* title() const override { return "Find Companion"; }
    void on_shown() override { central::start_scan(); }
    ~CompanionScan() override { central::stop_scan(); }
    void refresh(uint32_t d) override {
        if (d & app::kDirtyScan) rebuild();
    }
    void populate() override {
        std::vector<app::ScanEntry> list;
        bool scanning;
        {
            app::Locked s;
            list = s->scan;
            scanning = s->scanning;
        }
        section_header(c_, scanning ? "Searching nearby..." : "Scan finished");
        for (auto& e : list) {
            std::string sub = ble_addr_str(e.addr) + "  " + std::to_string(e.rssi) + " dBm";
            app::ScanEntry copy = e;
            list_row(c_, e.name.c_str(), sub.c_str(), nullptr, [copy] {
                central::pair(copy.addr, copy.addr_type, copy.name.c_str());
                toast("Pairing... enter the passkey from the companion's serial console");
                pop();
            }, color::blue(), LV_SYMBOL_BLUETOOTH);
        }
        if (!scanning) list_row(c_, "Scan again", nullptr, nullptr, [] { central::start_scan(); }, color::card2(), LV_SYMBOL_REFRESH);
    }
};

class CompanionScreen : public ListScreen {
public:
    const char* title() const override { return "Companion"; }
    void refresh(uint32_t d) override {
        if (d & (app::kDirtyLink | app::kDirtyCompanion)) rebuild();
    }
    void populate() override {
        app::LinkInfo li;
        {
            app::Locked s;
            li = s->link;
        }
        std::string st = app::link_state_name(li.state);
        if (li.state == app::LinkState::Waiting) st += " (retry in " + std::to_string(li.retry_in_s) + " s)";
        list_row(c_, "Status", nullptr, st.c_str(), nullptr, li.state == app::LinkState::Ready ? color::green() : color::orange(), LV_SYMBOL_BLUETOOTH);
        if (!li.last_error.empty()) section_header(c_, li.last_error.c_str());
        if (li.has_peer) {
            list_row(c_, li.peer_name.empty() ? "Companion" : li.peer_name.c_str(), ble_addr_str(li.peer_addr).c_str(),
                     li.peer_firmware.empty() ? nullptr : li.peer_firmware.c_str());
            section_header(c_, "Reconnects automatically when in range");
            if (li.state == app::LinkState::Disconnected) {
                pill_button(c_, "Reconnect", color::blue(), [] { central::reconnect(); }, LV_PCT(100), 64);
            } else {
                pill_button(c_, "Disconnect", color::card2(), [] { central::disconnect(); }, LV_PCT(100), 64);
            }
            pill_button(c_, "Forget companion", color::red(), [] { central::forget(); }, LV_PCT(100), 64);
        } else {
            section_header(c_, "Power on the companion, then search.");
        }
        pill_button(c_, li.has_peer ? "Pair a different companion" : "Find & pair", color::blue(), [] { push(new CompanionScan()); }, LV_PCT(100), 64);
    }
};

// ---- Phone (future) --------------------------------------------------------------------

class PhoneScreen : public ListScreen {
public:
    const char* title() const override { return "Phone"; }
    void populate() override {
        // TODO(PHONE): a future iOS/Android app will act as an alternative provider for
        // internet access, location and music over BLE, speaking the same SWP protocol with
        // Role::Phone. Until then this screen only explains the limitation.
        list_row(c_, "Not available yet", "A phone companion app is planned", nullptr, nullptr, color::card2(), LV_SYMBOL_WARNING);
        section_header(c_, "Today the watch gets internet, location and music through the ESP32 companion.");
    }
};

// ---- AirPods / audio sink (via companion) ----------------------------------------------

const char* sink_state(uint8_t s) {
    switch (static_cast<swp::SinkState>(s)) {
        case swp::SinkState::Idle: return "Not connected";
        case swp::SinkState::Discovering: return "Searching";
        case swp::SinkState::Connecting: return "Connecting";
        case swp::SinkState::Connected: return "Connected";
        case swp::SinkState::Streaming: return "Playing";
        case swp::SinkState::Disconnecting: return "Disconnecting";
        default: return "Failed";
    }
}

class AirPodsScreen : public ListScreen {
public:
    const char* title() const override { return "AirPods"; }
    void refresh(uint32_t d) override {
        if (d & (app::kDirtyAudio | app::kDirtyLink)) rebuild();
    }
    void populate() override {
        swp::AudioStatus a;
        swp::AudioDevices devs;
        bool ready;
        {
            app::Locked s;
            a = s->audio;
            devs = s->audio_devices;
            ready = s->link.state == app::LinkState::Ready;
        }
        if (!ready) {
            section_header(c_, "Companion not connected");
            return;
        }
        bool conn = a.state == static_cast<uint8_t>(swp::SinkState::Connected) || a.state == static_cast<uint8_t>(swp::SinkState::Streaming);
        list_row(c_, a.name.empty() ? "Headphones" : a.name.c_str(), a.remembered ? mac_str(a.bda).c_str() : "None remembered", sink_state(a.state),
                 nullptr, conn ? color::green() : color::card2(), LV_SYMBOL_AUDIO);
        if (conn) {
            pill_button(c_, "Disconnect", color::card2(), [] { swlink::send(swp::AudioDisconnect{}); }, LV_PCT(100), 64);
        } else if (a.remembered) {
            swp::AudioConnect c;
            memcpy(c.bda, a.bda, 6);
            pill_button(c_, "Connect", color::blue(), [c] { swlink::send(c); }, LV_PCT(100), 64);
        }
        if (a.remembered) {
            swp::AudioForget f;
            memcpy(f.bda, a.bda, 6);
            pill_button(c_, "Forget", color::red(), [f] { swlink::send(f); }, LV_PCT(100), 64);
        }
        section_header(c_, devs.scanning ? "Searching... (put AirPods in pairing mode)" : "Nearby audio devices");
        for (auto& d : devs.devices) {
            swp::AudioConnect c;
            memcpy(c.bda, d.bda, 6);
            list_row(c_, d.name.c_str(), (mac_str(d.bda) + "  " + std::to_string(d.rssi) + " dBm").c_str(), nullptr, [c] {
                swlink::send(c);
                toast("Connecting...");
            }, color::blue(), LV_SYMBOL_AUDIO);
        }
        if (!devs.scanning)
            pill_button(c_, "Search for devices", color::blue(), [] {
                swp::AudioScanStart s;
                s.seconds = 10;
                swlink::send(s);
            }, LV_PCT(100), 64);
        // TODO(HARDWARE): AirPods pairing via generic SSP has not been verified on hardware.
        section_header(c_, "Hold the AirPods case button until the light flashes white.");
    }
};

// ---- Home Wi-Fi (companion) -----------------------------------------------------------

const char* net_state(uint8_t s) {
    switch (static_cast<swp::NetState>(s)) {
        case swp::NetState::Disabled: return "Disabled";
        case swp::NetState::NotConfigured: return "Not configured";
        case swp::NetState::Searching: return "Searching";
        case swp::NetState::Connecting: return "Connecting";
        case swp::NetState::Connected: return "Connected";
        default: return "Failed";
    }
}

class WifiScreen : public ListScreen {
public:
    const char* title() const override { return "Home Wi-Fi"; }
    void on_shown() override { swlink::send(swp::WifiStatusRequest{}); }
    void refresh(uint32_t d) override {
        if (d & (app::kDirtyWifi | app::kDirtyLink)) rebuild();
    }
    void populate() override {
        swp::WifiStatus w;
        bool valid, ready;
        {
            app::Locked s;
            w = s->wifi;
            valid = s->wifi_valid;
            ready = s->link.state == app::LinkState::Ready;
        }
        if (!ready) {
            section_header(c_, "Companion not connected");
            return;
        }
        if (ssid_.empty() && valid) ssid_ = w.ssid;
        std::string st = valid ? net_state(w.state) : "...";
        std::string sub;
        if (valid && w.state == static_cast<uint8_t>(swp::NetState::Connected)) {
            char b[48];
            snprintf(b, sizeof b, "%lu.%lu.%lu.%lu  %d dBm%s", (unsigned long)(w.ipv4 & 0xFF), (unsigned long)((w.ipv4 >> 8) & 0xFF),
                     (unsigned long)((w.ipv4 >> 16) & 0xFF), (unsigned long)(w.ipv4 >> 24), w.rssi, w.internet_ok ? "" : "  (server unreachable)");
            sub = b;
        }
        list_row(c_, "Status", sub.c_str(), st.c_str(), nullptr, w.state == static_cast<uint8_t>(swp::NetState::Connected) ? color::green() : color::orange(),
                 LV_SYMBOL_WIFI);
        list_row(c_, "Network", ssid_.empty() ? "Not set" : ssid_.c_str(), nullptr, [this] {
            KeyboardOpts o;
            o.label = "SSID";
            o.initial = ssid_;
            o.max_len = 32;
            open_keyboard(o, [this](const std::string& s) {
                ssid_ = s;
                rebuild();
            });
        });
        list_row(c_, "Password", pass_set_ ? "New password entered" : (w.password_set ? "Saved (hidden)" : "Not set"), nullptr, [this] {
            KeyboardOpts o;
            o.label = "Password";
            o.password = true;
            o.max_len = 63;
            open_keyboard(o, [this](const std::string& s) {
                pass_ = s;
                pass_set_ = true;
                rebuild();
            });
        });
        pill_button(c_, "Apply", color::blue(), [this] {
            if (ssid_.empty()) return toast("Enter the network name first");
            swp::WifiConfigSet c;
            c.ssid = ssid_;
            c.password = pass_;
            if (!pass_set_) return toast("Enter the password (it is never read back)");
            swlink::send(c);
            pass_.assign(pass_.size(), '\0');  // don't keep the secret around longer than needed
            pass_.clear();
            pass_set_ = false;
            toast("Sent to companion");
        }, LV_PCT(100), 64);
        section_header(c_, "Stored on the companion only. The watch itself never uses Wi-Fi.");
    }

private:
    std::string ssid_, pass_;
    bool pass_set_ = false;
};

// ---- Notifications (ntfy topics, stored on the companion) --------------------------------

class NotificationSettingsScreen : public ListScreen {
public:
    const char* title() const override { return "Notifications"; }
    void on_shown() override {
        swp::ConfigGet g;
        g.key = "ntfy_topics";
        swlink::send(g);
    }
    void refresh(uint32_t d) override {
        if (d & (app::kDirtyConfig | app::kDirtyLink | app::kDirtySettings)) rebuild();
    }
    void populate() override {
        auto st = settings::get();
        switch_row(c_, "Show ntfy notifications", st.ntfy_enabled, [](bool on) {
            auto s = settings::get();
            s.ntfy_enabled = on;
            settings::set(s);
        });
        bool ready;
        swp::ConfigValue topics;
        {
            app::Locked s;
            ready = s->link.state == app::LinkState::Ready;
            for (auto& v : s->config_values) {
                if (v.key == "ntfy_topics") {
                    topics = v;
                    v.status = 0;  // report a rejection once
                }
            }
        }
        if (!ready) {
            section_header(c_, "Companion not connected");
            return;
        }
        if (topics.status) toast("Topic rejected: use letters, digits, - and _ only");
        topics_ = split(topics.value);

        section_header(c_, "Topics");
        for (size_t i = 0; i < topics_.size(); ++i) {
            std::string t = topics_[i];
            list_row(c_, t.c_str(), nullptr, "Remove", [this, t] {
                std::vector<std::string> next;
                for (auto& x : topics_)
                    if (x != t) next.push_back(x);
                save(next);
            }, color::purple(), LV_SYMBOL_BELL);
        }
        if (topics_.empty()) section_header(c_, "No topics yet. Add one to receive its notifications.");
        if (topics_.size() < 16)
            list_row(c_, "Add topic", nullptr, nullptr, [this] {
                KeyboardOpts o;
                o.label = "ntfy topic";
                o.max_len = 64;
                open_keyboard(o, [this](const std::string& s) {
                    if (s.empty()) return;
                    for (auto& x : topics_)
                        if (x == s) return;
                    auto next = topics_;
                    next.push_back(s);
                    save(next);
                });
            }, color::card2(), LV_SYMBOL_PLUS);

        section_header(c_, "The companion keeps one connection open to ntfy (Settings > Server) and forwards new messages here.");
    }

private:
    static std::vector<std::string> split(const std::string& csv) {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= csv.size()) {
            size_t comma = csv.find(',', start);
            std::string t = csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!t.empty()) out.push_back(t);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return out;
    }
    static void set_key(const char* key, const std::string& value) {
        swp::ConfigSet c;
        c.key = key;
        c.value = value;
        swlink::send(c);  // the companion answers with ConfigValue, which refreshes this screen
    }
    static void save(const std::vector<std::string>& topics) {
        std::string csv;
        for (auto& t : topics) csv += (csv.empty() ? "" : ",") + t;
        set_key("ntfy_topics", csv);
    }
    std::vector<std::string> topics_;
};

// ---- Server (companion settings for the VPS helper, ntfy and Navidrome) ------------------

class ServerSettingsScreen : public ListScreen {
public:
    const char* title() const override { return "Server"; }
    void on_shown() override {
        for (auto& f : kFields) {
            swp::ConfigGet g;
            g.key = f.key;
            swlink::send(g);
        }
    }
    void refresh(uint32_t d) override {
        if (d & (app::kDirtyConfig | app::kDirtyLink)) rebuild();
    }
    void populate() override {
        bool ready;
        std::vector<swp::ConfigValue> vals;
        {
            app::Locked s;
            ready = s->link.state == app::LinkState::Ready;
            vals = s->config_values;
            for (auto& v : s->config_values) v.status = 0;  // report each rejection once
        }
        if (!ready) {
            section_header(c_, "Companion not connected");
            return;
        }
        const char* section = nullptr;
        for (auto& f : kFields) {
            if (!section || std::strcmp(section, f.section) != 0) section_header(c_, section = f.section);
            swp::ConfigValue v;
            for (auto& x : vals)
                if (x.key == f.key) v = x;
            if (v.status) {
                toast(std::string(f.title) + ": value rejected" + (f.https ? " (must start with https://)" : ""));
            }
            std::string sub = f.secret ? (v.is_set ? "Saved (hidden)" : "Not set") : (v.value.empty() ? "Not set" : v.value);
            const Field* fp = &f;
            std::string current = v.value;
            list_row(c_, f.title, sub.c_str(), nullptr, [fp, current] {
                KeyboardOpts o;
                o.label = fp->title;
                o.password = fp->secret;
                o.max_len = fp->max_len;
                o.initial = fp->secret ? "" : (current.empty() && fp->https ? "https://" : current);
                open_keyboard(o, [fp](const std::string& s) {
                    swp::ConfigSet c;
                    c.key = fp->key;
                    c.value = s;
                    swlink::send(c);  // the companion answers with ConfigValue, which refreshes this screen
                });
            });
        }
        section_header(c_, "Stored on the companion. Secrets can be replaced but are never read back.");
    }

private:
    struct Field {
        const char* section;
        const char* key;
        const char* title;
        bool secret;
        bool https;
        size_t max_len;
    };
    static constexpr Field kFields[] = {
        {"VPS helper (weather, maps)", "srv_url", "Helper URL", false, true, 128},
        {"VPS helper (weather, maps)", "srv_token", "Device token", true, false, 128},
        {"Notifications (ntfy)", "ntfy_url", "ntfy URL", false, true, 128},
        {"Notifications (ntfy)", "ntfy_token", "Auth token", true, false, 128},
        {"Music (Navidrome)", "nd_url", "Navidrome URL", false, true, 128},
        {"Music (Navidrome)", "nd_user", "User", false, false, 64},
        {"Music (Navidrome)", "nd_pass", "Password", true, false, 128},
        {"Music (Navidrome)", "nd_hdr", "Proxy header name", false, false, 64},
        {"Music (Navidrome)", "nd_hdr_val", "Proxy header value", true, false, 256},
        {"Music (Navidrome)", "nd_playlist", "Playlist ID", false, false, 64},
    };
};

// ---- Maps (offline tiles on the companion SD card, OSM data on the VPS helper) ----------

void confirm(const char* title, const char* body, const char* action, lv_color_t colour, Fn fn) {
    lv_obj_t* m = lv_obj_create(lv_layer_top());
    lv_obj_set_size(m, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(m, color::bg(), 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_90, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_radius(m, 0, 0);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* t = label(m, title, &font_inter_24);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_t* b = label(m, body, &font_inter_20, color::muted());
    lv_obj_set_width(b, 320);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 160);
    lv_obj_t* ok = pill_button(m, action, colour, [m, fn] {
        lv_obj_delete_async(m);
        fn();
    }, 150, 64);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, -44, -70);
    lv_obj_t* no = pill_button(m, "Cancel", color::card2(), [m] { lv_obj_delete_async(m); }, 150, 64);
    lv_obj_align(no, LV_ALIGN_BOTTOM_LEFT, 44, -70);
}

class MapsSettingsScreen : public ListScreen {
public:
    const char* title() const override { return "Maps"; }
    void on_shown() override {
        swp::MapDataCommand c;
        c.action = static_cast<uint8_t>(swp::MapDataAction::Status);
        swlink::send(c);
    }
    void refresh(uint32_t d) override {
        if (d & (app::kDirtyMapData | app::kDirtyLink | app::kDirtyPosition)) rebuild();
    }
    void populate() override {
        bool ready, have_pos, have_sync, have_data;
        swp::Position pos;
        swp::MapSyncStatus sync;
        swp::MapDataStatus data;
        {
            app::Locked s;
            ready = s->link.state == app::LinkState::Ready;
            have_pos = s->position_valid;
            pos = s->position;
            have_sync = s->map_sync_valid;
            sync = s->map_sync;
            have_data = s->map_data_valid;
            data = s->map_data;
        }
        if (!ready) {
            section_header(c_, "Companion not connected");
            return;
        }
        section_header(c_, "Offline maps on the companion");
        static const char* kSync[] = {"Not started", "Downloading", "Done", "Failed", "Map data being prepared"};
        std::string sub = have_sync ? sync.message : "Tiles within 3 km, zoom 12-17";
        std::string val = have_sync ? (std::to_string(sync.done) + " tiles, " + std::to_string(sync.kbytes / 1024) + " MB") : "";
        list_row(c_, have_sync ? kSync[sync.state % 5] : "Download maps around me", sub.c_str(), val.empty() ? nullptr : val.c_str(),
                 [have_pos, pos] {
                     if (!have_pos) return toast("Location not available yet");
                     swp::MapSyncRequest r;
                     r.lat_e7 = pos.lat_e7;
                     r.lon_e7 = pos.lon_e7;
                     swlink::send(r);
                     toast("Offline map download started");
                 },
                 color::green(), LV_SYMBOL_DOWNLOAD);

        section_header(c_, "OSM data on the server");
        std::string st = !have_data ? "..." : !data.building.empty() ? "Building " + data.building : data.active.empty() ? "None yet" : data.active;
        list_row(c_, "Current area", have_data && !data.error.empty() ? data.error.c_str() : nullptr, st.c_str(), [] {
            swp::MapDataCommand c;
            c.action = static_cast<uint8_t>(swp::MapDataAction::Status);
            swlink::send(c);
        }, color::blue(), LV_SYMBOL_GPS);
        list_row(c_, "Update map data", "Download the latest OpenStreetMap extract and rebuild this area", nullptr, [have_pos, pos] {
            if (!have_pos) return toast("Location not available yet");
            confirm("Update map data?", "The server downloads fresh OSM data and rebuilds maps and routing here. This takes a few minutes.",
                    "Update", color::blue(), [pos] {
                        swp::MapDataCommand c;
                        c.action = static_cast<uint8_t>(swp::MapDataAction::Update);
                        c.lat_e7 = pos.lat_e7;
                        c.lon_e7 = pos.lon_e7;
                        swlink::send(c);
                    });
        }, color::card2(), LV_SYMBOL_REFRESH);
        list_row(c_, "Delete map data", "Remove all downloaded and built OSM data from the server", nullptr, [] {
            confirm("Delete map data?", "Maps and routing stop working until the server downloads data again for where you are.", "Delete",
                    color::red(), [] {
                        swp::MapDataCommand c;
                        c.action = static_cast<uint8_t>(swp::MapDataAction::Delete);
                        swlink::send(c);
                    });
        }, color::red(), LV_SYMBOL_TRASH);
        section_header(c_, "Map data \xC2\xA9 OpenStreetMap contributors (ODbL).");
    }
};

class ConnectionsScreen : public ListScreen {
public:
    const char* title() const override { return "Connections"; }
    void refresh(uint32_t d) override {
        if (d & (app::kDirtyLink | app::kDirtyAudio | app::kDirtyWifi)) rebuild();
    }
    void populate() override {
        app::LinkState ls;
        swp::AudioStatus a;
        swp::WifiStatus w;
        {
            app::Locked s;
            ls = s->link.state;
            a = s->audio;
            w = s->wifi;
        }
        list_row(c_, "Companion", app::link_state_name(ls), nullptr, [] { push(new CompanionScreen()); }, color::blue(), LV_SYMBOL_BLUETOOTH);
        list_row(c_, "Phone", "Not available yet", nullptr, [] { push(new PhoneScreen()); }, color::card2(), LV_SYMBOL_CALL);
        list_row(c_, "AirPods", sink_state(a.state), nullptr, [] { push(new AirPodsScreen()); }, color::pink(), LV_SYMBOL_AUDIO);
        list_row(c_, "Home Wi-Fi", w.ssid.empty() ? net_state(w.state) : (w.ssid + " - " + net_state(w.state)).c_str(), nullptr,
                 [] { push(new WifiScreen()); }, color::green(), LV_SYMBOL_WIFI);
    }
};

// ---- Device info -----------------------------------------------------------------------

class DeviceInfoScreen : public ListScreen {
public:
    const char* title() const override { return "About"; }
    void on_shown() override { swlink::request_status(); }
    void refresh(uint32_t d) override {
        if (d & (app::kDirtyCompanion | app::kDirtyLink)) rebuild();
    }
    void populate() override {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BT);
        section_header(c_, "Watch");
        list_row(c_, "Firmware", nullptr, esp_app_get_description()->version);
        list_row(c_, "Board", board::name());
        list_row(c_, "Bluetooth", nullptr, mac_str(mac).c_str());
        list_row(c_, "Uptime", nullptr, std::to_string(esp_timer_get_time() / 60000000) .append(" min").c_str());
        list_row(c_, "Memory", nullptr, (std::to_string(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024) + " KB / " +
                                         std::to_string(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024) + " KB PSRAM").c_str());
        list_row(c_, "Steps today", nullptr, std::to_string(services::steps_today()).c_str());
        auto p = services::power();
        list_row(c_, "Power", nullptr, p.battery_present ? (std::to_string(p.percent) + "%" + (p.charging ? " charging" : "")).c_str() : "USB (no battery)");
        swp::CompanionStatus cs;
        bool valid;
        {
            app::Locked s;
            cs = s->companion;
            valid = s->companion_valid;
        }
        section_header(c_, "Companion");
        if (!valid) {
            list_row(c_, "Not connected");
            return;
        }
        static const char* sd[] = {"No card", "Mounted", "Error", "Formatting", "Unsupported"};
        static const char* sync[] = {"Idle", "Syncing", "Up to date", "Failed", "Paused", "Not configured"};
        list_row(c_, "Firmware", nullptr, cs.firmware.c_str());
        list_row(c_, "SD card", (std::to_string(cs.sd_free_mb) + " MB free of " + std::to_string(cs.sd_total_mb) + " MB").c_str(), sd[cs.sd_state % 5]);
        list_row(c_, "Storage", ("Music " + std::to_string(cs.music_mb) + " MB, maps " + std::to_string(cs.maps_mb) + " MB").c_str());
        list_row(c_, "Music sync", (std::to_string(cs.sync_done) + "/" + std::to_string(cs.sync_total) + " tracks").c_str(), sync[cs.sync_state % 6]);
        list_row(c_, "Server", nullptr, cs.server_ok ? "Reachable" : "Unreachable");
        list_row(c_, "Notifications", nullptr, (std::to_string(cs.notif_pending) + " pending").c_str());
        list_row(c_, "Uptime", nullptr, (std::to_string(cs.uptime_s / 60) + " min").c_str());
    }
};

// ---- Web configuration -------------------------------------------------------------------
// Opens the companion's LAN settings page (http://<companion-ip>/) for 5 minutes.

class WebConfigScreen : public ListScreen {
public:
    const char* title() const override { return "Web config"; }
    void refresh(uint32_t d) override {
        if (d & (app::kDirtyWebConfig | app::kDirtyLink)) rebuild();
    }
    void tick() override {
        if (open_now()) rebuild();  // countdown
    }
    void populate() override {
        bool ready, open;
        std::string url;
        {
            app::Locked s;
            ready = s->link.state == app::LinkState::Ready;
            open = s->web_config_open;
            url = s->web_config_url;
        }
        if (!ready) {
            section_header(c_, "Companion not connected");
            return;
        }
        int left = remaining_s();
        if (open && left > 0) {
            section_header(c_, url.empty() ? "Waiting for the companion's Wi-Fi address" : ("Open " + url + " in a browser on the same network").c_str());
            char t[16];
            snprintf(t, sizeof t, "%d:%02d", left / 60, left % 60);
            list_row(c_, "Closes in", nullptr, t);
            list_row(c_, "Close now", nullptr, nullptr, [] {
                swp::WebConfigCommand c;
                swlink::send(c);
            }, color::red(), LV_SYMBOL_CLOSE);
        } else {
            section_header(c_, "Lets a browser on your Wi-Fi change the companion's settings for 5 minutes. Plain HTTP: use only on a trusted network.");
            list_row(c_, "Open for 5 minutes", nullptr, nullptr, [] {
                swp::WebConfigCommand c;
                c.seconds = 300;
                swlink::send(c);
            }, color::blue(), LV_SYMBOL_PLAY);
        }
    }

private:
    static int remaining_s() {
        app::Locked s;
        int64_t left = s->web_config_until_us - esp_timer_get_time();
        return left > 0 ? static_cast<int>((left + 999999) / 1000000) : 0;
    }
    static bool open_now() {
        bool open;
        {
            app::Locked s;
            open = s->web_config_open;
        }
        return open && remaining_s() > 0;
    }
};

// ---- Settings root ---------------------------------------------------------------------

class SettingsScreen : public ListScreen {
public:
    const char* title() const override { return "Settings"; }
    void refresh(uint32_t d) override {
        if (d & (app::kDirtyWebConfig | app::kDirtySettings)) rebuild();
    }
    void populate() override {
        auto st = settings::get();
        list_row(c_, "Connections", "Companion, phone, AirPods, Wi-Fi", nullptr, [] { push(new ConnectionsScreen()); }, color::blue(), LV_SYMBOL_BLUETOOTH);
        list_row(c_, "Notifications", st.ntfy_enabled ? "ntfy topics" : "Off", nullptr, [] { push(new NotificationSettingsScreen()); },
                 color::purple(), LV_SYMBOL_BELL);
        list_row(c_, "Maps", "Offline maps, OSM data", nullptr, [] { push(new MapsSettingsScreen()); }, color::green(), LV_SYMBOL_GPS);
        list_row(c_, "Server", "Helper, ntfy, Navidrome", nullptr, [] { push(new ServerSettingsScreen()); }, color::card2(), LV_SYMBOL_SETTINGS);
        switch_row(c_, "Vibration", st.vibration_enabled, [](bool on) {
            auto s = settings::get();
            s.vibration_enabled = on;
            settings::set(s);
            if (on) board::vibrate(120);
        });
        list_row(c_, "Brightness", "Tap to change", (std::to_string(st.brightness) + "%").c_str(), [] {
            auto s = settings::get();
            s.brightness = s.brightness >= 100 ? 30 : static_cast<uint8_t>(s.brightness + 10);
            settings::set(s);
            board::set_brightness(s.brightness);
            app::mark(app::kDirtySettings);
        }, color::yellow(), LV_SYMBOL_EYE_OPEN);
        list_row(c_, "Time zone", st.posix_tz.c_str(), nullptr, [] {
            KeyboardOpts o;
            o.label = "POSIX TZ, e.g. EST5EDT,M3.2.0,M11.1.0";
            o.initial = settings::get().posix_tz;
            o.max_len = 48;
            open_keyboard(o, [](const std::string& tz) {
                auto s = settings::get();
                s.posix_tz = tz;
                settings::set(s);
                app::mark(app::kDirtyTime | app::kDirtySettings);
            });
        }, color::orange(), LV_SYMBOL_LOOP);
        bool web;
        {
            app::Locked s;
            web = s->web_config_open && s->web_config_until_us > esp_timer_get_time();
        }
        list_row(c_, "Web configuration", web ? "Open on the companion's IP" : "Companion settings in a browser", nullptr,
                 [] { push(new WebConfigScreen()); }, color::purple(), LV_SYMBOL_SETTINGS);
        list_row(c_, "About", nullptr, nullptr, [] { push(new DeviceInfoScreen()); }, color::card2(), LV_SYMBOL_LIST);
    }
};

}  // namespace

Screen* make_settings() { return new SettingsScreen(); }

}  // namespace ui
